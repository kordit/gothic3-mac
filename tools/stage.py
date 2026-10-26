#!/usr/bin/env python3
"""Prepare the player's own Gothic 3 modules for analysis and translation.

    tools/stage.py --install /path/to/Gothic 3 --steamless ~/tools/steamless

Reads stage.toml, checks every source file against its pinned SHA-256, then
writes stage/<name> for each module:

  steamless  removes the SteamStub wrapper (Steamless CLI under Mono)
  upx        unpacks a UPX-packed DLL (upx -d)
  cab:NAME   extracts NAME from a DirectX cabinet (bsdtar)
  base       rebases the module there using its own .reloc table, so the kit
             maps every module at its preferred base without a relocating
             loader; each relocated dword is checked against the original

A module lands at stage/<dest> (default: its name), and every other file of
the install is linked into stage/, so stage/ is a complete game directory the
kit runs from. stage/manifest.json records the output hashes. Nothing is
written into the install itself.

A [[data]] entry names an archive (7-Zip, extracted with 7zz) whose files
replace install files at their `dest`: each is checked against its own pin
and the install file it replaces against `replaces`, then copied into stage/
instead of linked (Waste Data Patch 2.0, D-013)."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import tomllib

import pefile

ROOT = Path(__file__).resolve().parents[1]
GUEST_LIMIT = 0x7FFFF000


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(cmd, **kw):
    result = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if result.returncode:
        sys.exit("%s failed:\n%s%s" % (cmd[0], result.stdout, result.stderr))
    return result.stdout


def prepare(kind, src, work, steamless):
    """Return the path of the module's code before any rebasing."""
    if not kind:
        return src
    if kind == "steamless":
        if not steamless:
            sys.exit("Gothic3.exe needs --steamless (the extracted Steamless 3.1 directory)")
        copy = work / src.name
        shutil.copy2(src, copy)
        env = dict(os.environ, MONO_PATH=str(steamless / "Plugins"))
        run(["mono", str(steamless / "Steamless.CLI.exe"), "--quiet", str(copy)], cwd=steamless, env=env)
        out = copy.with_name(copy.name + ".unpacked.exe")
        if not out.is_file():
            sys.exit("Steamless did not unpack %s" % src.name)
        return out
    if kind == "upx":
        out = work / (src.stem + ".unpacked" + src.suffix)
        run(["upx", "-q", "-d", "-o", str(out), str(src)])
        return out
    if kind.startswith("cab:"):
        member = kind[4:]
        run(["tar", "-xf", str(src), "-C", str(work), member])
        return work / member
    sys.exit("unknown prepare step %r" % kind)


def relocations(pe):
    """(rva, size) of every HIGHLOW fixup; other types refuse."""
    fixups = []
    for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
        for entry in block.entries:
            if entry.type == pefile.RELOCATION_TYPE["IMAGE_REL_BASED_ABSOLUTE"]:
                continue
            if entry.type != pefile.RELOCATION_TYPE["IMAGE_REL_BASED_HIGHLOW"]:
                sys.exit("relocation type %d at %#x is not supported" % (entry.type, entry.rva))
            fixups.append(entry.rva)
    return fixups


def rebase(path, base, out):
    """Patch the file bytes directly: pefile's own write() re-serialises parsed
    structures and undid two fixups in ge3dialogs.dll's .data."""
    before = pefile.PE(str(path))
    old = before.OPTIONAL_HEADER.ImageBase
    fixups = relocations(before)
    if not fixups:
        sys.exit("%s has no relocations to rebase with" % path.name)
    data = bytearray(Path(path).read_bytes())
    delta = (base - old) & 0xFFFFFFFF
    for rva in fixups:
        at = before.get_offset_from_rva(rva)
        if at is None or at + 4 > len(data):
            sys.exit("%s: fixup at rva %#x has no file bytes" % (path.name, rva))
        value = int.from_bytes(data[at:at + 4], "little")
        data[at:at + 4] = ((value + delta) & 0xFFFFFFFF).to_bytes(4, "little")
    image_base = before.OPTIONAL_HEADER.get_field_absolute_offset("ImageBase")
    data[image_base:image_base + 4] = base.to_bytes(4, "little")
    Path(out).write_bytes(data)
    check = pefile.PE(str(out))
    delta = (base - old) & 0xFFFFFFFF
    for rva in fixups:
        want = (before.get_dword_at_rva(rva) + delta) & 0xFFFFFFFF
        if check.get_dword_at_rva(rva) != want:
            sys.exit("%s: fixup at rva %#x was not applied" % (out.name, rva))
    if check.OPTIONAL_HEADER.ImageBase != base:
        sys.exit("%s: image base was not rewritten" % out.name)
    return len(fixups)


def source_of(m, install):
    """A module's source file: in the install, or under `root` (relative to
    the repository) for a file the install does not carry."""
    return (ROOT / m["root"] if "root" in m else install) / m["src"]


