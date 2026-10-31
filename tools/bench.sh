#!/bin/sh
# tools/bench.sh <scenariusz> [etykieta]: the port without a window (pop_smoke: the real Metal renderer into an
# offscreen target, no audio device, no cursor capture) on the player's setup, the same build and settings as their
# game (tools/graj.sh). Scenarios:
#   swiat      the world (the engine's NoMenu), standing, 240 s
#   chodzenie  the player's QuickSave (F9), then about 5 minutes of walking with turns
#   walka      a new game, then the Ardea fight at its start, swinging, 360 s
#   sceny      reference pictures to compare before and after a change: the world, then for each scene of SCENES
#              (default below; one a line, "name;console command;console command") the commands through G3CMD, the
#              world frozen (timer.fTimeScale=0) 15 s later, and two frames 1 s apart, smoke_<name>_{a,b}_present.ppm.
#              The two frames' difference is the scene's own noise. Compared by tools/sceny_diff.py.
# Everything of one run goes to work/bench-<etykieta>/ (default <scenariusz>-<MMDD-HHMM>): bench.log (frame times
# every 10 s, FRAMES=10), the stacks of every frame over 45 ms (BENCH_SPIKE=<ms>; s-spike-*.txt, with syms.txt), and at the end a
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
  sceny) mode=world; skip=3 ;;
  *) echo "usage: tools/bench.sh swiat|chodzenie|walka|sceny [etykieta]" >&2; exit 2 ;;
