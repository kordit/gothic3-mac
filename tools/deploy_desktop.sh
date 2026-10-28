#!/bin/sh
# tools/deploy_desktop.sh: put build/Gothic3Recomp.app where the player starts it, ~/Desktop/Gothic3Recomp.app.
# The saves are not in the app: it reads ~/Library/Application Support/Gothic 3 Native (kit/runtime/layout.cpp,
# outside a checkout), and replacing the app never touches that folder. Still, every deploy first copies
# Documents/gothic3 (the saves) to ~/Documents/gothic3-save-backups/<date-time>; nothing is ever removed there.
# Refuses while the game runs.
set -e
cd "$(dirname "$0")/.."
app=build/Gothic3Recomp.app
dest="$HOME/Desktop/Gothic3Recomp.app"
profile="$HOME/Library/Application Support/Gothic 3 Native"
[ -x "$app/Contents/MacOS/Gothic3Recomp" ] || { echo "no $app: build it first (tools/build.py)"; exit 1; }
if pgrep -x Gothic3Recomp > /dev/null; then
  echo "the game is running (PID $(pgrep -x Gothic3Recomp | tr '\n' ' ')): close it first"; exit 1
fi
if [ -d "$profile/Documents/gothic3" ]; then
  backup="$HOME/Documents/gothic3-save-backups/$(date +%Y-%m-%d_%H%M%S)"
  mkdir -p "$backup"
  cp -Rp "$profile/Documents/gothic3/" "$backup/"
  echo "saves copied to $backup ($(ls "$backup" | wc -l | tr -d ' ') files)"
fi
mkdir -p "$dest"
rsync -a --delete "$app/" "$dest/"
codesign --verify --deep --strict "$dest"
# Outside the repo the app must use the player's profile (developer=0), not build/recomp/profile.
"$dest/Contents/MacOS/Gothic3Recomp" --probe-layout | grep -q '^developer=0' || { echo "$dest runs as a developer build"; exit 1; }
echo "deployed to $dest ($("$dest/Contents/MacOS/Gothic3Recomp" --version | head -1))"
