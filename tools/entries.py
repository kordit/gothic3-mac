#!/usr/bin/env python3
"""Analyse and translate modules until no jump reaches code outside the listing.

    tools/entries.py <module key> [...]     # game.toml [modules.aux.<key>]

Ghidra leaves some code outside every function it lists: MSVC's cold chunks
and the case bodies behind jump tables it could not bound (MSVCRT's memmove
reaches 110608bd by a JNZ, and its tail tables name blocks no function
holds). The translator refuses both. Each round runs the translator and
collects

  * every "dispatches to X, which is not an entry point" target, and
  * for every "jump table at S ... N of M entries have no block entry", all M
    slots of the table, read from the module itself (the translator names
    only the first few),

keeps the ones capstone decodes as code that do not start a listed function
(MSVCRT's memcpy and memmove share their tail blocks, so a table's target can
lie inside another function; a function entry is what both can jump to),
appends them to entries/<module>.txt and re-runs tools/analyze.py for that
module, whose AddEntries.java makes them functions. It stops when the module
translates or a round finds nothing new."""

import os
import re
from pathlib import Path
import subprocess
import sys
import tomllib

import capstone
import pefile

ROOT = Path(__file__).resolve().parents[1]
DANGLING = re.compile(r"dispatches to ([0-9a-f]{8}), which is not an entry point")
TABLE = re.compile(r"jump table at ([0-9a-f]{8}) \(in fn_([0-9a-f]+)\): (\d+) of (\d+) entries have no block entry, "
                   r"first((?: [0-9a-f]{8})+)")
ROUNDS = 12
GHIDRA = os.environ.get("GHIDRA_HOME", str(Path.home() / "tools/ghidra_12.1.3_PUBLIC"))
JAVA = os.environ.get("JAVA_HOME", "/opt/homebrew/opt/openjdk@21")


class Module:
    def __init__(self, path):
        self.pe = pefile.PE(str(path), fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.image = self.pe.get_memory_mapped_image()
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.code = [(self.base + s.VirtualAddress, self.base + s.VirtualAddress + s.Misc_VirtualSize)
                     for s in self.pe.sections if s.Characteristics & 0x20000000]

    def is_code(self, a):
        return any(lo <= a < hi for lo, hi in self.code)

    def decodes(self, a):
        if not self.is_code(a):
            return False
        return len(list(self.md.disasm(self.image[a - self.base:a - self.base + 32], a, 3))) == 3

    def dword(self, a):
        return int.from_bytes(self.image[a - self.base:a - self.base + 4], "little")

    def table_targets(self, site, count, before):
        """The `count` slots of the table a `jmp [reg*4 + disp]` at `site` reads.
        `before` is the listing line ahead of the jump: after a NEG of the index
        (MSVCRT's memmove tails) the index runs from -(count-1) to 0, so the
        table lies below disp."""
        ins = next(self.md.disasm(self.image[site - self.base:site - self.base + 16], site, 1), None)
        if ins is None or ins.mnemonic != "jmp" or len(ins.operands) != 1:
            return []
        op = ins.operands[0]
        if op.type != capstone.x86.X86_OP_MEM or op.mem.scale != 4 or op.mem.base != 0:
            return []
        table = op.mem.disp & 0xFFFFFFFF
        step = -4 if before.split()[1:2] == ["NEG"] else 4
        return [self.dword(table + step * i) for i in range(count)]


def function_starts(listings):
    rows = (listings / "functions.tsv").read_text().splitlines()[1:]
    return {int(row.split("\t")[0], 16) for row in rows if row}


def main():
    keys = sys.argv[1:]
    if not keys:
        sys.exit(__doc__)
    cfg = tomllib.loads((ROOT / "game.toml").read_text())
    for key in keys:
        entry = cfg["modules"]["aux"][key]
        module = Module(ROOT / entry["path"])
        listings = ROOT / entry.get("listings", "analysis/" + entry["name"])
        extra = ROOT / "entries" / (entry["name"] + ".txt")
        for round_ in range(ROUNDS):
            command = [sys.executable, str(ROOT / "kit/tools/recomp/translate.py"), "--out",
                       str(ROOT / "work/tr" / ("aux-" + key)), "--game", str(ROOT), "--module", key]
            result = subprocess.run(command, capture_output=True, text=True)
            out = result.stdout + result.stderr
            if result.returncode == 0:
                line = next((l for l in out.splitlines() if l.startswith("translated")), "translated")
                print("%-12s %s" % (key, line.split(" in ")[0]), flush=True)
                break
            wanted = {int(a, 16) for a in DANGLING.findall(out)}
            for site, fn, _, count, first in TABLE.findall(out):
                wanted |= {int(a, 16) for a in first.split()}
                lines = (listings / "functions" / (fn + ".asm")).read_text().splitlines()
                at = next((i for i, l in enumerate(lines) if l.startswith(site)), 0)
                before = lines[at - 1] if at else ""
                wanted |= set(module.table_targets(int(site, 16), int(count), before))
            listed = function_starts(listings)
            known = set()
            if extra.is_file():
                known = {int(l.split("#")[0], 16) for l in extra.read_text().splitlines() if l.split("#")[0].strip()}
            dead = set()
            for site, slot in entry.get("dead_table_entries", []):
                dead |= set(module.table_targets(site, slot + 1, "")[slot:slot + 1])
            new = sorted(a for a in wanted - listed - known - dead if module.decodes(a))
            if not new:
                print("%-12s stuck after %d rounds; last translator output:" % (key, round_))
                print("\n".join(out.splitlines()[-12:]))
                break
            extra.parent.mkdir(exist_ok=True)
            with extra.open("a") as fh:
                for a in new:
                    fh.write("%08x  # round %d\n" % (a, round_ + 1))
            print("%-12s round %d: %d new entries, re-analysing" % (key, round_ + 1, len(new)), flush=True)
            subprocess.run([sys.executable, str(ROOT / "tools/analyze.py"), "--ghidra-home", GHIDRA,
                            "--java-home", JAVA, "--max-memory", "4G", "--only", entry["name"]],
                           check=True, stdout=subprocess.DEVNULL)
        else:
            print("%-12s gave up after %d rounds" % (key, ROUNDS))


if __name__ == "__main__":
    main()
