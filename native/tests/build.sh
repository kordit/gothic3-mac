#!/bin/sh
# native/tests/build.sh <name> <generated chunk>... - builds and runs native/tests/<name>_test.c.
#
# Links the test against the translated originals: the named generated chunks (paths relative to
# the main repo's build/recomp/gen/), the kit's test harness (owns g_mem), the native group files
# the test names in NATIVE (default: <name>.c) and native/g3_opt.c, all compiled with the game's
# override header so FN_<addr> goes to g3n_<addr> exactly as in the game build. Every symbol none of
# those define (the thousands of fn_ the chunks call but the test never reaches) gets a stub that
# aborts with its name. Objects go to $G3N_OUT (default $TMPDIR/g3n-tests); the main repo is the
# one this checkout belongs to (git common dir), or $G3_MAIN. G3N_OPT=2 compiles as the game build does
# (timings with G3N_BENCH=1); G3N_CFLAGS adds flags, e.g. -DRECOMP_CTX_RESTRICT= for an A/B of the translation.
#
#   sh native/tests/build.sh rand aux-sharedbase/chunk_1006c000.c aux-sharedbase/chunk_10030000.c
set -e
name=$1
shift
here=$(cd "$(dirname "$0")" && pwd)
wt=$(cd "$here/../.." && pwd)
main=${G3_MAIN:-$(cd "$(git -C "$wt" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)}
export G3_MAIN="$main"
gen="$main/build/recomp/gen"
out=${G3N_OUT:-${TMPDIR:-/tmp}/g3n-tests}/$name
mkdir -p "$out"
native=${NATIVE:-$name.c}

cc() {
    xcrun clang -std=c11 -O${G3N_OPT:-1} -g -Wall -Wno-unused -Wno-unused-label -Wno-unused-variable \
        -Wno-unused-but-set-variable -Wno-parentheses-equality -Wno-unused-function \
        -I "$gen" -I "$main/kit" -I "$main/kit/runtime" -DRECOMP_NO_HOOKS \
        -DRECOMP_OVERRIDE_HEADER="\"$wt/native/g3_overrides.h\"" ${G3N_CFLAGS:-} "$@"
}

objs=""
for ch in "$@"; do
    o="$out/$(echo "$ch" | tr '/' '_').o"
    # the chunk names g3n_ through the override header, so a changed header rebuilds it too
    if [ ! -f "$o" ] || [ "$gen/$ch" -nt "$o" ] || [ "$wt/native/g3_overrides.h" -nt "$o" ]; then
        cc -w -c "$gen/$ch" -o "$o"
    fi
    objs="$objs $o"
done
cc -w -c "$main/kit/tools/recomp/tests/harness.c" -o "$out/harness.o"
cc -c "$wt/native/g3_opt.c" -o "$out/g3_opt.o"
nobjs=""
for n in $native; do
    cc -c "$wt/native/$n" -o "$out/native_$n.o"
    nobjs="$nobjs $out/native_$n.o"
done
cc -c "$here/${name}_test.c" -o "$out/test.o"

all="$objs $out/harness.o $out/g3_opt.o $nobjs $out/test.o"
# shellcheck disable=SC2086
nm -u $all | grep -v ':$' | sed 's/^ *//' | sort -u >"$out/undef.txt"
# shellcheck disable=SC2086
nm -g --defined-only $all | grep -v ':$' | awk 'NF==3{print $3}' | sort -u >"$out/def.txt"
comm -23 "$out/undef.txt" "$out/def.txt" | grep -v '^$' >"$out/missing.txt" || true
{
    echo '#include <stdio.h>'
    echo '#include <stdlib.h>'
    echo '#include <stdint.h>'
    while read -r s; do
        n=${s#_}
        case "$n" in
        recomp_frame_watch | recomp_return_hits | recomp_ic_tables | recomp_trace_game_hook) ;;
        fn_* | g3n_* | g3x_* | g3_console_control | g3_mods_control | recomp_*) printf "%s\n" "void $n(void) { puts(\"stub reached: $n\"); abort(); }" ;;
        *) ;; # libc and libm
        esac
    done <"$out/missing.txt"
    grep -qx '_recomp_frame_watch' "$out/missing.txt" && echo 'uint32_t recomp_frame_watch;'
    grep -qx '_recomp_return_hits' "$out/missing.txt" && echo 'uint32_t recomp_return_hits[1u << 14];'
    grep -qx '_recomp_ic_tables' "$out/missing.txt" && echo 'const void *recomp_ic_tables[256];'
    grep -qx '_recomp_trace_game_hook' "$out/missing.txt" && echo 'void (*recomp_trace_game_hook)(const char *);'
    true
} >"$out/stubs.c"
xcrun clang -w -c "$out/stubs.c" -o "$out/stubs.o"
# shellcheck disable=SC2086
xcrun clang $all "$out/stubs.o" -o "$out/$name"
"$out/$name"
