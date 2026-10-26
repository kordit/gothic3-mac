#!/usr/bin/env python3
# tools/bench_sum.py <label>... : frame times (ms) from work/bench-<label>.log, 10 s windows in order.
# A window under 20 ms before a spike over 200 ms is the menu; the scene is everything after the
# last window over 200 ms (loading). Prints median, min, max and the windows of the scene.
import re, sys, statistics
for label in sys.argv[1:]:
    ms = [float(m.group(1)) for m in re.finditer(r'frames, last [\d.]+ s: [\d.]+ fps, ([\d.]+) ms per frame', open(f'work/bench-{label}.log').read())]
    if not ms:
        print(label, 'no frames'); continue
    last = max([i for i, v in enumerate(ms) if v > 200] or [-1])
    scene = ms[last + 1:] if last >= 0 else ms[2:]
    if len(scene) > 1 and scene[0] > 1.4 * statistics.median(scene):
        scene = scene[1:]  # the first window after loading still settles
    print(f'{label}: median {statistics.median(scene):.1f} min {min(scene):.1f} max {max(scene):.1f} ms, {len(scene)} windows:',
          ' '.join(f'{v:.1f}' for v in scene))
