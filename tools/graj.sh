#!/bin/sh
# Testowa gra na Twojej konfiguracji: kopia builda z build/ poza repo (Gothic 3 Native/test-app), więc Twój profil,
# zapisy, mody, Waste Data i ustawienia, log w ~/Library/Application Support/Gothic 3 Native/logs/game.log.
#   sh tools/graj.sh none   wszystkie natywne optymalizacje wyłączone
#   sh tools/graj.sh all    jak zwykle
exec /usr/bin/open -n --env RECOMP_G3OPT="${1:-none}" "$HOME/Library/Application Support/Gothic 3 Native/test-app/Gothic3Recomp.app"
