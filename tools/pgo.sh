#!/bin/sh
# tools/pgo.sh: the profile for game.toml [translate] pgo_profile (stage/gothic3.profdata).
#   1. pop_smoke built instrumented (tools/build.py --pgo-generate --target smoke),
#   2. tools/bench.sh walka and chodzenie on the player's setup with LLVM_PROFILE_FILE in continuous mode (%c: the
#      counters live in the file while the game runs, so the bench's stop by PID loses nothing),
#   3. llvm-profdata merge into stage/gothic3.profdata,
#   4. tools/build.py: the app and pop_smoke with ThinLTO and the profile.
# Each step leaves work/pgo/<step>.done and is skipped when it is there, so a stopped run goes on from the next one.
# Progress: work/pgo/pgo.log. Run with nohup; stop by PID.
cd "$(dirname "$0")/.."
out="$PWD/work/pgo"
mkdir -p "$out/raw"
log() { echo "$(date '+%H:%M:%S') $*" | tee -a "$out/pgo.log"; }
step() { # step <name> <command...>
  name=$1; shift
  if [ -f "$out/$name.done" ]; then log "$name: done before, skipped"; return 0; fi
  log "$name: start"
  if "$@" >> "$out/$name.log" 2>&1; then touch "$out/$name.done"; log "$name: done"; else log "$name: FAILED (see $out/$name.log)"; exit 1; fi
}
step instrumented .venv/bin/python tools/build.py --pgo-generate --target smoke
export LLVM_PROFILE_FILE="$out/raw/%c%p.profraw"
step train-walka sh tools/bench.sh walka pgo-walka
step train-chodzenie sh tools/bench.sh chodzenie pgo-chodzenie
unset LLVM_PROFILE_FILE
step merge xcrun llvm-profdata merge -o stage/gothic3.profdata "$out"/raw/*.profraw
step final .venv/bin/python tools/build.py
log "all done: stage/gothic3.profdata, build/Gothic3Recomp.app and build/recomp/pop_smoke with ThinLTO and the profile"
