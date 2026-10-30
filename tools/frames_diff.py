#!/usr/bin/env python3
"""tools/frames_diff.py <a/frames.hash> <b/frames.hash> [--tolerance N]: where two runs' pictures part
(RECOMP_D3D9_FRAME_HASH lines, "<present> <w>x<h> <hash> <8x8 tile hashes> <8x8 tile channel sums> t=<game clock>").
Also prints the first frame whose game clock differs. Prints how many
frames both have, how many agree, the first that differs and the runs of differing frames.

--tolerance N (D-055): a frame whose differing tiles each moved their channel sum by at most N agrees within the
tolerance (a few pixels one level off on the GPU, as rain's edges in the cave scene); such frames are counted apart,
and the longest stretch of consecutive frames agreeing within it is printed. Exit 0 when every common frame agrees
(within the tolerance when one is given)."""
import argparse
import sys


def load(path):
    out = []
    for line in open(path):
        parts = line.split()
        if len(parts) >= 3:
            try:
                sums = [int(x, 16) for x in parts[4].split(",")] if len(parts) > 4 and "," in parts[4] else []
                clock = next((int(p[2:]) for p in parts[4:] if p.startswith("t=")), None)
            except ValueError:
                break  # the last line of a run killed while it wrote
            out.append((parts[1], parts[2], parts[3] if len(parts) > 3 else "", sums, clock))
    return out


def tiles(x, y):
    """The 8x8 grid's tiles that differ, as a picture of 8 rows ('#' differs)."""
    if len(x) != 256 or len(y) != 256:
        return ""
    return "/".join("".join("#" if x[(r * 8 + c) * 4:(r * 8 + c + 1) * 4] != y[(r * 8 + c) * 4:(r * 8 + c + 1) * 4]
                            else "." for c in range(8)) for r in range(8))


def sum_moves(fa, fb):
    """Per differing tile, how far its channel sum moved; None when a line has no sums."""
    if len(fa[3]) != 64 or len(fb[3]) != 64 or len(fa[2]) != 256 or len(fb[2]) != 256:
        return None
    return [abs(fa[3][t] - fb[3][t]) for t in range(64) if fa[2][t * 4:t * 4 + 4] != fb[2][t * 4:t * 4 + 4]]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--tolerance", type=int, default=None)
    args = ap.parse_args()
    a, b = load(args.a), load(args.b)
    n = min(len(a), len(b))
    diff = [i for i in range(n) if a[i][:2] != b[i][:2]]
    print(f"frames: {len(a)} and {len(b)}, {n} in common, {n - len(diff)} agree")
    clock = next((i for i in range(n) if a[i][4] is not None and b[i][4] is not None and a[i][4] != b[i][4]), None)
    if clock is not None:
        print(f"game clock parts at frame {clock + 1}: {a[clock][4]} and {b[clock][4]} ms"
              f" (frame {clock}: {a[clock - 1][4] if clock else '-'} and {b[clock - 1][4] if clock else '-'})")
    within = set()
    if args.tolerance is not None:
        for i in diff:
            moves = sum_moves(a[i], b[i])
            if moves is not None and max(moves, default=0) <= args.tolerance:
                within.add(i)
        longest = cur = 0
        for i in range(n):
            cur = cur + 1 if (i not in diff or i in within) else 0
            longest = max(longest, cur)
        worst = max((max(sum_moves(a[i], b[i]) or [0]) for i in within), default=0)
        print(f"within tolerance {args.tolerance}: {len(within)} more frames (largest tile sum move {worst}); "
              f"{len(diff) - len(within)} differ beyond it; longest stretch agreeing within it: {longest} frames")
    if not diff:
        return 0
    runs, start = [], diff[0]
    for prev, cur in zip(diff, diff[1:] + [None]):
        if cur != prev + 1:
            runs.append((start, prev))
            start = cur
    print(f"first difference at frame {diff[0] + 1}; {len(runs)} runs of differing frames:")
    for s, e in runs[:20]:
        moves = sum_moves(a[s], b[s])
        note = f" sums moved {moves}" if moves is not None else ""
        print(f"  {s + 1}-{e + 1} ({e - s + 1}) {tiles(a[s][2], b[s][2])}{note}")
    return 0 if args.tolerance is not None and len(within) == len(diff) else 1


if __name__ == "__main__":
    sys.exit(main())
