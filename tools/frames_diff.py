#!/usr/bin/env python3
"""tools/frames_diff.py <a/frames.hash> <b/frames.hash>: where two runs' pictures part (RECOMP_D3D9_FRAME_HASH lines,
"<present> <w>x<h> <hash>"). Prints how many frames both have, how many agree, the first that differs and the runs
of differing frames; exit 0 when every common frame agrees."""
import sys


def load(path):
    out = []
    for line in open(path):
        parts = line.split()
        if len(parts) == 3:
            out.append((parts[1], parts[2]))
    return out


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    n = min(len(a), len(b))
    diff = [i for i in range(n) if a[i] != b[i]]
    print(f"frames: {len(a)} and {len(b)}, {n} in common, {n - len(diff)} agree")
    if not diff:
        return 0
    runs, start = [], diff[0]
    for prev, cur in zip(diff, diff[1:] + [None]):
        if cur != prev + 1:
            runs.append((start, prev))
            start = cur
    print(f"first difference at frame {diff[0] + 1}; {len(runs)} runs of differing frames:")
    for s, e in runs[:20]:
        print(f"  {s + 1}-{e + 1} ({e - s + 1})")
    return 1


if __name__ == "__main__":
    sys.exit(main())
