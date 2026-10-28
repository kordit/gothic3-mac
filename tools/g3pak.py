#!/usr/bin/env python3
"""List Gothic 3 .pak/.cpt/.pNN archives, compare them entry by entry, pack a folder into one.

    tools/g3pak.py list <archive>
    tools/g3pak.py diff <archive A> <archive B>
    tools/g3pak.py pack <folder> <archive>
    tools/g3pak.py index <game folder> <extended 0|1>
    tools/g3pak.py selftest

The layout (after UniLinkIF/gothic3-impexp core/src/volume.rs, checked on
CP 1.75.14's Templates.pak): a 48-byte header (u32 0, "G3V0", u32 0, u32 0,
u32 compression, u32 packer tag, u64 tree offset twice, u64 end of tree),
file data from 0x30, then a tree of nodes. Each node is three FILETIMEs,
u64 0 and u32 attributes; a directory (attribute 0x10) has its name, its
subdirectories and then its files, each list behind a u32 count; a file has
u64 offset, u64 stored size, u64 size, u32 encryption, u32 compression
(2 zlib, 0 stored), its path and the path it was packed from. A name is a u32
length, the bytes and a NUL, or a bare u32 0 when empty.

diff compares the unpacked bytes, so an archive repacked without compression
(Waste Data Patch 2.0, G3CU) matches the compressed original where nothing
changed.

pack writes what G3PakDir.exe --no-comments writes (Sefaris Mod Manager's
packer, checked on G3_Mods/Static/Projects_compiled.m0x and the mods' .m0x):
header compression 1 and tag "G3PD", files stored (compression 0) from 0x30,
directories named by their full path with a trailing "/", attributes 0x20010
for a directory and 0x20020 for a file, no "packed from" path, and a u32 0
after the tree (the empty comment). Entries go in case-insensitive name order,
subdirectories before files, as in those archives. A deleted entry (attributes
0x28000, offset and sizes 0) hides the file of that name in lower archives:
Static/Projects_compiled.m0x is three of them, compiledinfos_G3_StartUp.bin,
compiledinfos_G3_World_01.bin and stringtable.bin. Whether the engine then
rebuilds those caches is not checked here."""

import hashlib
import mmap
import os
from pathlib import Path
import struct
import sys
import zlib


class Pak:
    def __init__(self, path):
        self.path = Path(path)
        with open(self.path, "rb") as f:
            self.data = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) if os.path.getsize(self.path) else b""
        if self.data[4:8] != b"G3V0":
            raise SystemExit("%s: not a Gothic 3 archive" % path)
        self.at = struct.unpack_from("<Q", self.data, 32)[0]
        self.files = {}
        self.attributes = {}
        self.dirs = set()
        self.node()

    def u32(self):
        self.at += 4
        return struct.unpack_from("<I", self.data, self.at - 4)[0]

    def u64(self):
        self.at += 8
        return struct.unpack_from("<Q", self.data, self.at - 8)[0]

    def name(self):
        n = self.u32()
        if not n:
            return ""
        self.at += n + 1
        return self.data[self.at - n - 1:self.at - 1].decode("latin-1")

    def node(self):
        self.at += 32
        attributes = self.u32()
        if attributes & 0x10:
            self.dirs.add(self.name())
            for _ in range(self.u32()):
                self.node()
            for _ in range(self.u32()):
                self.node()
            return
        offset, stored, size = self.u64(), self.u64(), self.u64()
        encryption, compression = self.u32(), self.u32()
        path = self.name()
        self.name()
        self.files[path.lower()] = (path, offset, stored, size, encryption, compression)
        self.attributes[path.lower()] = attributes

    def read(self, key):
        path, offset, stored, size, encryption, compression = self.files[key]
        raw = self.data[offset:offset + stored]
        if encryption:
            raise SystemExit("%s: %s is encrypted" % (self.path, path))
        out = zlib.decompress(raw) if compression == 2 else raw
        if len(out) != size:
            raise SystemExit("%s: %s unpacks to %d bytes, not %d" % (self.path, path, len(out), size))
        return out

    def digest(self, key):
        return hashlib.sha256(self.read(key)).hexdigest()


