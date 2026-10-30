#!/usr/bin/env python3
"""tools/memwatch.py <memtrace.txt> <syms.txt> [--top N]: the render's writes the next logic really reads, to the byte
(D-054 step 3, milestone 7.11).

The input is a MEMTRACE= report made by the read watch build (build-watch, RECOMP_READ_WATCH) with
RECOMP_MEMTRACE_WATCH=1 (kit/runtime/memtrace.cpp): after each "render N" section, whose written byte runs the kit
kept, a "watch logic N+1" section lists every place the next logic loaded one of those bytes ("H <pc> <addr> <len>
<count> <other thread>"). Each place is put to the render's run it hit and that run's page's storing function, and
reported by reader and writer.
"""
import argparse
import bisect
import collections
import re

import memtrace


def parse_watch(path):
    """The watch sections, in file order, each with the label of the render section just before it."""
    out, cur, last_render = [], None, None
    for line in open(path, errors="replace"):
        if line.startswith("memtrace "):
            label = line.split(" pages=")[0][len("memtrace "):]
            if label.startswith("render "):
                last_render = label
        elif line.startswith("watch "):
            label = line.split(" places=")[0][len("watch "):]
            image = int(re.search(r" image=([0-9a-f]+)", line).group(1), 16)
            cur = {"label": label, "render": last_render, "image": image, "hits": []}
            out.append(cur)
        elif cur is not None and line.startswith("H "):
            _, pc, addr, ln, count, other = line.split()
            cur["hits"].append({"pc": int(pc, 16), "addr": int(addr, 16), "len": int(ln, 16), "count": int(count),
                                "other": other == "1"})
        elif cur is not None and line.startswith("end "):
            cur = None
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("syms")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--skip", action="append", default=[],
                    help="<lo>-<hi> (hex): leave out loads in that range, e.g. the main thread's guest stack")
    a = ap.parse_args()
    memtrace.IMAGES = memtrace.Images()
    syms = memtrace.Syms(a.syms)
    sections = {s["label"]: s for s in memtrace.parse(a.trace)}
    dumps = memtrace.load_pages(a.trace + ".pages")
    for label, sec in sections.items():
        sec["dump"] = dumps.get(label, {})
    for w in parse_watch(a.trace):
        render = sections.get(w["render"])
        if not render:
            print(f"== {w['label']}: no render section before it")
            continue
        runs = sorted(render["runs"], key=lambda r: r["addr"])
        starts = [r["addr"] for r in runs]
        pairs = collections.Counter()
        places = collections.Counter()
        samples = {}
        skip = [tuple(int(x, 16) for x in r.split("-")) for r in a.skip]
        w["hits"] = [h for h in w["hits"] if not any(lo <= h["addr"] < hi for lo, hi in skip)]
        for h in w["hits"]:
            i = bisect.bisect_right(starts, h["addr"] + h["len"] - 1) - 1
            run = runs[i] if i >= 0 and runs[i]["addr"] + runs[i]["len"] > h["addr"] else None
            reader = memtrace.IMAGES.fn_name(syms.name(h["pc"] - w["image"]))
            if run:
                page = render["pages"][run["page"]]
                writer = memtrace.IMAGES.fn_name(syms.guest_fn(page["w"])) if page.get("w") else "?"
                where = memtrace.block_class(render, run)
            else:
                writer, where = "?", "?"
            key = f"{reader}{' (other thread)' if h['other'] else ''}  <-  {writer}  in {where}"
            pairs[key] += h["count"]
            places[key] += 1
            samples.setdefault(key, h["addr"])
        total = sum(h["count"] for h in w["hits"])
        print(f"== {w['label']} (after {w['render']}): {len(w['hits'])} places, {total} loads of bytes the render "
              f"changed")
        for key, n in pairs.most_common(a.top):
            print(f"  {n:>8} loads, {places[key]:>4} places  {key}  (e.g. {samples[key]:x})")


if __name__ == "__main__":
    main()
