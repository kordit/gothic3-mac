#!/bin/sh
# tools/wine_run.sh LABEL: the original game under Wine (set up by tools/wine_setup.sh) as a reference.
# Runs until the game exits. Writes to work/wine-LABEL/:
#   wine.log   the Wine log (WINEDEBUG, default err+all,fixme-all)
#   mem.tsv    the game's memory every 5 s (a few bytes a line)
#   shot-*.png a screenshot only on demand: `touch work/wine-LABEL/shot`
# WINE_SHOT_EVERY=<s> adds periodic screenshots, at most WINE_SHOT_MAX (default 20), 960 px wide.
# WINE_GAME_DIR overrides the game folder; WINE_D3D_CONFIG defaults to renderer=gl (Vulkan through
# MoltenVK failed in vkCreateBufferView).
set -eu
cd "$(dirname "$0")/.."
label=${1:?label}
out=work/wine-$label; mkdir -p $out
root=$HOME/g3-wine
bin="$root/wine/Wine Staging.app/Contents/Resources/wine/bin"
[ -x "$bin/wine" ] || { echo "no Wine: run tools/wine_setup.sh" >&2; exit 1; }
export WINEPREFIX="$root/prefix" WINEDEBUG="${WINEDEBUG:-err+all,fixme-all}" WINEDLLOVERRIDES="d3dx9_40=n,b"
export WINE_D3D_CONFIG="${WINE_D3D_CONFIG:-renderer=gl}"
game=${WINE_GAME_DIR:-"$WINEPREFIX/drive_c/Program Files/Gothic 3"}
every=${WINE_SHOT_EVERY:-0}; max=${WINE_SHOT_MAX:-20}

shot() {
    f=$out/shot-$(printf %05d $1).png
    screencapture -x -t png $f && sips -Z 960 $f > /dev/null 2>&1
    shots=$((shots + 1))
}

# The profile is the Mac's ~/Documents/Gothic3 (the prefix links Documents there). The game renames
# UserOptions.ini to __UserOptions.ini while it runs, so a crash leaves only the latter and the next
# start takes the defaults, which crash under Wine in wined3d's device creation (wine-w6). Without
# Shader.Cache it compiles every shader before the first frame; the install's cache seeds it.
prof=$HOME/Documents/Gothic3
mkdir -p "$prof"
[ -f "$prof/UserOptions.ini" ] || [ ! -f "$prof/__UserOptions.ini" ] || cp "$prof/__UserOptions.ini" "$prof/UserOptions.ini"
[ -f "$prof/Shader.Cache" ] || cp "$game/Shader.Cache" "$prof/Shader.Cache"

date "+start %F %T WINEDEBUG=$WINEDEBUG WINE_D3D_CONFIG=$WINE_D3D_CONFIG" > $out/run.txt
(cd "$game" && exec "$bin/wine" Gothic3Final.exe) > $out/wine.log 2>&1 &
wpid=$!
echo "$wpid" > $out/wine.pid
echo "game started (wine pid $wpid), output in $out; screenshot: touch $out/shot"
start=$(date +%s); shots=0; last=0
printf 't\tpid\trss_mb\n' > $out/mem.tsv
while kill -0 $wpid 2>/dev/null; do
    t=$(( $(date +%s) - start ))
    ps -axo pid=,rss=,command= | grep -i 'Gothic3Final.exe' | grep -v grep | \
        awk -v t=$t '{printf "%d\t%d\t%.0f\n", t, $1, $2/1024}' >> $out/mem.tsv
    if [ -f $out/shot ]; then mv $out/shot $out/shot.taken; shot $t; fi
    if [ $every -gt 0 ] && [ $shots -lt $max ] && [ $((t - last)) -ge $every ]; then last=$t; shot $t; fi
    sleep 5
done
date "+end %F %T after $(( $(date +%s) - start )) s, $shots screenshots" >> $out/run.txt
tail -1 $out/run.txt
