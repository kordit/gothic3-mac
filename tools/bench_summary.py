#!/usr/bin/env python3
"""Summary of a bench log (tools/bench.sh): per 10 s window frames over 33 and 50 ms, fps, and the
slow-frame lines' max wait yield, wait cs and bg load. python3 tools/bench_summary.py <log> [skip_windows]"""
import re, statistics, sys
text = open(sys.argv[1], errors="replace").read()
skip = int(sys.argv[2]) if len(sys.argv) > 2 else 12  # the start, the F9 load
wins = re.findall(r"frames, last 10\.0 s: ([\d.]+) fps, ([\d.]+) ms per frame.*?(\d+) over 33 ms, (\d+) over 50 ms", text)[skip:]
slow = re.findall(r"slow frame ([\d.]+) ms:.*", text)
def field(name):
    return [float(x) for x in re.findall(name + r" ([\d.]+) ms", "\n".join(l for l in text.splitlines() if "slow frame" in l))]
if wins:
    fps = [float(w[0]) for w in wins]; o33 = [int(w[2]) for w in wins]; o50 = [int(w[3]) for w in wins]
    print("%s: %d okien po %d pominiętych; fps mediana %.1f (min %.1f); ponad 33 ms: mediana %d, średnio %.1f, max %d; ponad 50 ms: mediana %d, średnio %.1f, max %d"
          % (sys.argv[1], len(wins), skip, statistics.median(fps), min(fps), statistics.median(o33), statistics.mean(o33), max(o33),
             statistics.median(o50), statistics.mean(o50), max(o50)))
for n in ("wait yield", "wait cs", "bg load"):
    v = field(n)
    if v:
        print("  slow frame %-10s max %.1f ms, mediana %.1f ms (%d linii)" % (n, max(v), statistics.median(v), len(v)))
print("  crash/abort:", len(re.findall(r"SIGSEGV|SIGBUS|abort|fatal", text)))
