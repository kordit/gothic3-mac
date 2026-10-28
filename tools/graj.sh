#!/bin/sh
# Gra gracza: build z build/ skopiowany poza repo (Gothic 3 Native/test-app), więc jego profil, zapisy, mody,
# Waste Data i ustawienia; log w ~/Library/Application Support/Gothic 3 Native/logs/game.log. Bez parametrów:
# ta sama konfiguracja, którą mierzą benche (G3OPT_DEFAULT w native/g3_opt.c).
# Każda gra zapisuje stosy wątku gry z klatek ponad 45 ms do work/gracz-<data>/ (streszczenie:
# python3 tools/spike_report.py work/gracz-<data>).
#   sh tools/graj.sh            gra
#   sh tools/graj.sh --wgraj    najpierw kopiuje build/Gothic3Recomp.app do test-app
cd "$(dirname "$0")/.."
app="$HOME/Library/Application Support/Gothic 3 Native/test-app/Gothic3Recomp.app"
if [ "$1" = "--wgraj" ]; then
  rm -rf "$app" && mkdir -p "$(dirname "$app")" && ditto build/Gothic3Recomp.app "$app" || exit 1
fi
out="$PWD/work/gracz-$(date +%m%d-%H%M)"
mkdir -p "$out"
nm -n "$app/Contents/MacOS/Gothic3Recomp" > "$out/syms.txt"
exec /usr/bin/open -n --env RECOMP_HOST_PROFILE="$out/s" --env RECOMP_HOST_PROFILE_SPIKE=45 "$app"
