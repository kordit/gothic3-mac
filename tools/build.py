#!/usr/bin/env python3
"""Build Gothic 3: kit/tools/build.py with --game-dir set to this repository.

Without --target it builds both binaries from the same tree, one after the other: the app the player plays
(build/Gothic3Recomp.app) and pop_smoke, which the benches run without a window (build/recomp/pop_smoke). Building
only one let the benches measure an older build than the game (session 19). --target <t> builds just that one."""

import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
KIT = [sys.executable, str(ROOT / "kit/tools/build.py"), "--game-dir", str(ROOT)]
args = sys.argv[1:]
if "--target" in args or any(a.startswith("--target=") for a in args) or "-h" in args or "--help" in args:
    os.execv(sys.executable, KIT + args)
for target in ("app", "smoke"):
    if subprocess.call(KIT + args + ["--target", target]) != 0:
        sys.exit(1)