esac
# BENCH_SECS=<s>: a shorter run, e.g. chodzenie for the first half minute of walking after the load (150).
secs=${BENCH_SECS:-$secs}
# BENCH_PIN=<start>:<step> (D-054 step 2): the guest's clock moves <step> ms a presented frame (RECOMP_PIN_CLOCK), and
# the script's waits with it, so two runs of one script see the same input at the same frame; not a timing
# measurement. The run may then take up to three times its seconds. BENCH_HASH=1: every frame's picture hashed into
# frames.hash (RECOMP_D3D9_FRAME_HASH), to compare runs with tools/frames_diff.py.
real=$secs
[ -n "$BENCH_PIN" ] && real=$((secs * 3))
label=${2:-$scenario-$(date +%m%d-%H%M)}
out="$PWD/work/bench-$label"
rm -rf "$out" && mkdir -p "$out" || exit 1
player="$HOME/Library/Application Support/Gothic 3 Native"
bin=${BENCH_BIN:-build/recomp/pop_smoke} # BENCH_BIN: another build of it (the read watch build, build-watch)

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
# Scenes: the commands at a fixed time from the start, the frames at the same times in the script, so both run on
# one plan; the world is in by T0 (about 60 s on the player's setup) and each scene takes 25 s. Chosen in session 22
# from 15 candidates: night with rain and torches (Geldern, Faring), close textures (Montera), the king's hall with guards and a fire
# (Rhobar), a valley with waterfalls (Silden), grass, rock and a banded sky (Lester).
T0=100
SCENES=${SCENES:-'geldern_noc;goto Geldern;game.SetTime(22,0)
faring_noc;goto Faring;game.SetTime(22,0)
montera;goto Montera;game.SetTime(15,0)
rhobar_noc;goto Rhobar;game.SetTime(23,0)
silden;goto Silden;game.SetTime(15,0)
lester;goto Lester;game.SetTime(15,0)'}
if [ "$scenario" = sceny ]; then
  n=$(printf '%s\n' "$SCENES" | grep -c .)
  secs=$((T0 + 25 * n + 5))
  steps="wait $((T0 * 1000))\n"
  for name in $(printf '%s\n' "$SCENES" | cut -d';' -f1); do
    # Two seconds after the goto the player steps back (S, 1.5 s), so the camera is not inside the entity.
    steps="${steps}wait 2000\nkey S down\nwait 1500\nkey S up\nwait 14500\ndump ${name}_a\nwait 1000\ndump ${name}_b\nwait 6000\n"
  done
fi
printf '%b' "$steps" > "$out/bench.smoke"
# BENCH_STEPS=<file>: smoke script lines instead of the scenario's own input, on the scenario's setup (chodzenie: the
# player's QuickSave), e.g. F9, then a screen or a dialog to measure.
[ -n "$BENCH_STEPS" ] && cp "$BENCH_STEPS" "$out/bench.smoke"
printf 'wait %s\nquit\n' "$((secs * 1000))" >> "$out/bench.smoke"
printf 'FRAMES=10\n' > "$out/bench.ctl"
# BENCH_CTL=<file>: more trace control lines from the start (kit/runtime/cpu.cpp, RECOMP_TRACE_CONTROL), e.g. CALLS and STACK.
[ -n "$BENCH_CTL" ] && cat "$BENCH_CTL" >> "$out/bench.ctl"

# BENCH_PROFILE=<dir>: the run's own profile instead of work/autostart-<mode>/profile, so pinned runs can go side by side
# (two runs on one profile overwrite each other's options: 30.10, two walka runs came up at 1280x960 in the menu).
# It starts with the shared profile's warmed shaders.
if [ -n "$BENCH_PROFILE" ]; then
  profile=$(.venv/bin/python -c "import sys; sys.path.insert(0, 'tools'); import run; print(run.prepare_profile('$mode', '$BENCH_PROFILE'))")
  shared="work/autostart-$mode/profile/shader-warm"
  [ -d "$shared" ] && [ ! -d "$profile/shader-warm" ] && cp -R "$shared" "$profile/shader-warm"
else
  profile=$(.venv/bin/python -c "import sys; sys.path.insert(0, 'tools'); import run; print(run.prepare_profile('$mode'))")
fi
cp "$player/active-mods.json" "$profile/active-mods.json"
[ -f "$player/mod-settings.json" ] && cp "$player/mod-settings.json" "$profile/mod-settings.json"
if [ "$scenario" = chodzenie ]; then
  mkdir -p "$profile/Documents/gothic3"
  # BENCH_SAVE=<dir>: the QuickSave from that folder instead of the player's latest (a copy keeps a scene fixed while
  # the player goes on saving).
  cp -p "${BENCH_SAVE:-$player/Documents/gothic3}/QuickSave.g3savcpx"* "$profile/Documents/gothic3/"
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

[ -n "$BENCH_PIN" ] && export RECOMP_PIN_CLOCK="$BENCH_PIN"
[ -n "$BENCH_HASH" ] && export RECOMP_D3D9_FRAME_HASH="$out/frames.hash"
# BENCH_HASH_DUMP=<first>-<last>: those frames' pictures beside it too (frames.hash.<n>.ppm).
[ -n "$BENCH_HASH_DUMP" ] && export RECOMP_D3D9_FRAME_HASH_DUMP="$BENCH_HASH_DUMP"
# The new game's intro (G3_Intro.bik, about 214 s) skipped; without it a scripted new game sat at 1 draw per frame.
RECOMP_EXE="$player/game/Gothic3Final.exe" RECOMP_BINK_SKIP=G3_Intro \
  RECOMP_PROFILE_DIR="$profile" RECOMP_D3D9_SHADER_WARM_DIR="$profile/shader-warm" RECOMP_ASSUME_FOCUS=1 \
  RECOMP_TRACE_CONTROL="$out/bench.ctl" RECOMP_SMOKE_SECONDS="$((real + 10))" RECOMP_SMOKE_DRAWABLE=3456x2170 \
  RECOMP_HOST_PROFILE="$out/s" RECOMP_HOST_PROFILE_SPIKE=${BENCH_SPIKE:-45} RECOMP_HOST_DUMP_DIR="$out" \
  nohup "$bin" --script "$out/bench.smoke" > "$out/bench.log" 2>&1 &
pid=$!
echo "$pid" > "$out/bench.pid"
if [ "$scenario" = sceny ]; then
  (
    sleep $T0
    i=0
    printf '%s\n' "$SCENES" | while IFS= read -r line; do
      [ -n "$line" ] || continue
      i=$((i + 1))
      echo "G3CMD=timer.fTimeScale=1.0 #$i" >> "$out/bench.ctl"
      printf '%s\n' "$line" | cut -d';' -f2- | tr ';' '\n' | while IFS= read -r cmd; do
        echo "G3CMD=$cmd #$i" >> "$out/bench.ctl"
      done
      sleep 15
      echo "G3CMD=timer.fTimeScale=0.0 #$i" >> "$out/bench.ctl"
      sleep 10
    done
  ) &
fi
end=$(( $(date +%s) + real + 30 ))
while kill -0 "$pid" 2>/dev/null && [ "$(date +%s)" -lt "$end" ]; do sleep 5; done
kill "$pid" 2>/dev/null
sleep 4
kill -9 "$pid" 2>/dev/null
.venv/bin/python tools/bench_summary.py "$out/bench.log" "$skip"
ls "$out"/s-spike-*.txt > /dev/null 2>&1 && .venv/bin/python tools/spike_report.py "$out" 8 | tail -3
