#!/usr/bin/env python3
"""Merge recorded Metal shaders into build/shader-warm-seed, which the app ships in Contents/Resources.

Every profile records the MSL libraries and pipelines it compiled (<profile>/shader-warm: <key>.msl, order.txt,
pipelines.txt). The kit compiles a seed's entries in the background at start-up, so a player's first torch or spell
does not compile in a frame (session 15: 100-440 ms per library). The seed only grows: the existing seed is read
first, then the profiles, so work/ can be cleaned without losing what it recorded.

A file is taken only when its name is the SHA-256 of "<identity>\\n<source>" (the kit's key; a stale or damaged
file would never match a draw), and a pipeline only when both its libraries are in the seed.

  .venv/bin/python tools/shader_seed.py [extra shader-warm dirs...]
"""

import hashlib
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SEED = ROOT / "build/shader-warm-seed"
# d3d9_metal.mm: "msl-v1", plus "-safe" on macOS 15 and later (every Mac this port is played on so far).
IDENTITY = b"msl-v1-safe\n"
LIMIT = 8192  # ShaderWarm reads at most this many entries of each list


def sources(extra):
    dirs = [SEED, Path.home() / "Library/Application Support/Gothic 3 Native/shader-warm",
            ROOT / "build/recomp/profile/shader-warm"]
    dirs += sorted((ROOT / "work").glob("**/shader-warm"))
    dirs += [Path(d) for d in extra]
    seen = set()
    for d in dirs:
        if d.is_dir() and (d / "order.txt").is_file() and d.resolve() not in seen:
            seen.add(d.resolve())
            yield d


def main():
    tmp = SEED.with_name("shader-warm-seed.tmp")
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir(parents=True)
    order, pipes, bad = [], [], 0
    keys, pipe_seen = set(), set()
    for d in sources(sys.argv[1:]):
        before = len(order)
        for line in (d / "order.txt").read_text().split():
            if line in keys or len(order) >= LIMIT:
                continue
            f = d / (line + ".msl")
            if not f.is_file():
                continue
            src = f.read_bytes()
            if hashlib.sha256(IDENTITY + src).hexdigest() != line:
                bad += 1
                continue
            (tmp / f.name).write_bytes(src)
            keys.add(line)
            order.append(line)
        if (d / "pipelines.txt").is_file():
            for spec in (d / "pipelines.txt").read_text().splitlines():
                parts = spec.split()
                if (len(parts) > 2 and parts[1] in keys and parts[2] in keys and spec not in pipe_seen
                        and len(pipes) < LIMIT):
                    pipe_seen.add(spec)
                    pipes.append(spec)
        print("%5d new from %s" % (len(order) - before, d))
    (tmp / "order.txt").write_text("".join(k + "\n" for k in order))
    (tmp / "pipelines.txt").write_text("".join(p + "\n" for p in pipes))
    shutil.rmtree(SEED, ignore_errors=True)
    tmp.rename(SEED)
    print("%s: %d libraries, %d pipelines (%d files left out: not keyed by their source)"
          % (SEED.relative_to(ROOT), len(order), len(pipes), bad))


if __name__ == "__main__":
    main()
