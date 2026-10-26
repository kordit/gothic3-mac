#!/bin/sh
# tools/wine_probe.sh LABEL [SECS]: one unattended start of the original under Wine (tools/wine_run.sh).
# Polls every 3 s; a crash is a game log (ge3log*.log) written after the start or the crash dialog
# (Dialogs.dll) loading. Kills the game on a crash or after SECS (default 90) and prints one line:
#   LABEL CRASH|ALIVE t=<s> rss_max=<MB> cpu=<s> [crash address]
# Takes the same environment as wine_run.sh (WINE_D3D_CONFIG, WINEDEBUG, ...).
set -u
cd "$(dirname "$0")/.."
label=${1:?label}; secs=${2:-90}
out=work/wine-$label
prof=$HOME/Documents/Gothic3
server="$HOME/g3-wine/wine/Wine Staging.app/Contents/Resources/wine/bin/wineserver"
mark=$(mktemp); trap 'rm -f $mark' EXIT
nohup sh tools/wine_run.sh $label > /dev/null 2>&1 &
start=$(date +%s); state=ALIVE; t=0
sleep 3
while [ $t -lt $secs ]; do
    t=$(( $(date +%s) - start ))
    if [ -n "$(find "$prof" -name 'ge3log*.log' -newer $mark 2>/dev/null)" ] || \
       grep -q 'Dialogs.dll" at' $out/wine.log 2>/dev/null; then state=CRASH; break; fi
    pgrep -f 'Gothic3Final.exe' > /dev/null || { state=EXITED; break; }
    sleep 3
done
cpu=$(ps -o time= -p "$(pgrep -f 'Gothic3Final.exe' | head -1)" 2>/dev/null | tr -d ' ')
WINEPREFIX=$HOME/g3-wine/prefix "$server" -k 2>/dev/null; WINEPREFIX=$HOME/g3-wine/prefix "$server" -w 2>/dev/null
rss=$(awk 'NR>1 && $3>m {m=$3} END {print m+0}' $out/mem.tsv 2>/dev/null)
addr=$(find "$prof" -name 'ge3log*.log' -newer $mark -exec sh -c "LC_ALL=C tr -cd '\11\12\40-\176' < \"\$1\" | grep -m1 'in module'" _ {} \; 2>/dev/null)
echo "$label $state t=$t rss_max=${rss}MB cpu=${cpu:-?} $addr" | tee -a work/wine-probe.log
