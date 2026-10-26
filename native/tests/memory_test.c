/* native/tests/memory_test.c - native/memory.c against the translated CRT copies of all seven
 * modules.
 *
 * Build and run (from the repository root or a worktree):
 *   sh native/tests/run.sh memory
 *
 * Every size 0-64 at every alignment pair mod 4, disjoint and overlapping by -12..+12 bytes;
 * sizes around and above the SSE2 threshold (256) with overlaps in both directions and the
 * alignments that pick or skip _VEC_memcpy, with each module's __sse2_available 0 and 1;
 * random sizes and overlaps. Compares EAX, ESP, EIP, EBX ESI EDI EBP, the copied window and the
 * stack above the final ESP. */
#include "difftest.h"

#define COPY_FN(addr) void fn_##addr(X86 *c);
COPY_FN(10078780)
COPY_FN(1006bba0)
COPY_FN(1056ad40)
COPY_FN(10564220)
COPY_FN(10db3620)
COPY_FN(10db0050)
COPY_FN(10af1980)
COPY_FN(10ae9d80)
COPY_FN(10dde620)
COPY_FN(10dd99b0)
COPY_FN(10e34ef0)
COPY_FN(10e2d980)
COPY_FN(10f58ef0)
COPY_FN(10f4d5a0)

static const struct {
    const char *name;
    guest_fn orig, native;
    uint32_t sse2; /* the module's __sse2_available */
} fns[] = {
    {"SharedBase memcpy", fn_10078780, g3n_10078780, 0x102442c4u},
    {"SharedBase memmove", fn_1006bba0, g3n_1006bba0, 0x102442c4u},
    {"Engine memcpy", fn_1056ad40, g3n_1056ad40, 0x10879a7cu},
    {"Engine memmove", fn_10564220, g3n_10564220, 0x10879a7cu},
    {"FileSystem memcpy", fn_10db3620, g3n_10db3620, 0x10dc857cu},
    {"FileSystem memmove", fn_10db0050, g3n_10db0050, 0x10dc857cu},
    {"Game memcpy", fn_10af1980, g3n_10af1980, 0x10d1886cu},
    {"Game memmove", fn_10ae9d80, g3n_10ae9d80, 0x10d1886cu},
    {"GFC memcpy", fn_10dde620, g3n_10dde620, 0x10df1ee4u},
    {"GFC memmove", fn_10dd99b0, g3n_10dd99b0, 0x10df1ee4u},
    {"GUI memcpy", fn_10e34ef0, g3n_10e34ef0, 0x10e6cadcu},
    {"GUI memmove", fn_10e2d980, g3n_10e2d980, 0x10e6cadcu},
    {"Script memcpy", fn_10f58ef0, g3n_10f58ef0, 0x10f98184u},
    {"Script memmove", fn_10f4d5a0, g3n_10f4d5a0, 0x10f98184u},
};
#define NFNS (sizeof fns / sizeof fns[0])

/* Each module's _VEC_memcpy, the SSE2 path, is entered through a tail call whose hook flag the
 * test sets, so the hook counts that the originals really took the path. */
#define VEC_FN(key, addr)                                                                          \
    extern unsigned char recomp_##key##_hk_##addr;                                                 \
    void fn_##addr(X86 *c);                                                                        \
    void recomp_##key##_hook_call(X86 *c, uint32_t a) {                                            \
        if (a != 0x##addr##u) {                                                                    \
            fprintf(stderr, "unexpected hook %08x\n", a);                                          \
            abort();                                                                               \
        }                                                                                          \
        vec_calls++;                                                                               \
        fn_##addr(c);                                                                              \
    }
static unsigned long vec_calls;
VEC_FN(sharedbase, 100722a0)
VEC_FN(engine, 1056db01)
VEC_FN(filesystem, 10db132f)
VEC_FN(game, 10aed63e)
VEC_FN(gfc, 10ddb185)
VEC_FN(gui, 10e2efca)
VEC_FN(script, 10f4f4c5)

#define DATA 0x01000000u /* 16 MB in, well clear of the stack and the modules' data */

/* The widest span (dst 1 MB below src, 200000 bytes) is 3.4 MB, read from up to 1 MB in. */
static uint8_t noise[5u << 20];

