#!/bin/sh
# tools/bench.sh <label> [seconds] [mode] [trace.ctl lines...]: the port without a window (pop_smoke:
# the real Metal renderer into an offscreen target, no audio device, no cursor capture), on the
# autostart profile of <mode> (world = NoMenu, default; tools/run.py prepares it, so the player's
# saves and options are untouched). FRAMES=10 from the start through its own control file
# (work/bench-<label>.ctl; work/trace.ctl belongs to a game the player runs). Log
# work/bench-<label>.log; stopped after <seconds> (default 240) by PID.
cd "$(dirname "$0")/.."
label=$1
secs=${2:-240}
mode=${3:-world}
shift 3 2>/dev/null || shift $#
ctl="$PWD/work/bench-$label.ctl"
printf 'FRAMES=10\n' > "$ctl"
for line in "$@"; do printf '%s\n' "$line" >> "$ctl"; done
script="$PWD/work/bench-$label.smoke"
# BENCH_STEPS: smoke script lines run first (kit/host/script.h), e.g. the clicks that start a new game.
# G3_INI_SET="Key=value,...": ge3.ini keys for this run (tools/run.py prepare_profile).
# BENCH_BIN: another pop_smoke binary for an A/B (default build/recomp/pop_smoke).
printf '%b' "${BENCH_STEPS:-}" > "$script"
printf 'wait %s\nquit\n' "$((secs * 1000))" >> "$script"
# BENCH_PROFILE=<dir>: a profile of its own (two benches at once, an A/B under the same load), prepared as <mode>'s.
profile=$(.venv/bin/python -c "import os, sys; sys.path.insert(0, 'tools'); import run; print(run.prepare_profile('$mode', os.environ.get('BENCH_PROFILE') or None))")
# The player's own setup by default (user, 28.10.2026: measurements on other data than the player's never matched
# the game; the bench said 70 fps in a fight where the player had 20). Copies, never the player's files:
# - mods: the player's active-mods.json (BENCH_MODS_FILE another list, BENCH_MODS=0 unmodded);
# - host settings: the player's mod-settings.json (BENCH_PLAYER_SETTINGS=0 keeps the profile's own);
# - drawable: the player's screen, 3456x2170 (BENCH_DRAWABLE another size);
# - game data: the player's game folder, read only, when RECOMP_EXE is unset (BENCH_GAME=stage runs on stage/), with
#   the archives the player's profile lays over its Data (Waste Data Patch 2.0, D-040) linked into this profile.
# ge3.ini, UserOptions.ini and Shader.Cache come from the player's profile already (tools/run.py prepare_profile).
player="$HOME/Library/Application Support/Gothic 3 Native"
if [ "${BENCH_MODS:-1}" = 1 ]; then
  cp "${BENCH_MODS_FILE:-$player/active-mods.json}" "$profile/active-mods.json"
else
  rm -f "$profile/active-mods.json"
fi
if [ "${BENCH_PLAYER_SETTINGS:-1}" = 1 ] && [ -f "$player/mod-settings.json" ]; then
  cp "$player/mod-settings.json" "$profile/mod-settings.json"
fi
mkdir -p "$profile/Data"
for f in "$profile"/Data/*.pak "$profile"/Data/*.cpt "$profile"/Data/*.p[0-9][0-9]; do
  [ -L "$f" ] && rm -f "$f"
done
for f in "$player"/Data/*.pak "$player"/Data/*.cpt "$player"/Data/*.p[0-9][0-9]; do
  [ -e "$f" ] && ln -s "$f" "$profile/Data/${f##*/}"
done
game_exe="$player/game/Gothic3Final.exe"
if [ -z "${RECOMP_EXE:-}" ] && [ "${BENCH_GAME:-player}" != stage ] && [ -f "$game_exe" ]; then
  export RECOMP_EXE="$game_exe"
fi
.venv/bin/python -c "import sys; sys.path.insert(0, 'tools'); import run; run.build_mod_view('$profile')" >&2
# The new game's intro (G3_Intro.bik, 5354 frames, about 214 s) skipped as tools/run.py does for newgame;
# without it a scripted new game sat at 1 draw per frame for the whole run.
RECOMP_BINK_SKIP="${RECOMP_BINK_SKIP-G3_Intro}" \
RECOMP_PROFILE_DIR="$profile" RECOMP_D3D9_SHADER_WARM_DIR="$profile/shader-warm" RECOMP_ASSUME_FOCUS=1 \
  RECOMP_TRACE_CONTROL="$ctl" RECOMP_SMOKE_SECONDS="$((secs + 10))" RECOMP_SMOKE_DRAWABLE="${BENCH_DRAWABLE:-3456x2170}" \
  nohup "${BENCH_BIN:-build/recomp/pop_smoke}" --script "$script" > "work/bench-$label.log" 2>&1 &
pid=$!
echo "$pid" > "work/bench-$label.pid"
end=$(( $(date +%s) + secs + 30 ))
while kill -0 "$pid" 2>/dev/null && [ "$(date +%s)" -lt "$end" ]; do sleep 5; done
kill "$pid" 2>/dev/null
sleep 4
kill -9 "$pid" 2>/dev/null
grep "frames, last" "work/bench-$label.log" | tail -12