DIR_ATTRIBUTES = 0x20010
FILE_ATTRIBUTES = 0x20020
DELETED_ATTRIBUTES = 0x28000


def name_bytes(name):
    if not name:
        return struct.pack("<I", 0)
    raw = name.encode("latin-1")
    return struct.pack("<I", len(raw)) + raw + b"\0"


def write(out, entries, filetime=0):
    """Pack entries {"dir/name.ext": bytes, or None for a deleted entry} into the archive `out`.
    filetime (a Windows FILETIME) goes on every directory and file; give the newest input's so equal
    inputs give equal bytes."""
    tree = {}
    for path, data in entries.items():
        node = tree
        parts = path.replace("\\", "/").split("/")
        for d in parts[:-1]:
            node = node.setdefault(d + "/", {})
        node[parts[-1]] = None if data is None else bytes(data)
    body, nodes = bytearray(), bytearray()
    times = struct.pack("<QQQQ", filetime, filetime, filetime, 0)

    def emit(node, prefix):
        nodes.extend(times + struct.pack("<I", DIR_ATTRIBUTES) + name_bytes(prefix))
        subdirs = sorted((k for k in node if k.endswith("/")), key=str.lower)
        files = sorted((k for k in node if not k.endswith("/")), key=str.lower)
        nodes.extend(struct.pack("<I", len(subdirs)))
        for k in subdirs:
            emit(node[k], prefix + k)
        nodes.extend(struct.pack("<I", len(files)))
        for k in files:
            data = node[k]
            if data is None:
                nodes.extend(times + struct.pack("<IQQQII", DELETED_ATTRIBUTES, 0, 0, 0, 0, 0))
            else:
                offset = 0x30 + len(body)
                body.extend(data)
                nodes.extend(times + struct.pack("<I", FILE_ATTRIBUTES))
                nodes.extend(struct.pack("<QQQII", offset, len(data), len(data), 0, 0))
            nodes.extend(name_bytes(prefix + k) + name_bytes(""))

    emit(tree, "")
    tree_at = 0x30 + len(body)
    header = struct.pack("<I4sQI4sQQQ", 0, b"G3V0", 0, 1, b"G3PD", tree_at, tree_at, tree_at + len(nodes))
    Path(out).write_bytes(header + bytes(body) + bytes(nodes) + struct.pack("<I", 0))


def selftest():
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        # Sefaris' Static/Projects_compiled.m0x: three deleted entries, byte for byte but the
        # root's times (the file's own, which write() puts on every node).
        out = Path(tmp) / "a.m0x"
        write(out, {"compiledinfos_G3_StartUp.bin": None, "compiledinfos_G3_World_01.bin": None,
                    "stringtable.bin": None})
        got = out.read_bytes()
        static = (
            "000000004733563000000000000000000100000047335044300000000000000030000000000000008f01000000000000"
            + "00" * 24 + "000000000000000010000200000000000000000003000000"
            + "".join("00" * 32 + "00800200" + "00" * 32 + struct.pack("<I", len(n)).hex()
                      + n.encode().hex() + "00" + "00000000"
                      for n in ("compiledinfos_G3_StartUp.bin", "compiledinfos_G3_World_01.bin", "stringtable.bin"))
            + "00000000")
        assert got == bytes.fromhex(static), got.hex()
        back = Pak(out)
        assert sorted(back.files) == ["compiledinfos_g3_startup.bin", "compiledinfos_g3_world_01.bin",
                                      "stringtable.bin"], back.files
        # Nested folders and data round-trip through the reader.
        out = Path(tmp) / "b.mod"
        want = {"G3_World_01/G3_World_01.wrldatasc": b"[Sector.List]\r\n", "stringtable.ini": "﻿x".encode("utf-16-le"),
                "G3_World_01/Sub/z.bin": bytes(range(256))}
        write(out, want, 0x01d9003030f3a10b)
        back = Pak(out)
        for path, data in want.items():
            assert back.read(path.lower()) == data, path
        assert back.files["g3_world_01/g3_world_01.wrldatasc"][0] == "G3_World_01/G3_World_01.wrldatasc"
    print("g3pak selftest: ok")


