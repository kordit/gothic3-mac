#!/bin/sh
# tools/bench_fight.sh <label> [trace.ctl lines...]: the fight at the start of a new game, as close to the player's
# (work/play.log, 25.10.2026) as a script gets: the player's mods, the new game clicked, then the player steps into
# the Ardea fight and swings (W for FIGHT_WALK ms, left clicks, FIGHT_TURN mouse units a swing, a short step every
# sixth), 360 s. Samples of the game at 170 s and 280 s (work/sample-<label>-a/b.txt), summary work/bench-<label>.sum.
# BENCH_BIN and BENCH_PROFILE as tools/bench.sh has them.
cd "$(dirname "$0")/.."
label=${1:-f0}; shift
STEPS='wait 60000\nmove 960 600\nwait 1500\nclick left 960 600\nwait 12000\nmove 729 475\nwait 1500\nclick left 729 475\nwait 36000\n'
STEPS="${STEPS}key W down\nwait ${FIGHT_WALK:-1500}\nkey W up\n"
i=0
while [ $i -lt 60 ]; do
  STEPS="${STEPS}button left down\nwait 300\nbutton left up\nwait 900\nmoveby ${FIGHT_TURN:-0} 0\nwait 300\n"
  if [ $((i % 6)) = 5 ]; then STEPS="${STEPS}key W down\nwait ${FIGHT_STEP:-400}\nkey W up\n"; fi
  i=$((i + 1))
done
( BENCH_MODS=1 BENCH_STEPS="$STEPS" sh tools/bench.sh "$label" 360 newgame "$@" > /dev/null 2>&1 ) &
sleep 175; sample "$(cat work/bench-$label.pid)" 8 -mayDie -file "work/sample-$label-a.txt" > /dev/null 2>&1
sleep 100; sample "$(cat work/bench-$label.pid)" 8 -mayDie -file "work/sample-$label-b.txt" > /dev/null 2>&1
wait
.venv/bin/python tools/bench_sum.py "$label" > "work/bench-$label.sum" 2>&1
echo done > "work/bench-$label.done"
