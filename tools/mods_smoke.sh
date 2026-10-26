#!/bin/sh
# tools/mods_smoke.sh <label> <out-dir> <mod-folder|contract.json|none> [seconds]: the port without
# a window (pop_smoke, Metal offscreen, no audio device) in NoMenu world mode on the profile
# <out-dir>/profile, with one Sefaris mod folder linked into it by tools/g3mods.py, a whole
# active-mods.json (several mods, a set), or none (the same profile, unmodded).
#
# Writes <out-dir>/profile/active-mods.json the way the launcher does, prepares the profile like
# tools/run.py's autostart profiles (ini/ge3.ini with NoMenu=true, options and Shader.Cache from the
# player's), builds the view, then runs with RECOMP_TRACE_FILES=1 so the log shows every open.
# Evidence in <out-dir>: <label>.log (game output), <label>.prep (the view: links made, files left
# out), <label>-frames/frame-<s>.ppm (RECOMP_FRAME_DUMP every 20 s), <label>.pid. Stopped by PID
# after <seconds> (default 240). G3_ROOT names the checkout with stage/ and build/ (default: the
# parent of tools/). Run it with nohup; it waits for the game itself.
TOOLS=$(cd "$(dirname "$0")" && pwd)
ROOT=${G3_ROOT:-$(dirname "$TOOLS")}
label=$1
out=$2
mod=$3
secs=${4:-240}
if [ -z "$label" ] || [ -z "$out" ] || [ -z "$mod" ]; then
    sed -n 2,12p "$0"
    exit 2
fi
profile="$out/profile"
mkdir -p "$profile"
case "$mod" in
*.json)
    [ "$mod" -ef "$profile/active-mods.json" ] || cp "$mod" "$profile/active-mods.json" ;;
none)
    printf '{"version": 1, "preset": null, "mods": []}\n' > "$profile/active-mods.json" ;;
*)
    printf '{"version": 1, "preset": null, "mods": [{"id": "%s", "path": "%s"}]}\n' \
      "$(basename "$mod")" "$mod" > "$profile/active-mods.json" ;;
esac
cd "$ROOT" || exit 1
G3_ROOT="$ROOT" .venv/bin/python -I -c '
import sys
sys.path.insert(0, sys.argv[1])
import run
p = run.prepare_profile("world", sys.argv[2])
r = run.build_mod_view(p)
for l in r["links"]:
    print("link %s -> %s (%s)" % (l["path"], l["target"], l["mod"]))
for s in r["skipped"]:
    print("left out %s (%s): %s" % (s["file"], s["mod"], s["reason"]))
' "$TOOLS" "$profile" > "$out/$label.prep" 2>&1 || { cat "$out/$label.prep"; exit 1; }
ctl="$out/$label.ctl"
printf 'FRAMES=10\n' > "$ctl"
# The Metal renderer's RECOMP_FRAME_DUMP writes into an existing directory only. Its frames are the
# evidence; the smoke host's own `dump` sees no picture of a Direct3D 9 game (non-black 0.000).
mkdir -p "$out/$label-frames" "$out/$label-dumps"
script="$out/$label.smoke"
# Frame dumps by the smoke host (`dump`, <out-dir>/<label>-dumps/smoke_<label>-<s>_present.ppm) at
# 40%, 65% and 90% of the run: the same script times in a run with the mod and one without.
a=$((secs * 400)); b=$((secs * 250)); c=$((secs * 250)); d=$((secs * 100))
printf 'wait %s\ndump %s-%s\nwait %s\ndump %s-%s\nwait %s\ndump %s-%s\nwait %s\nquit\n' \
  "$a" "$label" "$((a / 1000))" "$b" "$label" "$(((a + b) / 1000))" "$c" "$label" "$(((a + b + c) / 1000))" \
  "$d" > "$script"
RECOMP_PROFILE_DIR="$profile" RECOMP_D3D9_SHADER_WARM_DIR="$profile/shader-warm" RECOMP_ASSUME_FOCUS=1 \
  RECOMP_TRACE_CONTROL="$ctl" RECOMP_SMOKE_DRAWABLE="${DRAWABLE:-1920x1200}" \
  RECOMP_SMOKE_SECONDS="$((secs + 15))" RECOMP_TRACE_FILES=1 \
  RECOMP_FRAME_DUMP="$out/$label-frames" RECOMP_FRAME_DUMP_EVERY=20 \
  RECOMP_HOST_DUMP_DIR="$out/$label-dumps" \
  nohup build/recomp/pop_smoke --script "$script" > "$out/$label.log" 2>&1 &
pid=$!
echo "$pid" > "$out/$label.pid"
end=$(( $(date +%s) + secs + 30 ))
while kill -0 "$pid" 2>/dev/null && [ "$(date +%s)" -lt "$end" ]; do sleep 5; done
kill "$pid" 2>/dev/null
sleep 4
kill -9 "$pid" 2>/dev/null
echo "mods_smoke: $label done"
