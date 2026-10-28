#!/usr/bin/env python3
"""Stored copies of Gothic 3 archives: the same tree, every zlib entry unpacked, for native/filesystem_core.cpp to
read instead of the original (session 22: inflating a World Texture Mix texture took 30-57 ms inside the frame).

    tools/g3stored.py <archive>...       copies the ones not done yet; progress in work/g3stored.log
    tools/g3stored.py --profile          the texture archives the player's game mounts (their game folder and the
                                         archives their profile lays over it)

A copy is ~/Library/Caches/Gothic3Recomp/stored/<key>.g3s, where key is FNV-1a 64 of the archive's real path, size
and modification time in nanoseconds, each followed by a NUL (filesystem_core.cpp computes the same). A changed
archive has another key, so its old copy is just never opened. The copy keeps the original's header and tree byte for
byte except each zlib file's offset, stored size and compression (fixed-width fields, patched in place) and the
header's tree offsets; encrypted entries and stored ones are copied as they are. Every unpacked entry must have its
recorded size (zlib checks its adler32). One archive is one part: written to <key>.g3s.part, renamed when complete,
so an interrupted run redoes only the archive it was in."""

import os
from pathlib import Path
import struct
import sys
import time
import zlib

CACHE = Path.home() / "Library/Caches/Gothic3Recomp/stored"
LOG = Path(__file__).resolve().parent.parent / "work/g3stored.log"
PROFILE = Path.home() / "Library/Application Support/Gothic 3 Native"


def key(path):
    real = os.path.realpath(path)
    st = os.stat(real)
    h = 0xcbf29ce484222325
    for part in (real.encode(), str(st.st_size).encode(), str(st.st_mtime_ns).encode()):
        for b in part + b"\0":
            h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return "%016x" % h


def log(text):
    LOG.parent.mkdir(parents=True, exist_ok=True)
    with open(LOG, "a") as f:
        f.write("%s %s\n" % (time.strftime("%H:%M:%S"), text))
    print(text, flush=True)


def files(tree, at):
    """Every file record of the tree from `at`: (offset of its offset field, offset, stored, size, encryption,
    compression)."""
    out = []

    def name():
        nonlocal at
        n = struct.unpack_from("<I", tree, at)[0]
        at += 4 + (n + 1 if n else 0)

    def node():
        nonlocal at
        at += 32
        attributes = struct.unpack_from("<I", tree, at)[0]
        at += 4
        if attributes & 0x10:
            name()
            for _ in range(2):
                count = struct.unpack_from("<I", tree, at)[0]
                at += 4
                for _ in range(count):
                    node()
            return
        out.append((at,) + struct.unpack_from("<QQQII", tree, at))
        at += 32
        name()
        name()

    node()
    return out


def copy(src):
    dst = CACHE / (key(src) + ".g3s")
    if dst.exists():
        return False
    real = os.path.realpath(src)
    with open(real, "rb") as f:
        head = bytearray(f.read(0x30))
        if head[4:8] != b"G3V0" or struct.unpack_from("<I", head, 12)[0] == 1:
            raise SystemExit("%s: not a plain Gothic 3 archive" % src)
        tree_at, _, end = struct.unpack_from("<QQQ", head, 24)
        f.seek(tree_at)
        tree = bytearray(f.read(end - tree_at))
        tail = f.read()  # the comment after the tree
        records = files(tree, 0)
        part = dst.with_suffix(".g3s.part")
        CACHE.mkdir(parents=True, exist_ok=True)
        t0, done, unpacked = time.time(), 0, 0
        with open(part, "wb") as out:
            out.write(head)
            for field, offset, stored, size, encryption, compression in sorted(records, key=lambda r: r[1]):
                f.seek(offset)
                raw = f.read(stored)
                if compression == 2 and encryption == 0:
                    raw = zlib.decompress(raw)
                    if len(raw) != size:
                        raise SystemExit("%s: an entry unpacks to %d bytes, not %d" % (src, len(raw), size))
                    compression = 0
                    unpacked += 1
                struct.pack_into("<QQQII", tree, field, out.tell(), len(raw), size, encryption, compression)
                out.write(raw)
                done += 1
                if done % 200 == 0:
                    log("%s: %d of %d entries" % (Path(src).name, done, len(records)))
            new_tree = out.tell()
            out.write(tree)
            new_end = out.tell()
            out.write(tail)
            struct.pack_into("<QQQ", head, 24, new_tree, new_tree, new_end)
            out.seek(0)
            out.write(head)
            out.flush()
            os.fsync(out.fileno())
        os.replace(part, dst)
    log("%s: done, %d entries (%d unpacked), %.0f MB, %.1f s -> %s" %
        (Path(src).name, len(records), unpacked, dst.stat().st_size / 1048576, time.time() - t0, dst.name))
    return True


def profile_archives():
    seen = []
    for folder in (PROFILE / "game/Data", PROFILE / "Data"):
        for p in sorted(folder.glob("*")):
            if p.name.lower().startswith("_compiledimage.") and p.is_file():
                seen.append(p)
    return seen


def main(argv):
    archives = profile_archives() if argv == ["--profile"] else [Path(a) for a in argv]
    if not archives:
        print(__doc__)
        return 2
    todo = [a for a in archives if not (CACHE / (key(a) + ".g3s")).exists()]
    log("%d archives, %d to copy" % (len(archives), len(todo)))
    for i, a in enumerate(todo):
        log("%d of %d: %s" % (i + 1, len(todo), a))
        copy(a)
    log("all done")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
