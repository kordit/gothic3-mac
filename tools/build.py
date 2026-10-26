#!/usr/bin/env python3
"""Build Gothic 3: kit/tools/build.py with --game-dir set to this repository.

The process becomes the kit's build (exec), so its PID is the one to kill."""

import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
os.execv(sys.executable, [sys.executable, str(ROOT / "kit/tools/build.py"), "--game-dir", str(ROOT)] + sys.argv[1:])
