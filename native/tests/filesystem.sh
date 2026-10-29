#!/bin/sh
# native/tests/filesystem.sh [game folder] - native/filesystem_core.cpp without the game (docs/PLAN.md step 1.3).
#
# Builds native/tests/filesystem_test.cpp, then for ExtendedContent off and on compares its walk of every mounted
# folder (path, size, attributes, the archive or disk an open gets) with tools/g3pak.py index, and runs the write
# calls on a temporary folder, and checks that finds without wildcards answer the same through the archives' maps as
# through the walk over every name, and reads every entry of the archives and of their unpacked copies (if there are
# any), the stored ones also through EntryBytes. The game folder is read only; the default is stage/. Output goes to
# $TMPDIR.
set -e
here=$(cd "$(dirname "$0")" && pwd)
wt=$(cd "$here/../.." && pwd)
game=${1:-$wt/stage}
out=${TMPDIR:-/tmp}/g3fs-test
mkdir -p "$out"
xcrun clang -std=c11 -O2 -c "$wt/native/filesystem_match.c" -o "$out/match.o"
xcrun clang++ -std=c++17 -O2 -Wall -I "$wt/native" "$here/filesystem_test.cpp" "$wt/native/filesystem_core.cpp" \
    "$out/match.o" /opt/homebrew/lib/libdeflate.a -o "$out/filesystem_test"
status=0
for ext in 0 1; do
    "$out/filesystem_test" index "$game" $ext >"$out/native-$ext.tsv"
    "$wt/.venv/bin/python" "$wt/tools/g3pak.py" index "$game" $ext >"$out/g3pak-$ext.tsv"
    if cmp -s "$out/native-$ext.tsv" "$out/g3pak-$ext.tsv"; then
        echo "index, ExtendedContent $ext: $(wc -l <"$out/native-$ext.tsv" | tr -d ' ') files, the same"
    else
        echo "index, ExtendedContent $ext: differs (diff $out/native-$ext.tsv $out/g3pak-$ext.tsv)"
        diff "$out/native-$ext.tsv" "$out/g3pak-$ext.tsv" | head -20
        status=1
    fi
done
find "$game/Data" -maxdepth 1 \( -iname '*.pak' -o -iname '*.p0?' -o -iname '*.cpt' -o -iname '*.c0?' \
    -o -iname '*.mod' -o -iname '*.m0?' -o -iname '*.nod' -o -iname '*.n0?' \) -print0 |
    xargs -0 "$out/filesystem_test" read || status=1
# The unpacked copies tools/g3stored.py made (D-046), read only: every entry stored, so all go through EntryBytes.
stored=$HOME/Library/Caches/Gothic3Recomp/stored
if ls "$stored"/*.g3s >/dev/null 2>&1; then
    "$out/filesystem_test" read "$stored"/*.g3s || status=1
fi
for ext in 0 1; do
    "$out/filesystem_test" names "$game" $ext || status=1
done
scratch=$(mktemp -d "$out/scratch.XXXXXX")
rmdir "$scratch"
"$out/filesystem_test" scratch "$scratch" || status=1
exit $status
