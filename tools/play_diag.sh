#!/bin/sh
# tools/play_diag.sh: start the game as the player does (their profile, their mods, the launcher) and record
# what a slow frame is made of, without the player doing anything else:
#   work/play.log          the game's log, with the frame time every 10 s (FRAMES=10)
#   work/play-prev.log     the log of the run before (each start used to overwrite it)
#   work/play-sample-N.txt only with PLAY_SAMPLE=1: a 5 s `sample` of the game whenever a 10 s window is slower
#                          than 40 ms, or a single frame took over 100 ms (the kit logs "slow frame" with its CPU
#                          time and page-ins); at most one a minute, eight in all, named by the frame time that
#                          triggered it. Off by default: `sample` stops the process every millisecond, its 5 s
#                          are frames of about 110 ms with 10 ms of work, a stutter the player feels.
#   work/play-mem.log      swap and memory pressure every 30 s
# work/trace.ctl is reset to FRAMES=10 first: a leftover CALLS/STACK trace costs frame time.
cd "$(dirname "$0")/.."
printf 'FRAMES=10\n' > work/trace.ctl
[ -s work/play.log ] && mv work/play.log work/play-prev.log
: > work/play-mem.log
rm -f work/play-sample-*.txt
RECOMP_TRACE_CONTROL="$PWD/work/trace.ctl" nohup .venv/bin/python tools/run.py --autostart menu > work/play.log 2>&1 &
launcher=$!
# tools/run.py execs the app, so the PID stays the game's.
echo "$launcher" > work/play.pid
(
  n=0; last=0; spikes=0
  while kill -0 "$launcher" 2>/dev/null; do
    sleep 5
    now=$(date +%s)
    if [ $((now % 30)) -lt 5 ]; then
      { date '+%H:%M:%S'; sysctl -n vm.swapusage; memory_pressure 2>/dev/null | tail -1;
        ps -o rss= -p "$launcher" | awk '{printf "game RSS %.0f MB\n", $1/1024}'; } >> work/play-mem.log
    fi
    [ "${PLAY_SAMPLE:-0}" = 1 ] || continue
    # A new single frame over 100 ms (not while loading: the window around it still has to be under 1 s).
    count=$(grep -c "d3d9: slow frame" work/play.log)
    if [ "$count" -gt "$spikes" ]; then
      spikes=$count
      spike=$(grep "d3d9: slow frame" work/play.log | tail -1 | sed 's/.*slow frame \([0-9.]*\) ms.*/\1/')
      if [ -n "$spike" ] && [ "$(echo "$spike < 1000" | bc 2>/dev/null)" = 1 ] && [ $((now - last)) -ge 60 ] && [ "$n" -lt 8 ]; then
        n=$((n + 1)); last=$now
        sample "$launcher" 5 -mayDie -file "work/play-sample-$n-spike-${spike}ms.txt" > /dev/null 2>&1
        continue
      fi
    fi
    ms=$(grep "ms per frame" work/play.log | tail -1 | sed 's/.*fps, \([0-9.]*\) ms per frame.*/\1/')
    [ -n "$ms" ] || continue
    slow=$(echo "$ms > 40 && $ms < 1000" | bc 2>/dev/null)
    if [ "$slow" = 1 ] && [ $((now - last)) -ge 60 ] && [ "$n" -lt 8 ]; then
      n=$((n + 1)); last=$now
      sample "$launcher" 5 -mayDie -file "work/play-sample-$n-${ms}ms.txt" > /dev/null 2>&1
    fi
  done
) > /dev/null 2>&1 &
echo "Gra startuje (PID $launcher). Graj normalnie; log w work/play.log, próbki wolnych klatek w work/play-sample-*.txt."
