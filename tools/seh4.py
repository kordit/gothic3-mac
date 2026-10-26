#!/usr/bin/env python3
"""List MSVC SEH4 filters and handlers that no translation reaches.

    tools/seh4.py <module key | exe> [...]     # game.toml [modules.aux.<key>]

MSVC's __SEH_prolog4 frames name their filters, __except bodies and
__finally blocks only in a scope table: _except_handler4 jumps to them and
_local_unwind4 calls them, so no instruction in the image does. Ghidra
leaves them outside every function and the translator's data-pointer scan
finds some of them, not all (CP 1.75.14 Engine.dll: 36 of 85; CallCatchBlock's
__except at 10568ad3 is one). One that is missing runs in the kit's
interpreter, if at all. This reads every table pushed ahead of a call to
__SEH_prolog4 (found by its body) and prints the scope entries whose
addresses are not in the module's work/tr/*/symbols.json."""

import json
from pathlib import Path
import struct
import sys
import tomllib

import pefile

ROOT = Path(__file__).resolve().parents[1]
# __SEH_prolog4 after its PUSH of _except_handler4: push fs:[0]; mov eax,[esp+16];
# mov [esp+16],ebp; lea ebp,[esp+16]; sub esp,eax
PROLOG4 = bytes.fromhex("64ff35000000008b442410896c24108d6c24102be0")


def scan(path, symbols):
    pe = pefile.PE(str(path), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    image = pe.get_memory_mapped_image()
    code = [(base + s.VirtualAddress, base + s.VirtualAddress + s.Misc_VirtualSize)
            for s in pe.sections if s.Characteristics & 0x20000000]

    def is_code(a):
        return any(lo <= a < hi for lo, hi in code)

    at = image.find(PROLOG4)
    if at < 5 or image[at - 5] != 0x68:
        return None, 0, []
    prolog = base + at - 5
    # PUSH table / CALL __SEH_prolog4
    tables = set()
    for lo, hi in code:
        text = image[lo - base:hi - base]
        i = text.find(b"\x68")
        while i != -1:
            if i + 10 <= len(text) and text[i + 5] == 0xE8:
                target = (lo + i + 10 + struct.unpack_from("<i", text, i + 6)[0]) & 0xFFFFFFFF
                if target == prolog:
                    tables.add(struct.unpack_from("<I", text, i + 1)[0])
            i = text.find(b"\x68", i + 1)
    have = {int(f["addr"], 16) for f in json.loads(symbols.read_text())["functions"]}
    total, missing = 0, []
    for table in sorted(tables):
        # 16 bytes of cookie offsets, then {enclosing level, filter, handler} per level
        for level in range(64):
            enclosing, filter_, handler = struct.unpack_from("<iII", image, table - base + 16 + 12 * level)
            if not is_code(handler) or not (filter_ == 0 or is_code(filter_)) or not (enclosing == -2 or 0 <= enclosing < level):
                break
            for a, kind in ((filter_, "filter"), (handler, "finally" if filter_ == 0 else "except")):
                if a:
                    total += 1
                    if a not in have:
                        missing.append((table, kind, a))
    return prolog, total, missing


def main():
    keys = sys.argv[1:]
    if not keys:
        sys.exit(__doc__)
    cfg = tomllib.loads((ROOT / "game.toml").read_text())
    for key in keys:
        if key == "exe":
            path, out = ROOT / cfg["game"]["developer_exe"], ROOT / "work/tr/exe"
        else:
            path, out = ROOT / cfg["modules"]["aux"][key]["path"], ROOT / "work/tr" / ("aux-" + key)
        if not (out / "symbols.json").is_file():
            print("%-12s not translated yet (%s)" % (key, out / "symbols.json"))
            continue
        prolog, total, missing = scan(path, out / "symbols.json")
        if prolog is None:
            print("%-12s no __SEH_prolog4" % key)
            continue
        print("%-12s __SEH_prolog4 %08x: %d of %d scope-table addresses not translated"
              % (key, prolog, len(missing), total))
        for table, kind, a in missing:
            print("    table %08x  %-7s %08x" % (table, kind, a))


if __name__ == "__main__":
    main()
