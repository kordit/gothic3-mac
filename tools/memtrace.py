#!/usr/bin/env python3
"""tools/memtrace.py <memtrace.txt> <syms.txt> [--top N]: what a frame's logic and render touch (D-054 step 1).

The input is native/phases.c's MEMTRACE= report (kit/runtime/memtrace.cpp): for each traced iteration a "logic N"
and a "render N" section, each page the code touched (R read, W written) with its first toucher, and the written
byte runs with their heap block. syms.txt is `nm -n` of the binary that ran (tools/bench.sh writes it).

Reported for each iteration:
  - render: pages read, bytes written; its writes split into pages the next logic reads (render -> logic, D-054
    pt 3) and the rest, each by the guest function that stored (the first fn_ frame) and by heap block;
  - the snapshot's size: bytes logic wrote in pages render then read (an upper bound at page grain), and the heap
    blocks they are in.
"""
import argparse
import bisect
import collections
import re
import sys

PAGE_DEFAULT = 16384
NM_BASE = 0x100000000  # unslid load address of an arm64 macOS executable


def modules():
    """(base, name) of every module in game.toml, sorted by base."""
    import tomllib
    import pathlib
    cfg = tomllib.loads((pathlib.Path(__file__).resolve().parent.parent / "game.toml").read_text())
    mods = [(cfg["game"]["image_base"], "exe")]
    for key, m in cfg.get("modules", {}).get("aux", {}).items():
        mods.append((m["base"], m["name"].rsplit(".", 1)[0]))
    return sorted(mods)


MODULES = modules()


def module_of(name):
    m = re.match(r"(?:fn|g3n|body)_([0-9a-f]{8})$", name)
    if not m:
        return "host"
    a = int(m.group(1), 16)
    i = bisect.bisect_right([b for b, _ in MODULES], a) - 1
    return MODULES[i][1] if i >= 0 else "?"


class Images:
    """The staged modules (stage/, at the bases the game maps them): MSVC RTTI names of vtables, function names from
    analysis/<module>/functions.tsv."""

    def __init__(self):
        import pathlib
        import pefile
        root = pathlib.Path(__file__).resolve().parent.parent
        self.pes = []
        for f in sorted((root / "stage").iterdir()):
            if f.suffix.lower() in (".dll", ".exe"):
                try:
                    pe = pefile.PE(str(f), fast_load=True)
                except pefile.PEFormatError:
                    continue
                self.pes.append((pe.OPTIONAL_HEADER.ImageBase, pe.OPTIONAL_HEADER.SizeOfImage, pe))
        self.cache = {}
        self.fnames = {}
        for tsv in (root / "analysis").glob("*/functions.tsv"):
            for line in open(tsv, errors="replace"):
                parts = line.split("\t")
                if len(parts) > 1 and re.fullmatch(r"[0-9a-f]{8}", parts[0]):
                    self.fnames[int(parts[0], 16)] = parts[1]

    def _pe(self, va):
        for base, size, pe in self.pes:
            if base <= va < base + size:
                return base, pe
        return None, None

    def dword(self, va):
        base, pe = self._pe(va)
        return pe.get_dword_at_rva(va - base) if pe else None

    def cstr(self, va):
        base, pe = self._pe(va)
        return pe.get_string_at_rva(va - base, 256).decode("latin-1") if pe else None

    def class_of(self, vtable):
        if vtable in self.cache:
            return self.cache[vtable]
        name = None
        try:
            col = self.dword(vtable - 4)
            td = self.dword(col + 12) if col else None
            raw = self.cstr(td + 8) if td else None
            if raw and raw.startswith((".?AV", ".?AU")) and raw.endswith("@@"):
                name = "::".join(reversed(raw[4:-2].split("@")))
        except Exception:
            name = None
        self.cache[vtable] = name
        return name

    def fn_name(self, name):
        m = re.match(r"(?:fn|g3n|body)_([0-9a-f]{8})$", name)
        n = self.fnames.get(int(m.group(1), 16)) if m else None
        return f"{name} {n}" if n and not n.startswith("FUN_") else name


class Syms:
    def __init__(self, path):
        self.addrs, self.names = [], []
        for line in open(path, errors="replace"):
            parts = line.split()
            if len(parts) != 3 or parts[1] not in "tT":
                continue
            self.addrs.append(int(parts[0], 16))
            self.names.append(parts[2].lstrip("_"))

    def name(self, rel):
        i = bisect.bisect_right(self.addrs, NM_BASE + rel) - 1
        return self.names[i] if i >= 0 else "?"

    def guest_fn(self, touch):
        """The first translated or native game function from the store outwards; else the innermost name."""
        names = [self.name(touch["pc"])] + [self.name(f) for f in touch["frames"]]
        for n in names:
            if n.startswith(("fn_", "g3n_")):
                return n
        return names[0]


TOUCH = re.compile(r" (r|w) t=([0-9a-f]+)(\*?)(?: eip=([0-9a-f]+) esp=([0-9a-f]+))? pc=([0-9a-f]+) lr=([0-9a-f]+) f=([0-9a-f,]*)")


