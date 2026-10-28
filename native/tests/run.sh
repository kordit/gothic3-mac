#!/bin/sh
# native/tests/run.sh - build and run one differential test of a native group against the
# translated originals.
#
#   sh native/tests/run.sh strings      # native/tests/strings_test.c
#   sh native/tests/run.sh memory       # native/tests/memory_test.c
#
# Compiles the generated chunks that hold the originals (from the main checkout's
# build/recomp/gen, or $G3_GEN; the test maps the modules from stage/, or $G3_STAGE, for their
# jump tables and data), the group's native file, kit/tools/recomp/tests/harness.c
# (owns g_mem) and the test, all with the game's override header, so FN_<addr> call sites in the
# chunks reach g3n_<addr> exactly as in the game build. Every symbol the chunks name but
# nothing here defines gets a stub: the hook flags and call caches as zeroed data, functions
# as an abort that names them. Objects go to $TMPDIR/g3native-tests/<name>, nothing into build/.
set -eu
name=$1
here=$(cd "$(dirname "$0")/.." && pwd)            # native/
repo=$(cd "$here/.." && pwd)
# build/ and stage/ are not in git: a worktree uses the main checkout's
main="$repo"
[ -d "$main/build/recomp/gen" ] || main="$(git -C "$repo" worktree list --porcelain | sed -n '1s/^worktree //p')"
gen=${G3_GEN:-$main/build/recomp/gen}
stage=${G3_STAGE:-$main/stage}
kit="$repo/kit"
out="${TMPDIR:-/tmp}/g3native-tests/$name"
mkdir -p "$out"

case $name in
strings)
    native=strings.c
    chunks="aux-sharedbase/chunk_1006c000.c aux-sharedbase/chunk_10000000.c aux-sharedbase/chunk_10004000.c"
    ;;
memory)
    native=memory.c
    chunks="aux-sharedbase/chunk_10078000.c aux-sharedbase/chunk_10068000.c aux-sharedbase/chunk_10070000.c
            aux-engine/chunk_10568000.c aux-engine/chunk_10564000.c aux-engine/chunk_1056c000.c
            aux-game/chunk_10af0000.c aux-game/chunk_10ae8000.c aux-game/chunk_10aec000.c
            aux-gfc/chunk_10ddc000.c aux-gfc/chunk_10dd8000.c
            aux-gui/chunk_10e34000.c aux-gui/chunk_10e2c000.c
            aux-script/chunk_10f58000.c aux-script/chunk_10f4c000.c"
    ;;
*)
    echo "usage: $0 strings|memory" >&2
    exit 2
    ;;
esac

cc() {
    xcrun clang -std=c11 -O1 -w -I "$gen" -I "$kit" -I "$kit/runtime" -I "$here" -DRECOMP_STORE_HOOKS=0 \
        -DRECOMP_OVERRIDE_HEADER="\"$here/g3_overrides.h\"" -DG3_STAGE="\"$stage\"" "$@"
}

objs=""
for ch in $chunks; do
    o="$out/$(echo "$ch" | tr / _).o"
    if [ ! -f "$o" ] || [ "$gen/$ch" -nt "$o" ] || [ "$gen/x86.h" -nt "$o" ]; then
        cc -c "$gen/$ch" -o "$o"
    fi
    objs="$objs $o"
done
cc -c "$kit/tools/recomp/tests/harness.c" -o "$out/harness.o"
cc -c "$here/$native" -o "$out/native.o"
cc -c "$here/tests/${name}_test.c" -o "$out/test.o"
objs="$objs $out/harness.o $out/native.o $out/test.o"

# stubs for what nothing defines
# shellcheck disable=SC2086
nm -gU $objs 2>/dev/null | awk 'NF==3{print $3}' | sort -u > "$out/defined.txt"
# shellcheck disable=SC2086
nm -u $objs 2>/dev/null | grep '^_' | sort -u > "$out/undefined.txt"
comm -23 "$out/undefined.txt" "$out/defined.txt" | awk '
    BEGIN { print "#include <stdio.h>\n#include <stdlib.h>\nstatic void stub_hit(const char *s) { fprintf(stderr, \"stub called: %s\\n\", s); abort(); }" }
    {
        s = substr($1, 2)
        if (s !~ /^(recomp_|fn_|g3n_|g_)/)
            next  # libc and libm
        if (s ~ /_hk_/)
            printf "unsigned char %s;\n", s
        else if (s == "recomp_frame_watch" || s == "recomp_ic_tables" || s == "recomp_return_hits")
            printf "unsigned char %s[1u << 20] __attribute__((aligned(64)));\n", s
        else
            printf "void %s(void) { stub_hit(\"%s\"); }\n", s, s
    }' > "$out/stubs.c"
xcrun clang -std=c11 -O1 -w -c "$out/stubs.c" -o "$out/stubs.o"
# shellcheck disable=SC2086
xcrun clang $objs "$out/stubs.o" -o "$out/${name}_test"
"$out/${name}_test"
