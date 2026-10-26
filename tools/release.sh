#!/bin/sh
# tools/release.sh <version> [--publish]
#
# Builds Gothic3Recomp.app at <version> (numbers and dots, e.g. 0.1.0) and writes build/release/<version>/:
#   Gothic3Recomp-macOS.zip   the app, which the launcher's update installs (game.toml releases_repo)
#   Gothic3-Mac-<version>.dmg the app and a link to Applications, for a first install
#   SHA256SUMS
# --publish then creates the GitHub release v<version> in game.toml's releases_repo with both files. The
# launcher finds updates only in a public repository: GitHub answers an anonymous request for a private
# one with 404.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
version=${1:-}
publish=${2:-}
case "$version" in
  '' | *[!0-9.]* | .* | *.) echo "usage: tools/release.sh <version, e.g. 0.1.0> [--publish]" >&2; exit 2 ;;
esac
repo=$(sed -n 's/^releases_repo = "\(.*\)"$/\1/p' "$root/game.toml")
app="$root/build/Gothic3Recomp.app"
out="$root/build/release/$version"

cd "$root"
.venv/bin/python tools/build.py --target app --version "$version"
shown=$(defaults read "$app/Contents/Info.plist" CFBundleShortVersionString)
[ "$shown" = "$version" ] || { echo "the app says version $shown, not $version" >&2; exit 1; }
[ -x "$app/Contents/MacOS/DepotDownloader" ] || { echo "the app carries no DepotDownloader" >&2; exit 1; }
codesign --verify --deep --strict "$app"

mkdir -p "$out"
rm -f "$out/Gothic3Recomp-macOS.zip" "$out/Gothic3-Mac-$version.dmg"
ditto -c -k --keepParent "$app" "$out/Gothic3Recomp-macOS.zip"

dmg_root=$(mktemp -d "$root/work/dmg.XXXXXX")
trap 'rm -rf "$dmg_root"' EXIT
ditto "$app" "$dmg_root/Gothic3Recomp.app"
ln -s /Applications "$dmg_root/Applications"
cp "$root/README.md" "$dmg_root/README.md"
hdiutil create -volname "Gothic 3 Mac $version" -srcfolder "$dmg_root" -ov -format UDZO \
  "$out/Gothic3-Mac-$version.dmg" >/dev/null

(cd "$out" && shasum -a 256 Gothic3Recomp-macOS.zip "Gothic3-Mac-$version.dmg" > SHA256SUMS)
cat "$out/SHA256SUMS"
du -h "$out"/*

if [ "$publish" = "--publish" ]; then
  [ -n "$repo" ] || { echo "game.toml has no releases_repo" >&2; exit 1; }
  gh release create "v$version" "$out/Gothic3Recomp-macOS.zip" "$out/Gothic3-Mac-$version.dmg" \
    "$out/SHA256SUMS" --repo "$repo" --title "Gothic 3 for Mac $version" --notes-file "$root/docs/release-notes.md"
fi
