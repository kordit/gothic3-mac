#!/bin/sh
# tools/bench_ab.sh world|fight <labelA> <binA> <labelB> <binB>: two pop_smoke builds at the same time, each on a
# profile of its own, so both see the same machine load. Benches taken one after the other drift: 25.10.2026 the same
# binary gave 25.6 ms and an hour later 31.1 ms while the player's Chrome took 1-2 cores. world = NoMenu, 170 s
# (steady, the one to trust for small differences); fight = tools/bench_fight.sh. Summary work/bench-ab-<A>-<B>.sum.
cd "$(dirname "$0")/.."
mode=$1; la=$2; ba=$3; lb=$4; bb=$5
case $mode in
world) prof=world ;;
fight) prof=newgame ;;
*) echo "usage: tools/bench_ab.sh world|fight labelA binA labelB binB" >&2; exit 2 ;;
esac
b="$PWD/work/autostart-$prof-b/profile"
.venv/bin/python -c "import sys; sys.path.insert(0, 'tools'); import run; run.prepare_profile('$prof', '$b')" > /dev/null
[ -d "$b/shader-warm" ] || cp -R "work/autostart-$prof/profile/shader-warm" "$b/" 2>/dev/null
if [ "$mode" = world ]; then
  ( BENCH_BIN=$ba sh tools/bench.sh "$la" 170 world 'G3OPT=all' > /dev/null 2>&1 ) &
  sleep 2
  ( BENCH_BIN=$bb BENCH_PROFILE="$b" sh tools/bench.sh "$lb" 170 world 'G3OPT=all' > /dev/null 2>&1 ) &
else
  BENCH_BIN=$ba sh tools/bench_fight.sh "$la" 'G3OPT=all' &
  sleep 3
  BENCH_BIN=$bb BENCH_PROFILE="$b" sh tools/bench_fight.sh "$lb" 'G3OPT=all' &
fi
wait
.venv/bin/python tools/bench_sum.py "$la" "$lb" > "work/bench-ab-$la-$lb.sum"
echo done > "work/bench-ab-$la-$lb.done"