def parse(path):
    sections = []
    cur = None
    for line in open(path, errors="replace"):
        if line.startswith("memtrace "):
            label = line.split(" pages=")[0][len("memtrace "):]
            page = int(re.search(r" page=(\d+)", line).group(1)) if " page=" in line else PAGE_DEFAULT
            cur = {"label": label, "page": page, "pages": {}, "runs": [], "vtables": {}}
            sections.append(cur)
        elif cur is None:
            continue
        elif line[:2] in ("R ", "W "):
            addr = int(line.split()[1], 16)
            touches = {}
            for m in TOUCH.finditer(line):
                touches[m.group(1)] = {
                    "thread": m.group(2), "self": bool(m.group(3)),
                    "eip": int(m.group(4), 16) if m.group(4) else None,
                    "pc": int(m.group(6), 16), "lr": int(m.group(7), 16),
                    "frames": [int(x, 16) for x in m.group(8).split(",") if x],
                }
            cur["pages"][addr] = {"kind": line[0], **touches}
            cur["last"] = addr
        elif line.startswith("  b "):
            parts = line.split()
            run = {"addr": int(parts[1], 16), "len": int(parts[2], 16), "page": cur["last"], "blk": None}
            for p in parts[3:]:
                if p.startswith("blk="):
                    base, size = p[4:].replace("(free)", "").split("+")
                    run["blk"] = (int(base, 16), int(size, 16), "(free)" in p)
            cur["runs"].append(run)
        elif line.startswith("V "):
            _, blk, dw = line.split()
            cur["vtables"][int(blk, 16)] = int(dw, 16)
        elif line.startswith("end "):
            cur = None
    return sections


def fmt_bytes(n):
    return f"{n / 1048576:.2f} MB" if n >= 1 << 20 else f"{n / 1024:.1f} KB" if n >= 1024 else f"{n} B"


IMAGES = None


def block_class(sec, r):
    if not r["blk"]:
        return "(outside the heap)"
    vt = sec["vtables"].get(r["blk"][0])
    cls = IMAGES.class_of(vt) if vt is not None else None
    return cls or f"(block {r['blk'][1]:#x} B)"


def by_writer(runs, sec, syms, top):
    agg = collections.Counter()
    for r in runs:
        w = sec["pages"][r["page"]].get("w")
        name = syms.guest_fn(w) if w else "?"
        agg[f"{IMAGES.fn_name(name)} [{module_of(name)}] -> {block_class(sec, r)}"] += r["len"]
    return agg.most_common(top)


def by_class(runs, sec, top):
    agg = collections.Counter()
    for r in runs:
        agg[block_class(sec, r)] += r["len"]
    return agg.most_common(top)


def own(sec, runs):
    """Runs in pages the game thread (the one that began the trace) wrote first; pages another thread wrote first
    are that thread's (fmodex's mix buffers, the kit's renderer), though a later store by the game thread in the
    same page is counted with them."""
    mine = [r for r in runs if sec["pages"][r["page"]]["w"]["self"]]
    return mine, [r for r in runs if not sec["pages"][r["page"]]["w"]["self"]]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("syms")
    ap.add_argument("--top", type=int, default=15)
    a = ap.parse_args()
    global IMAGES
    IMAGES = Images()
    syms = Syms(a.syms)
    sections = parse(a.trace)
    by_label = {s["label"]: s for s in sections}
    for s in sections:
        if not s["label"].startswith("render "):
            continue
        n = int(s["label"].split()[1])
        logic, nxt = by_label.get(f"logic {n}"), by_label.get(f"logic {n + 1}")
        page = s["page"]
        mine_pages = [p for p, v in s["pages"].items() if v["r"]["self"]]
        read_only = [p for p in mine_pages if s["pages"][p]["kind"] == "R"]
        runs, others = own(s, s["runs"])
        written = sum(r["len"] for r in runs)
        print(f"== {s['label']}: the game thread touched {len(mine_pages)} pages ({fmt_bytes(len(mine_pages) * page)}),"
              f" {len(read_only)} only read; wrote {fmt_bytes(written)} in {len(runs)} runs."
              f" Other threads: {len(s['pages']) - len(mine_pages)} pages, {fmt_bytes(sum(r['len'] for r in others))}"
              f" written")
        for name, b in by_writer(others, s, syms, 4):
            print(f"    other {fmt_bytes(b):>10}  {name}")
        if nxt:
            to_logic = [r for r in runs if r["page"] in nxt["pages"]]
            rest = [r for r in runs if r["page"] not in nxt["pages"]]
            print(f"  writes to pages logic {n + 1} reads: {fmt_bytes(sum(r['len'] for r in to_logic))}"
                  f" in {len(to_logic)} runs; the rest {fmt_bytes(sum(r['len'] for r in rest))}")
            for name, b in by_writer(to_logic, s, syms, a.top):
                print(f"    {fmt_bytes(b):>10}  {name}")
        else:
            print(f"  (no logic {n + 1} traced: render's writes not split)")
            for name, b in by_writer(runs, s, syms, a.top):
                print(f"    {fmt_bytes(b):>10}  {name}")
        if logic:
            shared = [r for r in own(logic, logic["runs"])[0] if r["page"] in s["pages"]
                      and s["pages"][r["page"]]["r"]["self"]]
            blocks = collections.Counter()
            for r in shared:
                blocks[r["blk"][:2] if r["blk"] else None] += r["len"]
            heap = sum(v for k, v in blocks.items() if k)
            print(f"  logic {n} wrote {fmt_bytes(sum(r['len'] for r in logic['runs']))}; in pages render {n} read: "
                  f"{fmt_bytes(sum(r['len'] for r in shared))} ({fmt_bytes(heap)} in {len(blocks) - (None in blocks)}"
                  f" heap blocks, {fmt_bytes(blocks.get(None, 0))} outside the heap)")
            for name, b in by_writer(shared, logic, syms, a.top):
                print(f"    {fmt_bytes(b):>10}  {name}")
            print("   by class:")
            for name, b in by_class(shared, logic, a.top):
                print(f"    {fmt_bytes(b):>10}  {name}")


if __name__ == "__main__":
    sys.exit(main())
