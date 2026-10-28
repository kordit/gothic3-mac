#!/bin/sh
# tools/bench.sh <scenariusz> [etykieta]: the port without a window (pop_smoke: the real Metal renderer into an
# offscreen target, no audio device, no cursor capture) on the player's setup, the same build and settings as their
# game (tools/graj.sh). Scenarios:
#   swiat      the world (the engine's NoMenu), standing, 240 s
#   chodzenie  the player's QuickSave (F9), then about 5 minutes of walking with turns
#   walka      a new game, then the Ardea fight at its start, swinging, 360 s
# Everything of one run goes to work/bench-<etykieta>/ (default <scenariusz>-<MMDD-HHMM>): bench.log (frame times
# every 10 s, FRAMES=10), the stacks of every frame over 45 ms (s-spike-*.txt, with syms.txt), and at the end a
# summary of both on stdout (tools/bench_summary.py, tools/spike_report.py). Stopped by PID after its time.
#
# The player's setup, copies only, never their files (user, 28.10.2026: measurements on other data than the
# player's never matched the game): their game folder (read only) with the archives their profile lays over Data
# (Waste Data Patch 2.0, D-040), their active-mods.json and mod-settings.json, their ge3.ini, UserOptions.ini and
# Shader.Cache (tools/run.py prepare_profile), their screen's drawable, 3456x2170.
cd "$(dirname "$0")/.."
scenario=$1
case "$scenario" in
  swiat) mode=world; secs=240; skip=3 ;;
  chodzenie) mode=world; secs=420; skip=12 ;;
  walka) mode=newgame; secs=360; skip=12 ;;
  *) echo "usage: tools/bench.sh swiat|chodzenie|walka [etykieta]" >&2; exit 2 ;;
esac
label=${2:-$scenario-$(date +%m%d-%H%M)}
out="$PWD/work/bench-$label"
rm -rf "$out" && mkdir -p "$out" || exit 1
player="$HOME/Library/Application Support/Gothic 3 Native"
bin=build/recomp/pop_smoke

# The input: smoke script lines (kit/host/script.h), then the run's length.
steps=
if [ "$scenario" = chodzenie ]; then
  steps='wait 70000\nkey F9 down\nwait 150\nkey F9 up\nwait 40000\n'
  i=0
  while [ $i -lt 30 ]; do
    steps="${steps}key W down\nwait 7000\nmoveby 700 0\nwait 3000\nmoveby -350 0\n"
    i=$((i + 1))
  done
  steps="${steps}key W up\nwait 5000\n"
elif [ "$scenario" = walka ]; then
  steps='wait 60000\nmove 960 600\nwait 1500\nclick left 960 600\nwait 12000\nmove 729 475\nwait 1500\nclick left 729 475\nwait 36000\n'
  steps="${steps}key W down\nwait 1500\nkey W up\n"
  i=0
  while [ $i -lt 60 ]; do
    steps="${steps}button left down\nwait 300\nbutton left up\nwait 900\nmoveby 0 0\nwait 300\n"
    [ $((i % 6)) = 5 ] && steps="${steps}key W down\nwait 400\nkey W up\n"
    i=$((i + 1))
  done
fi
printf '%b' "$steps" > "$out/bench.smoke"
printf 'wait %s\nquit\n' "$((secs * 1000))" >> "$out/bench.smoke"
printf 'FRAMES=10\n' > "$out/bench.ctl"

profile=$(.venv/bin/python -c "import sys; sys.path.insert(0, 'tools'); import run; print(run.prepare_profile('$mode'))")
cp "$player/active-mods.json" "$profile/active-mods.json"
[ -f "$player/mod-settings.json" ] && cp "$player/mod-settings.json" "$profile/mod-settings.json"
if [ "$scenario" = chodzenie ]; then
  mkdir -p "$profile/Documents/gothic3"
  cp -p "$player/Documents/gothic3/QuickSave.g3savcpx"* "$profile/Documents/gothic3/"
fi
mkdir -p "$profile/Data"
for f in "$profile"/Data/*.pak "$profile"/Data/*.cpt "$profile"/Data/*.p[0-9][0-9]; do
  [ -L "$f" ] && rm -f "$f"
done
for f in "$player"/Data/*.pak "$player"/Data/*.cpt "$player"/Data/*.p[0-9][0-9]; do
  [ -e "$f" ] && ln -s "$f" "$profile/Data/${f##*/}"
done
.venv/bin/python -c "import sys; sys.path.insert(0, 'tools'); import run; run.build_mod_view('$profile')" >&2
nm -n "$bin" > "$out/syms.txt"

# The new game's intro (G3_Intro.bik, about 214 s) skipped; without it a scripted new game sat at 1 draw per frame.
RECOMP_EXE="$player/game/Gothic3Final.exe" RECOMP_BINK_SKIP=G3_Intro \
  RECOMP_PROFILE_DIR="$profile" RECOMP_D3D9_SHADER_WARM_DIR="$profile/shader-warm" RECOMP_ASSUME_FOCUS=1 \
  RECOMP_TRACE_CONTROL="$out/bench.ctl" RECOMP_SMOKE_SECONDS="$((secs + 10))" RECOMP_SMOKE_DRAWABLE=3456x2170 \
  RECOMP_HOST_PROFILE="$out/s" RECOMP_HOST_PROFILE_SPIKE=45 \
  nohup "$bin" --script "$out/bench.smoke" > "$out/bench.log" 2>&1 &
pid=$!
echo "$pid" > "$out/bench.pid"
end=$(( $(date +%s) + secs + 30 ))
while kill -0 "$pid" 2>/dev/null && [ "$(date +%s)" -lt "$end" ]; do sleep 5; done
kill "$pid" 2>/dev/null
sleep 4
kill -9 "$pid" 2>/dev/null
.venv/bin/python tools/bench_summary.py "$out/bench.log" "$skip"
ls "$out"/s-spike-*.txt > /dev/null 2>&1 && .venv/bin/python tools/spike_report.py "$out" 8 | tail -3
