#!/usr/bin/env python3
"""Unattended smoke test: start the port, let it run, keep the evidence, give a verdict.

    tools/smoke.py                       # world mode (NoMenu), 240 s, label from the time
    tools/smoke.py --seconds 180 --label after-rep-fastpath
    tools/smoke.py --autostart newgame --seconds 420

Starts the game through tools/run.py (its autostart modes), dumps the presented frame every
--every seconds (RECOMP_FRAME_DUMP), samples the process memory, and stops it after --seconds
(SIGTERM, then SIGKILL). Everything goes to work/smoke-<label>/: run.log (the game's stdout and
stderr), frames/frame-<s>.ppm and the last one as last.png, mem.tsv (resident memory and CPU
seconds used), result.json with the second of the first frame after the start-up one.

In NoMenu the game may load the whole world without presenting a frame, as the original does
under Wine (black until about 90 s): a run with no frame in the first two minutes is not yet a
failure, which is why the default is 240 s.

The verdict (exit status 0 = pass, 1 = fail):
- the game did not exit or crash before --seconds (an exit with code 0 counts as a pass only
  with --allow-exit);
- no fatal pattern in the log (an abort from the runtime, unmodelled SSE, an unknown call
  target, the guest heap running out);
- the last frame is not black (at least --min-lit of its pixels brighter than 16).
It does not say that the world looks right: hand last.png to visual-check for that."""

import argparse
import json
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run  # noqa: E402

ROOT = run.ROOT
FATAL = re.compile(r"an abort from the runtime|unmodelled SSE|call to unknown target|out of guest heap|"
                   r"guest process exited with code [1-9]|Segmentation fault|SIGSEGV|SIGBUS")


def read_ppm(path):
    data = path.read_bytes()
    parts = data.split(maxsplit=4)
    if len(parts) < 5 or parts[0] != b"P6":
        return None
    w, h = int(parts[1]), int(parts[2])
    return w, h, parts[4][: w * h * 3]


def lit_fraction(rgb):
    n = len(rgb) // 3
    if not n:
        return 0.0
    step = max(1, n // 20000)
    lit = sum(1 for i in range(0, n, step) if max(rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2]) > 16)
    return lit / len(range(0, n, step))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--autostart", choices=run.MODES, default="world")
    ap.add_argument("--seconds", type=int, default=240)
    ap.add_argument("--every", type=float, default=10.0, help="seconds between frame dumps")
    ap.add_argument("--label", default=time.strftime("%m%d-%H%M%S"))
    ap.add_argument("--min-lit", type=float, default=0.05)
    ap.add_argument("--allow-exit", action="store_true")
    args = ap.parse_args()

    if subprocess.run(["pgrep", "-x", "Gothic3Recomp"], capture_output=True).returncode == 0:
        sys.exit("smoke: Gothic3Recomp is already running; one game at a time")
    out = ROOT / "work" / ("smoke-" + args.label)
    frames = out / "frames"
    frames.mkdir(parents=True, exist_ok=True)
    env = run.environment(args.autostart, {
        "RECOMP_FRAME_DUMP": str(frames),
        "RECOMP_FRAME_DUMP_EVERY": str(args.every),
    })
    env.setdefault("RECOMP_LOG", "0")
    log = open(out / "run.log", "wb")
    start = time.time()
    proc = subprocess.Popen([str(run.APP)], env=env, stdout=log, stderr=subprocess.STDOUT,
                            start_new_session=True)
    (out / "pid").write_text(str(proc.pid))
    mem = open(out / "mem.tsv", "w")
    mem.write("t\trss_mb\tcpu_s\n")
    peak = 0
    fatal = None
    while proc.poll() is None and time.time() - start < args.seconds:
        time.sleep(3)
        t = int(time.time() - start)
        ps = subprocess.run(["ps", "-o", "rss=,time=", "-p", str(proc.pid)], capture_output=True,
                            text=True).stdout.split()
        if len(ps) == 2:
            rss = int(ps[0]) // 1024
            peak = max(peak, rss)
            cpu = sum(float(x) * 60 ** i for i, x in enumerate(reversed(ps[1].split(":"))))
            mem.write("%d\t%d\t%.1f\n" % (t, rss, cpu))
            mem.flush()
        text = (out / "run.log").read_bytes().decode("utf-8", "replace")
        m = FATAL.search(text)
        if m:
            fatal = m.group(0)
            break
    elapsed = round(time.time() - start, 1)
    code = proc.poll()
    if code is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        stopped = "by the test after %.0f s" % elapsed
    else:
        stopped = "by itself after %.0f s with code %d" % (elapsed, code)
    log.close()
    mem.close()

    dumps = sorted(frames.glob("frame-*.ppm"), key=lambda p: float(re.sub(r"[^0-9.]", "", p.stem) or 0))
    last, lit = None, 0.0
    if dumps:
        img = read_ppm(dumps[-1])
        if img:
            w, h, rgb = img
            lit = lit_fraction(rgb)
            try:
                from PIL import Image
                Image.frombytes("RGB", (w, h), rgb).save(out / "last.png")
                last = str(out / "last.png")
            except Exception:  # noqa: BLE001 - the verdict does not need the PNG
                last = str(dumps[-1])
    seconds_of = lambda p: float(re.sub(r"[^0-9.]", "", p.stem) or 0)  # noqa: E731
    later = [seconds_of(p) for p in dumps if seconds_of(p) > 0]
    first_frame = min(later) if later else None
    reasons = []
    if code is not None and not (code == 0 and args.allow_exit):
        reasons.append("the game exited %s" % stopped)
    if fatal:
        reasons.append("log: %s" % fatal)
    if not dumps:
        reasons.append("no frame was dumped")
    elif first_frame is None:
        reasons.append("no frame after the start-up one")
    elif lit < args.min_lit:
        reasons.append("the last frame is black (%.1f%% lit)" % (100 * lit))
    result = {
        "label": args.label, "autostart": args.autostart, "seconds": args.seconds, "stopped": stopped,
        "exit_code": code, "fatal": fatal, "peak_rss_mb": peak, "frames": len(dumps), "first_frame_s": first_frame,
        "last_frame": last, "last_frame_lit": round(lit, 3), "pass": not reasons, "reasons": reasons,
        "log": str(out / "run.log"),
    }
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    sys.exit(0 if not reasons else 1)


if __name__ == "__main__":
    main()