def stage_data(entry, install, out):
    """Extract a [[data]] archive's files over their install counterparts."""
    archive = ROOT / entry["archive"]
    if not archive.is_file():
        sys.exit("%s is missing" % archive)
    if sha256(archive) != entry["archive_sha256"]:
        sys.exit("%s: SHA-256 is not the pinned %s" % (archive, entry["archive_sha256"]))
    for f in entry["files"]:
        replaced = install / f["dest"]
        if not replaced.is_file() or sha256(replaced) != f["replaces"]:
            sys.exit("%s is not the file %s replaces (%s)" % (replaced, f["src"], f["replaces"]))
    staged = set()
    with tempfile.TemporaryDirectory() as tmp:
        run(["7zz", "x", "-y", "-o" + tmp, str(archive)] + [f["src"] for f in entry["files"]])
        for f in entry["files"]:
            src, dst = Path(tmp) / f["src"], out / f["dest"]
            if sha256(src) != f["sha256"]:
                sys.exit("%s in %s: SHA-256 is not the pinned %s" % (f["src"], archive, f["sha256"]))
            # An earlier run linked the whole install directory (stage/Data ->
            # install/Data); writing through that link would overwrite the
            # install. Make each directory on the way a real one, and
            # link_install links the rest of its entries.
            at = out
            for part in Path(f["dest"]).parent.parts:
                at = at / part
                if at.is_symlink():
                    at.unlink()
                at.mkdir(exist_ok=True)
            if not dst.parent.resolve().is_relative_to(out.resolve()):
                sys.exit("%s would land outside %s" % (dst, out))
            if dst.is_symlink():
                dst.unlink()
            shutil.move(src, dst)
            staged.add(dst.resolve())
            print("%-26s from %s" % (f["dest"], archive.name))
    return staged


def link_install(install, out, staged):
    """Symlink every install entry the staging did not produce, descending
    only into directories that hold a staged module."""
    count = 0
    for src in sorted(install.iterdir()):
        dst = out / src.name
        if dst.resolve() in staged or src.name == "steamapps":
            continue
        if src.is_dir() and any(str(p).startswith(str(dst.resolve()) + os.sep) for p in staged):
            dst.mkdir(exist_ok=True)
            count += link_install(src, dst, staged)
            continue
        if dst.is_symlink() or dst.exists():
            if dst.is_symlink() and dst.resolve() == src.resolve():
                continue
            if dst.is_symlink():
                dst.unlink()
            else:
                sys.exit("%s exists and is not a link to the install" % dst)
        dst.symlink_to(src)
        count += 1
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--install", type=Path, required=True, help="the Gothic 3 install directory")
    parser.add_argument("--steamless", type=Path, help="the extracted Steamless 3.1 directory")
    parser.add_argument("--out", type=Path, default=ROOT / "stage")
    args = parser.parse_args()
    cfg = tomllib.loads((ROOT / "stage.toml").read_text())
    install = args.install.expanduser().resolve()
    steamless = args.steamless.expanduser().resolve() if args.steamless else None
    args.out.mkdir(parents=True, exist_ok=True)

    for m in cfg["module"]:
        src = source_of(m, install)
        if not src.is_file():
            sys.exit("%s is missing from %s" % (m["src"], src.parent))
        digest = sha256(src)
        if digest != m["sha256"]:
            sys.exit("%s: SHA-256 %s is not the supported Steam build (%s)" % (m["src"], digest, m["sha256"]))

    manifest, spans = [], []
    with tempfile.TemporaryDirectory() as tmp:
        for m in cfg["module"]:
            work = Path(tmp) / m["name"]
            work.mkdir()
            code = prepare(m.get("prepare"), source_of(m, install), work, steamless)
            out = args.out / m.get("dest", m["name"])
            out.parent.mkdir(parents=True, exist_ok=True)
            fixups = 0
            if "base" in m:
                fixups = rebase(code, m["base"], out)
            else:
                shutil.copy2(code, out)
            pe = pefile.PE(str(out), fast_load=True)
            base, size = pe.OPTIONAL_HEADER.ImageBase, pe.OPTIONAL_HEADER.SizeOfImage
            if base % 0x10000 or base + size > GUEST_LIMIT:
                sys.exit("%s: %#x+%#x does not fit the guest layout" % (m["name"], base, size))
            spans.append((base, base + size, m["name"]))
            manifest.append({"name": m["name"], "path": out.relative_to(args.out).as_posix(), "base": base,
                             "size": size, "sha256": sha256(out), "fixups": fixups})
            print("%-18s %#010x-%#010x %7d fixups  %s" % (m["name"], base, base + size, fixups, manifest[-1]["sha256"]))

    spans.sort()
    for (a0, a1, an), (b0, b1, bn) in zip(spans, spans[1:]):
        if b0 < a1:
            sys.exit("%s overlaps %s" % (an, bn))
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    staged = {(args.out / m["path"]).resolve() for m in manifest}
    for entry in cfg.get("data", []):
        staged |= stage_data(entry, install, args.out)
    links = link_install(install, args.out, staged)
    print("Linked %d install entries into %s" % (links, args.out))
    print("Staged %d modules into %s" % (len(manifest), args.out))


if __name__ == "__main__":
    main()
