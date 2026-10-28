"""Reference scenes (tools/bench.sh sceny): each scene's noise and, given a second run, how far it moved.

python3 tools/sceny_diff.py work/bench-<przed> [work/bench-<po>] [--png]

For every smoke_<name>_a_present.ppm: its mean brightness (a black frame is a failed scene), the noise between its
a and b frames (1 s apart in a frozen world: particles, flames, whatever still moves), and with a second run the
difference of before a to after a. A scene changed when that difference is clearly above its noise: the share of
pixels off by more than 24 levels, against the same share between a and b. --png writes <dir>/<name>.png (and
<name>-diff.png with two runs, the differing pixels in red) for looking at them."""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

THRESHOLD = 24


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def off(x, y):
    """The share of pixels whose largest channel difference is over THRESHOLD, and the mean difference."""
    d = np.abs(x - y).max(axis=2)
    return float((d > THRESHOLD).mean()), float(d.mean())


def main(argv):
    png = "--png" in argv
    dirs = [Path(a) for a in argv if a != "--png"]
    if not dirs or len(dirs) > 2:
        print(__doc__)
        return 2
    before = dirs[0]
    after = dirs[1] if len(dirs) == 2 else None
    names = sorted(p.name[len("smoke_"):-len("_a_present.ppm")] for p in before.glob("smoke_*_a_present.ppm"))
    if not names:
        print("no scenes in %s" % before)
        return 1
    print("%-10s %6s %14s %16s" % ("scene", "bright", "noise a/b", "before/after"))
    changed = 0
    for name in names:
        a = load(before / ("smoke_%s_a_present.ppm" % name))
        b = load(before / ("smoke_%s_b_present.ppm" % name))
        noise, _ = off(a, b)
        line = "%-10s %6.1f %13.2f%%" % (name, a.mean(), 100 * noise)
        if png:
            Image.fromarray(a.astype(np.uint8)).save(before / ("%s.png" % name))
        if after:
            path = after / ("smoke_%s_a_present.ppm" % name)
            if not path.exists():
                line += "   missing"
            else:
                c = load(path)
                share, mean = off(a, c)
                moved = share > max(2 * noise, 0.001)
                changed += moved
                line += " %14.2f%% %s" % (100 * share, "CHANGED" if moved else "same")
                if png:
                    Image.fromarray(c.astype(np.uint8)).save(after / ("%s.png" % name))
                    mark = c.astype(np.uint8).copy()
                    mark[np.abs(a - c).max(axis=2) > THRESHOLD] = (255, 0, 0)
                    Image.fromarray(mark).save(after / ("%s-diff.png" % name))
        if a.mean() < 8:
            line += "   BLACK"
        print(line)
    if after:
        print("%d of %d scenes changed" % (changed, len(names)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