static void one(unsigned f, uint32_t dst, uint32_t src, uint32_t n, uint32_t sse2) {
    wr32(fns[f].sse2, sse2);
    uint32_t lo = (dst < src ? dst : src) - 64u;
    uint32_t hi = (dst > src ? dst : src) + n + 64u;
    memcpy(g_mem + lo, noise + rnd_below(1u << 20), hi - lo);
    uint32_t args[3] = {dst, src, n};
    X86 c = call_state(args, 3);
    char what[96];
    snprintf(what, sizeof what, "%s dst %08x src %08x n %u sse2 %u", fns[f].name, dst, src, n, sse2);
    diff_run(what, fns[f].orig, fns[f].native, &c, CALLEE_SAVED | REG(R_EAX), lo, hi);
}

int main(void) {
    harness_mem();
    static const char *dlls[] = {"SharedBase.dll", "Engine.dll", "FileSystem.dll", "Game.dll",
                                 "GFC.dll",        "GUI.dll",    "Script.dll"};
    for (unsigned i = 0; i < sizeof dlls / sizeof dlls[0]; ++i)
        map_module(dlls[i]);
    for (size_t i = 0; i < sizeof noise; ++i)
        noise[i] = (uint8_t)rnd();
    recomp_sharedbase_hk_100722a0 = recomp_engine_hk_1056db01 = recomp_filesystem_hk_10db132f = 1;
    recomp_game_hk_10aed63e = recomp_gfc_hk_10ddb185 = recomp_gui_hk_10e2efca = 1;
    recomp_script_hk_10f4f4c5 = 1;
    for (unsigned f = 0; f < NFNS; ++f) {
        /* small: every size and alignment, disjoint and close overlaps */
        for (uint32_t n = 0; n <= 64; ++n)
            for (uint32_t da = 0; da < 4; ++da)
                for (uint32_t sa = 0; sa < 4; ++sa) {
                    one(f, DATA + 0x10000u + da, DATA + 0x20000u + sa, n, 0);
                    for (int d = -12; d <= 12; ++d) {
                        uint32_t src = DATA + 0x30000u + sa;
                        one(f, src + (uint32_t)d + da, src, n, 0);
                    }
                }
        /* large: the SSE2 threshold, overlaps both ways, alignments mod 16 */
        static const uint32_t sizes[] = {255, 256, 257, 300, 1000, 4096 + 13, 65536 + 7, 200000};
        static const uint32_t aligns[][2] = {{0, 0}, {1, 1}, {3, 3}, {0, 1}, {1, 0}, {15, 15}, {8, 0}};
        for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; ++s) {
            const uint32_t n = sizes[s];
            const int32_t deltas[] = {1, 3, 4, 15, 16, 17, 32, 128, (int32_t)n / 2, (int32_t)n - 1};
            for (unsigned al = 0; al < sizeof aligns / sizeof aligns[0]; ++al)
                for (uint32_t sse2 = 0; sse2 < 2; ++sse2) {
                    const uint32_t src = DATA + 0x400000u + aligns[al][1];
                    one(f, DATA + 0x100000u + aligns[al][0], src, n, sse2);
                    for (unsigned k = 0; k < sizeof deltas / sizeof deltas[0]; ++k) {
                        uint32_t d = (uint32_t)deltas[k] + aligns[al][0] - aligns[al][1];
                        one(f, src + d, src, n, sse2);
                        one(f, src - d, src, n, sse2);
                    }
                }
        }
        /* random */
        for (int i = 0; i < 20000; ++i) {
            uint32_t n = rnd_below(8) ? rnd_below(600) : rnd_below(5000);
            uint32_t src = DATA + 0x800000u + rnd_below(64);
            uint32_t dst = rnd_below(3) ? src + rnd_below(2 * n + 64) - (n + 32) : DATA + 0xc00000u + rnd_below(64);
            one(f, dst, src, n, rnd_below(2));
        }
    }
    printf("_VEC_memcpy entered %lu times by the originals\n", vec_calls);
    if (!vec_calls)
        return 1;
    return diff_report("memory");
}
