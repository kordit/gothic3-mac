#!/bin/sh
# Gothic 3 na Maca: instalacja jednym poleceniem.
#   curl -fsSL https://raw.githubusercontent.com/kordit/gothic3-mac/main/install.sh | sh
# Pobiera najnowsze wydanie, sprawdza podpis aplikacji, kładzie ją w /Applications (albo ~/Applications,
# gdy do /Applications nie da się pisać) i uruchamia. Plik pobrany przez curl nie dostaje kwarantanny,
# więc macOS nie blokuje aplikacji, która nie jest notaryzowana przez Apple.
set -eu
repo=kordit/gothic3-mac
url="https://github.com/$repo/releases/latest/download/Gothic3Recomp-macOS.zip"
app=Gothic3Recomp.app

if [ "$(uname -s)" != Darwin ] || [ "$(uname -m)" != arm64 ]; then
    echo "Potrzebny jest Mac z procesorem Apple (M1 albo nowszy)." >&2
    exit 1
fi
dest=${GOTHIC3_INSTALL_DIR:-/Applications}
if [ ! -w "$dest" ]; then
    dest="$HOME/Applications"
    mkdir -p "$dest"
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
echo "Pobieranie Gothic 3 na Maca..."
curl -fL --proto =https --proto-redir =https --progress-bar -o "$tmp/app.zip" "$url"
ditto -x -k "$tmp/app.zip" "$tmp/new"
if ! codesign --verify --deep --strict "$tmp/new/$app" 2>/dev/null; then
    echo "Pobrana aplikacja ma uszkodzony podpis, przerywam." >&2
    exit 1
fi
if [ -d "$dest/$app" ]; then
    mv "$dest/$app" "$tmp/old.app" # the previous version goes with the temporary folder
fi
ditto "$tmp/new/$app" "$dest/$app"
xattr -dr com.apple.quarantine "$dest/$app" 2>/dev/null || true
echo "Zainstalowane: $dest/$app"
if [ -z "${GOTHIC3_NO_OPEN:-}" ]; then
    open "$dest/$app"
fi
