#!/bin/sh
# Stage 4 soak: the player's QuickSave (F9), then about 15 minutes of walking with turns, a quick save (F5) every 3
# minutes and one quick load (F9) halfway. Log work/bench-soak-<label>.log. BENCH_BIN picks the binary.
cd "$(dirname "$0")/.."
label=$1; shift
p="$PWD/work/soak-$label/profile"
.venv/bin/python -c "import sys; sys.path.insert(0, 'tools'); import run; run.prepare_profile('world', '$p')" > /dev/null
mkdir -p "$p/Documents/gothic3"
cp -p "$HOME/Library/Application Support/Gothic 3 Native/Documents/gothic3/QuickSave.g3savcpx"* "$p/Documents/gothic3/"
steps='wait 70000\nkey F9 down\nwait 150\nkey F9 up\nwait 40000\n'
i=0
while [ $i -lt 90 ]; do
  steps="${steps}key W down\nwait 7000\nmoveby 700 0\nwait 3000\nmoveby -350 0\n"
  if [ $((i % 18)) -eq 17 ]; then steps="${steps}key W up\nwait 500\nkey F5 down\nwait 150\nkey F5 up\nwait 6000\n"; fi
  if [ $i -eq 45 ]; then steps="${steps}key W up\nwait 500\nkey F9 down\nwait 150\nkey F9 up\nwait 40000\n"; fi
  i=$((i + 1))
done
steps="${steps}key W up\nwait 5000\n"
opt=${WALK_OPT:-all}
RECOMP_G3OPT="$opt" BENCH_PROFILE="$p" BENCH_MODS=1 BENCH_STEPS="$steps" \
  sh tools/bench.sh "soak-$label" 1200 world "G3OPT=$opt" "$@" > /dev/null 2>&1
echo done > "work/soak-$label.done"
