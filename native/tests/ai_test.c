/* native/tests/ai_test.c - native/ai.c's _strrchr against the translation.
 *
 * Build and run (from the checkout root):
 *   sh native/tests/build.sh ai aux-sharedbase/chunk_10068000.c
 * (the chunk holds SharedBase _strrchr). G3N_BENCH=1 adds timings. The FileSystem cases (FUN_10da9e10,
 * FUN_10da29f0) went with the translated FileSystem.dll (D-043); its name match is native/tests/fsmatch_test.c's.
 *
 * Every case runs the original and the native from the same state (random registers, x87, XMM) and compares
 * registers, x87 state, XMM and memory bitwise (the stack from the final ESP up). Strings are drawn so that every path
 * is taken: '/' and '\\' found or not, the NUL as the character, garbage above its low byte, bytes >= 0x80, a start
 * inside the string, long strings. */
#include "difftest_rp.h"

#include <time.h>

void fn_1006b740(X86 *c);
void g3n_1006b740(X86 *c);

#define ESP0 0x0e800000u
#define STR 0x03002000u       /* strrchr's string */
#define RET_SITE2 0x1000456cu /* ReverseFind's continuation after CALL _strrchr */

static unsigned paths[16];

static void put_str(uint32_t at, const uint8_t *s, uint32_t n) {
    memcpy(g_mem + at, s, n + 1u);
    /* bytes after the NUL that a wrong reader would pick up */
    for (uint32_t k = n + 1u; k < n + 9u; ++k)
        g_mem[at + k] = (uint8_t)("q.Q*x?.z"[k & 7u]);
}

static void test_strrchr(unsigned cases) {
    uint8_t s[0x400];
    for (unsigned i = 0; i < cases; ++i) {
        uint32_t n = dt_rand32() % 8u ? dt_rand32() % 40u : dt_rand32() % 0x300u;
        for (uint32_t k = 0; k < n; ++k)
            s[k] = (uint8_t)("ab/c\\d./"[dt_rand32() % 8u]);
        if (dt_rand32() % 4u == 0)
            for (uint32_t k = 0; k < n; ++k)
                s[k] = (uint8_t)(1u + dt_rand32() % 0xffu);
        put_str(STR, s, n);
        X86 c;
        dt_context(&c, ESP0 - 12u, dt_rand32() % 4u);
        wr32(ESP0 - 12u, RET_SITE2);
        wr32(ESP0 - 8u, STR + (dt_rand32() % 8u == 0 && n ? dt_rand32() % n : 0u));
        uint32_t ch;
        switch (dt_rand32() % 6u) {
        case 0: ch = '/'; break;
        case 1: ch = 0; break;
        case 2: ch = n ? s[dt_rand32() % n] : 'a'; break;
        case 3: ch = dt_rand32(); break; /* garbage above the low byte */
        case 4: ch = 0xffffff00u | (n ? s[dt_rand32() % n] : 0u); break;
        default: ch = dt_rand32() & 0xffu; break;
        }
        wr32(ESP0 - 4u, ch);
        c.eflags_df = 0;
        dt_run("1006b740", &c, fn_1006b740, g3n_1006b740);
        ++paths[c.r[0] ? 4 : 5];
    }
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

/* A speech file's path cut at its last '/', as bCString::ReverseFind asks. */
static void bench(void) {
    double t[2];
    put_str(STR, (const uint8_t *)"Data/Sound/Speech/Info_Ardea_Bauer_02_07.wav", 44);
    for (int v = 0; v < 2; ++v) {
        g3_opt = v ? 0xffffffffu : 0u;
        const int n = 2000000;
        const double t0 = now();
        for (int i = 0; i < n; ++i) {
            X86 c;
            memset(&c, 0, sizeof c);
            c.r[4] = ESP0 - 12u;
            wr32(ESP0 - 12u, RET_SITE2);
            wr32(ESP0 - 8u, STR);
            wr32(ESP0 - 4u, '/');
            g3n_1006b740(&c);
        }
        t[v] = (now() - t0) * 1e9 / n;
    }
    printf("bench _strrchr, ns per call: translation %.1f, native %.1f\n", t[0], t[1]);
}

int main(void) {
    harness_mem();
    dt_load_pe("SharedBase.dll");
    dt_stack_floor = ESP0 - 0x800u;
    dt_region(dt_stack_floor, 0x900u);
    dt_region(STR, 0x400u);

    test_strrchr(200000);
    printf("paths: strrchr found/none %u/%u\n", paths[4], paths[5]);
    if (getenv("G3N_BENCH"))
        bench();
    return dt_report("ai_test");
}
