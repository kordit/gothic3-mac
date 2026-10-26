#!/bin/sh
# tools/launcher_shot.sh <app> <out-dir> <label>: screenshots of the launcher without a window.
# SDL's dummy video driver, no GPU device (the launcher draws in software), a throwaway profile in
# <out-dir>/profile-<label> (never the player's), the game in stage/. The window is 1280x1000 points drawn at
# 2x: 2560x2000 pixels (SIZE=<w>x<h> and DENSITY=<n> change that). Writes <out-dir>/<label>-main.png
# (the main screen) and <label>-mods.png (the mod screen with the first preset applied and World
# Texture Mix selected), and <label>.log. The catalog is $RECOMP_SEFARIS_DIR or game.toml's.
TOOLS=$(cd "$(dirname "$0")" && pwd)
ROOT=${G3_ROOT:-$(dirname "$TOOLS")}
app=$1
out=$2
label=$3
if [ -z "$app" ] || [ -z "$out" ] || [ -z "$label" ]; then
    sed -n 2,8p "$0"
    exit 2
fi
size=${SIZE:-2560x2000}
density=${DENSITY:-2}
profile="$out/profile-$label"
mkdir -p "$profile"
exe="$app/Contents/MacOS/$(basename "$app" .app)"
keys="wait,wait,dump:$out/$label-main.ppm,id:5000,id:7000,select:World Texture,wait,wait,wait,wait,wait,dump:$out/$label-mods.ppm,quit"
SDL_VIDEO_DRIVER=dummy RECOMP_GPU_BACKEND=none RECOMP_LAUNCHER=1 RECOMP_LAUNCHER_SIZE="$size" \
  RECOMP_LAUNCHER_DENSITY="$density" RECOMP_PROFILE_DIR="$profile" \
  RECOMP_EXE="$ROOT/stage/Gothic3Final.exe" RECOMP_LAUNCHER_KEYS="$keys" \
  "$exe" > "$out/$label.log" 2>&1
for s in main mods; do
    if [ -f "$out/$label-$s.ppm" ]; then
        sips -s format png "$out/$label-$s.ppm" --out "$out/$label-$s.png" > /dev/null
        echo "$out/$label-$s.png"
    else
        echo "no $label-$s picture; see $out/$label.log" >&2
    fi
done
