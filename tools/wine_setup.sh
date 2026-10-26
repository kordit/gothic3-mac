#!/bin/sh
# tools/wine_setup.sh: the original Gothic 3 under Wine on this Mac, as the reference for the port and
# the Windows test bed. Idempotent: every step skips what is already in place.
#
#   ~/g3-wine/dl/       the Wine tarball (checked against its sha256)
#   ~/g3-wine/wine/     Wine Staging.app
#   ~/g3-wine/prefix/   the Wine prefix; the game in drive_c/Program Files/Gothic 3
#
# The game is an APFS clone of original-cp175 (cp -c: no extra disk, writes never reach the source),
# plus d3dx9_40.dll from the DirectX redist cab, as in stage.toml. Run it with tools/wine_run.sh.
set -eu
cd "$(dirname "$0")/.."
here=$(pwd)
root=$HOME/g3-wine
ver=11.18
tarball=wine-staging-$ver-osx64.tar.xz
tar_sha=b63704b91af269bc026a87f12bd297c4a50caaf570c322e600b6621ef918f127
cab=redist/DirectX/Nov2008_d3dx9_40_x86.cab
cab_sha=5f6dc0268e4dded33787348992883751e25cdf68fb7d4e6004ba15bc63a26afb
src=$here/original-cp175
bin="$root/wine/Wine Staging.app/Contents/Resources/wine/bin"
export WINEPREFIX="$root/prefix" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
game="$WINEPREFIX/drive_c/Program Files/Gothic 3"

check_sha() { [ "$(shasum -a 256 "$1" | cut -d' ' -f1)" = "$2" ] || { echo "sha256 mismatch: $1" >&2; exit 1; }; }

echo "== wine $ver"
mkdir -p "$root/dl" "$root/wine"
if [ ! -f "$root/dl/$tarball" ]; then
    curl -fL -o "$root/dl/$tarball.part" \
        "https://github.com/Gcenx/macOS_Wine_builds/releases/download/$ver/$tarball"
    mv "$root/dl/$tarball.part" "$root/dl/$tarball"
fi
check_sha "$root/dl/$tarball" $tar_sha
[ -x "$bin/wine" ] || tar -xJf "$root/dl/$tarball" -C "$root/wine"
"$bin/wine" --version

echo "== prefix"
[ -f "$WINEPREFIX/system.reg" ] || "$bin/wineboot" -i
"$bin/wineserver" -w

echo "== game"
mkdir -p "$(dirname "$game")"
[ -f "$game/Gothic3Final.exe" ] || cp -c -R "$src" "$game"
rm -rf "$game/steamapps"
if [ ! -f "$game/d3dx9_40.dll" ]; then
    check_sha "$src/$cab" $cab_sha
    tmp=$(mktemp -d)
    tar -xf "$src/$cab" -C "$tmp" d3dx9_40.dll
    mv "$tmp/d3dx9_40.dll" "$game/"
    rmdir "$tmp"
fi

echo "== polish"
"$here/.venv/bin/python" -I tools/wine_polish.py "$game" "$HOME/Documents/Gothic3"

echo "== verify"
bad=0
(cd "$src" && find . -iname '*.exe' -o -iname '*.dll' | grep -v '^./steamapps/' | grep -v '^./redist/') | while read -r f; do
    a=$(shasum -a 256 "$src/$f" | cut -d' ' -f1); b=$(shasum -a 256 "$game/$f" | cut -d' ' -f1)
    [ "$a" = "$b" ] || { echo "DIFF $f"; exit 1; }
done || bad=1
n=$(cd "$game" && find . -iname '*.exe' -o -iname '*.dll' | grep -vc '^./redist/')
[ $bad = 0 ] && echo "ok: $n exe/dll in the game folder, all but d3dx9_40.dll and Speech_Polish.pak identical to original-cp175"