def index(root, extended):
    """The lines native/tests/filesystem_test.cpp's index prints, from the archives and the disk alone."""
    fold = str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz")  # the module folds A-Z only
    root = Path(root)
    lines = set()
    section = None
    for raw in (root / "Ini" / "mountlist.ini").read_text("latin-1").splitlines():
        line = raw.strip("\r")
        if not line or line.startswith(";"):
            continue
        if line.startswith("["):
            section = line.lower()
            continue
        if section != "[list]" or "=" not in line:
            continue
        virtual, physical = line.split("=", 1)
        if "projects" in virtual.lower():
            physical = "Data/Projects_compiled"
        base = root / physical
        paks = []
        for first, letter, on in ((".pak", "p", True), (".cpt", "c", extended), (".mod", "m", True),
                                  (".nod", "n", extended)):
            if not on or not Path(str(base) + first).is_file():
                continue
            paks.append(Path(str(base) + first))
            i = 0
            while Path(str(base) + ".%s%02d" % (letter, i)).is_file():
                paks.append(Path(str(base) + ".%s%02d" % (letter, i)))
                i += 1
        opened = []
        for p in paks:
            pak = Pak(p)
            files = {v[0].translate(fold): (v[3], pak.attributes[k]) for k, v in pak.files.items()}
            opened.append((pak.path.name, files, {d.translate(fold) for d in pak.dirs}))
        on_disk = {}
        if base.is_dir():
            for d, _, names in os.walk(base):
                for n in names:
                    full = Path(d) / n
                    on_disk[full.relative_to(base).as_posix().translate(fold)] = full
        prefix = ("#G3:/" + virtual + "/").translate(fold)
        for key, full in on_disk.items():
            writable = os.access(full, os.W_OK)
            lines.add("%s%s\t%d\t%#x\tdisk" % (prefix, key, full.stat().st_size, 0x80 if writable else 0x81))
        for i, (name, files, dirs) in enumerate(opened):
            for key, (size, attributes) in files.items():
                if key in on_disk or any(key in q[1] or key + "/" in q[2] for q in opened[i + 1:]):
                    continue
                lines.add("%s%s\t%d\t%#x\t%s" % (prefix, key, size, attributes, name))
    out = sys.stdout.buffer
    for line in sorted(lines, key=lambda t: t.encode("latin-1")):
        out.write(line.encode("latin-1") + b"\n")


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "index":
        index(sys.argv[2], sys.argv[3] == "1")
        return
    if sys.argv[1:] == ["selftest"]:
        selftest()
        return
    if len(sys.argv) == 4 and sys.argv[1] == "pack":
        root = Path(sys.argv[2])
        files = [p for p in sorted(root.rglob("*")) if p.is_file()]
        newest = max((p.stat().st_mtime_ns for p in files), default=0)
        write(sys.argv[3], {p.relative_to(root).as_posix(): p.read_bytes() for p in files},
              newest // 100 + 116444736000000000)
        return
    if len(sys.argv) == 3 and sys.argv[1] == "list":
        pak = Pak(sys.argv[2])
        for key in sorted(pak.files):
            path, _, stored, size, _, compression = pak.files[key]
            print("%10d %10d %d  %s" % (size, stored, compression, path))
        return
    if len(sys.argv) != 4 or sys.argv[1] != "diff":
        sys.exit(__doc__)
    a, b = Pak(sys.argv[2]), Pak(sys.argv[3])
    only_a = sorted(set(a.files) - set(b.files))
    only_b = sorted(set(b.files) - set(a.files))
    changed = [k for k in sorted(set(a.files) & set(b.files)) if a.digest(k) != b.digest(k)]
    print("%s: %d entries, %s: %d entries; %d only in the first, %d only in the second, %d changed, %d same"
          % (a.path, len(a.files), b.path, len(b.files), len(only_a), len(only_b), len(changed),
             len(set(a.files) & set(b.files)) - len(changed)))
    for label, keys, pak in (("-", only_a, a), ("+", only_b, b), ("~", changed, b)):
        for k in keys:
            print("%s %s" % (label, pak.files[k][0]))


if __name__ == "__main__":
    main()
