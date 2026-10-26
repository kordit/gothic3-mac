/* native/tests/shadow_test.c - native/shadow.c (Engine's qsort 10564ec0) against the translation.
 *
 * Build and run (from the checkout root):
 *   NATIVE="shadow.c exec.c" sh native/tests/build.sh shadow aux-engine/chunk_10564000.c \
 *       aux-engine/chunk_103fc000.c aux-engine/chunk_10438000.c
 * (exec.c holds the particle comparator shadow.c's qsort calls in place; its test is exec_test)
 * (the first chunk holds the translated qsort 10564ec0, shortsort 10564e30 and swap 10564e00, the
 * others the comparators 103fd240, 103fd1c0 and 1043abd0, which run for real under the original).
 * G3N_BENCH=1 adds a timing of a 256-element sort. native/g3_overrides.h does not carry qsort yet;
 * the test calls g3n_10564ec0 and fn_10564ec0 directly, so the chunks need no override.
 *
 * Cases:
 *  - the three inline comparators (ascending and descending float at (*elem)+0x18, the int at
 *    (*elem)+0x64) over pointer arrays with duplicates, repeated pointers, NaNs, signed zeros,
 *    infinities and denormals, sorted, reversed and random orders, 0 to 400 elements;
 *  - a fake guest comparator at FAKE for widths 1 to 24: it appends what it was handed (return
 *    address, ESP, EAX, ECX, EDX, both arguments, a hash of EBX, EBP, ESI, EDI, the x87 top and
 *    tags) to a log in guest memory and answers from the elements' bytes (consistent) or from a hash (an
 *    inconsistent comparator, which qsort must still follow the same way), clobbering ECX, EDX
 *    and XMM0. The log is compared memory, so a call in another order, with another argument,
 *    return address, ESP or register, or one call too many, fails the case;
 *  - num 0 and 1, base 0 with num 0, and the group switched off.
 * Every case starts from a random context (scratch registers, x87 top with live values, stale
 * tags, rounding control, XMM; difftest_rp.h), random bytes below ESP, and compares registers,
 * x87 state, XMM and memory bitwise (the stack below the final ESP excepted). */
#include "difftest_rp.h"

#include <math.h>
#include <time.h>

void fn_10564ec0(X86 *c);
void g3n_10564ec0(X86 *c); /* #define FN_10564ec0 g3n_10564ec0 in native/g3_overrides.h */
void fn_103fd240(X86 *c);
void fn_103fd1c0(X86 *c);
void fn_1043abd0(X86 *c);

#define ESP0 0x0e800000u
#define STACK_LO (ESP0 - 0x1000u)
#define ARR 0x03000000u /* the array being sorted */
#define ARR_SIZE 0x4000u
#define OBJ 0x03100000u /* objects the pointer arrays point to, 0x80 bytes each */
#define NOBJ 256u
#define LOG 0x03200000u /* the fake comparator's log: count, then 8 words per call */
#define LOG_SIZE 0x30000u
#define FAKE 0x0fd00000u
#define RET_SITE 0x10402567u /* a return site in ExecuteShadowMap */

static unsigned fake_width;
static int fake_noisy;
static uint32_t fake_seed;
static int bench_fast;

static uint32_t mix(uint32_t h, uint32_t v) {
    h ^= v + 0x7f4a7c15u + (h << 6) + (h >> 2);
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    return h ^ (h >> 16);
}

/* int comp(const void *, const void *), cdecl. */
static void fake_cmp(X86 *c) {
    const uint32_t esp = c->r[4];
    const uint32_t a = rd32(esp + 4u), b = rd32(esp + 8u);
    const uint32_t n = rd32(LOG);
    uint32_t h = mix(fake_seed, n);
    if (!bench_fast) {
        const uint32_t e = LOG + 4u + 32u * n;
        if (e + 32u > LOG + LOG_SIZE) {
            fprintf(stderr, "fake log full\n");
            abort();
        }
        uint32_t rest = mix(c->r[3], c->r[5]);
        rest = mix(rest, c->r[6]);
        rest = mix(rest, c->r[7]);
        rest = mix(rest, c->fpu_top | ((uint32_t)c->fpu_tag << 8));
        const uint32_t w[8] = {rd32(esp), esp, c->r[0], c->r[1], c->r[2], a, b, rest};
        for (unsigned i = 0; i < 8; ++i) {
            wr32(e + 4u * i, w[i]);
            h = mix(h, w[i]);
        }
    }
    wr32(LOG, n + 1u);
    int32_t r = 0;
    if (fake_noisy) {
        const uint32_t k = h % 7u;
        r = k < 3 ? -(int32_t)(1 + k) : k < 6 ? (int32_t)(k - 2) : 0;
        if ((h >> 8) % 13u == 0)
            r = (h >> 16) & 1 ? INT32_MIN : INT32_MAX;
    } else {
        for (unsigned i = 0; i < fake_width && !r; ++i)
            r = (int32_t)rd8(a + i) - (int32_t)rd8(b + i);
        if (r && (h & 3u) == 0)
            r = r < 0 ? (int32_t)(0x80000000u | h) : (int32_t)(h & 0x7fffffffu) | 1;
    }
    c->r[0] = (uint32_t)r;
    c->r[1] = mix(h, 1);
    c->r[2] = mix(h, 2);
    for (int i = 0; i < 4; ++i)
        c->xmm[0][i] = mix(h, 3u + (uint32_t)i);
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u;
}

void recomp_call_ic(X86 *c, uint32_t target, uint64_t *ic) {
    (void)ic;
    switch (target) {
    case 0x103fd240u:
        fn_103fd240(c);
        return;
    case 0x103fd1c0u:
        fn_103fd1c0(c);
        return;
    case 0x1043abd0u:
        fn_1043abd0(c);
        return;
    case FAKE:
        fake_cmp(c);
        return;
    default:
        fprintf(stderr, "unexpected indirect call to %08x\n", target);
        abort();
    }
}

static float key_float(unsigned style, unsigned i, unsigned n) {
    switch (style) {
    case 0: return dt_float();
    case 1: return (float)(dt_rand32() % 5u); /* many equal keys */
    case 2: return (float)i;                  /* sorted */
    case 3: return (float)(n - i);            /* reversed */
    case 4: return 1.0f;                      /* all equal */
    case 5: return (dt_rand32() & 3u) ? (float)(dt_rand32() % 50u) : dt_bits(0x7fc00000u); /* NaNs */
    case 6: return (dt_rand32() & 1u) ? 0.0f : -0.0f;
    default: return (float)(int32_t)dt_rand32() / 65536.0f;
    }
}

static unsigned pick_n(void) {
    switch (dt_rand32() % 8u) {
    case 0: return dt_rand32() % 4u;
    case 1:
    case 2: return 2u + dt_rand32() % 10u; /* around the shortsort cutoff */
    case 3: return 9u + dt_rand32() % 24u;
    case 4: return 300u + dt_rand32() % 101u;
    default: return dt_rand32() % 200u;
    }
}

static void stack_garbage(void) {
    for (uint32_t a = STACK_LO; a < ESP0; a += 4)
        wr32(a, dt_rand32());
}

static void call_ctx(X86 *c, uint32_t base, uint32_t num, uint32_t width, uint32_t comp) {
    dt_context(c, ESP0, dt_rand32() % 7u);
    stack_garbage();
    wr32(ESP0, RET_SITE);
    wr32(ESP0 + 4u, base);
    wr32(ESP0 + 8u, num);
    wr32(ESP0 + 12u, width);
    wr32(ESP0 + 16u, comp);
    wr32(LOG, 0u);
    fake_seed = dt_rand32();
}

static unsigned by_kind[4];

/* Pointer arrays for the inline comparators. */
static void test_inline(unsigned cases) {
    static const uint32_t comps[3] = {0x103fd240u, 0x103fd1c0u, 0x1043abd0u};
    for (unsigned k = 0; k < cases; ++k) {
        const uint32_t comp = comps[dt_rand32() % 3u];
        const unsigned n = pick_n(), style = dt_rand32() % 8u;
        const unsigned nobj = 1u + dt_rand32() % NOBJ;
        for (unsigned i = 0; i < NOBJ; ++i) {
            const uint32_t o = OBJ + 0x80u * i;
            wrf32(o + 0x18u, key_float(style, i, nobj));
            uint32_t v = dt_rand32();
            if (style == 1)
                v %= 4u;
            else if (style == 4)
                v = 7u;
            else if (style == 2)
                v = i * 3u;
            wr32(o + 0x64u, v);
        }
        for (unsigned i = 0; i < n; ++i) {
            const unsigned j = style == 2 || style == 3 ? i % nobj : dt_rand32() % nobj;
            wr32(ARR + 4u * i, OBJ + 0x80u * j);
        }
        X86 c;
        call_ctx(&c, ARR, n, 4u, comp);
        dt_run(comp == 0x1043abd0u ? "qsort int+0x64" : comp == 0x103fd240u ? "qsort asc+0x18" : "qsort desc+0x18",
               &c, fn_10564ec0, g3n_10564ec0);
        by_kind[comp == 0x1043abd0u ? 2 : comp == 0x103fd240u ? 0 : 1]++;
    }
}

/* Raw elements through the fake guest comparator. */
static void test_guest(unsigned cases) {
    static const unsigned widths[] = {1, 2, 3, 4, 4, 4, 5, 8, 12, 16, 24};
    for (unsigned k = 0; k < cases; ++k) {
        const unsigned w = widths[dt_rand32() % (sizeof widths / sizeof widths[0])];
        unsigned n = pick_n();
        if (n * w > ARR_SIZE)
            n = ARR_SIZE / w;
        fake_width = w;
        fake_noisy = (dt_rand32() % 5u) == 0;
        const unsigned style = dt_rand32() % 4u;
        for (unsigned i = 0; i < n * w; ++i) {
            uint8_t v = (uint8_t)dt_rand32();
            if (style == 1)
                v &= 3u;
            else if (style == 2)
                v = (uint8_t)(i / w);
            else if (style == 3)
                v = (uint8_t)(255u - i / w);
            wr8(ARR + i, v);
        }
        X86 c;
        call_ctx(&c, ARR, n, w, FAKE);
        dt_run(fake_noisy ? "qsort guest (noisy)" : "qsort guest", &c, fn_10564ec0, g3n_10564ec0);
        by_kind[3]++;
    }
}

static void test_edges(void) {
    X86 c;
    for (unsigned k = 0; k < 200; ++k) {
        const uint32_t num = k & 1u, base = (k & 2u) ? ARR : (num ? ARR : 0u);
        call_ctx(&c, base, num, 1u + (k % 9u), (k & 4u) ? FAKE : 0x103fd240u);
        dt_run("qsort num 0/1", &c, fn_10564ec0, g3n_10564ec0);
    }
    /* base 0 with num 0 is not an error */
    call_ctx(&c, 0u, 0u, 4u, FAKE);
    dt_run("qsort base 0 num 0", &c, fn_10564ec0, g3n_10564ec0);
}

static void test_off(unsigned cases) {
    for (unsigned k = 0; k < cases; ++k) {
        const unsigned n = pick_n();
        for (unsigned i = 0; i < n; ++i)
            wr32(ARR + 4u * i, OBJ + 0x80u * (dt_rand32() % NOBJ));
        X86 c;
        call_ctx(&c, ARR, n, 4u, 0x103fd240u);
        X86 start = c, o;
        dt_save(0);
        g3_opt = 0;
        fn_10564ec0(&c);
        o = c;
        dt_save(1);
        dt_restore();
        c = start;
        g3_opt = ~G3OPT_SHADOW;
        g3n_10564ec0(&c);
        dt_compare("qsort (shadow off)", &o, &c);
    }
    g3_opt = 0xffffffffu;
}

static double now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

/* G3N_BENCH=1: one 256-element pointer array with random keys, sorted again and again from the
 * same order (restored each time, as in the shadow pass with a still camera), through each. */
static void bench_one(const char *what, uint32_t comp, unsigned n) {
    enum { REP = 4000 };
    static uint8_t saved[ARR_SIZE];
    for (unsigned i = 0; i < NOBJ; ++i)
        wrf32(OBJ + 0x80u * i + 0x18u, (float)(dt_rand32() % 1000u) / 7.0f);
    for (unsigned i = 0; i < n * 4u; ++i)
        wr8(ARR + i, (uint8_t)dt_rand32());
    for (unsigned i = 0; i < n; ++i)
        wr32(ARR + 4u * i, OBJ + 0x80u * (dt_rand32() % NOBJ));
    fake_width = 4;
    fake_noisy = 0;
    memcpy(saved, g_mem + ARR, n * 4u);
    X86 start;
    call_ctx(&start, ARR, n, 4u, comp);
    double t[2];
    void (*const f[2])(X86 *) = {fn_10564ec0, g3n_10564ec0};
    bench_fast = 1;
    for (int k = 0; k < 2; ++k) {
        g3_opt = k ? 0xffffffffu : 0u;
        const double t0 = now_ns();
        for (unsigned rep = 0; rep < REP; ++rep) {
            memcpy(g_mem + ARR, saved, n * 4u);
            wr32(LOG, 0u);
            X86 c = start;
            f[k](&c);
        }
        t[k] = (now_ns() - t0) / REP;
    }
    bench_fast = 0;
    g3_opt = 0xffffffffu;
    printf("bench qsort %s, %u elements: translation %.0f ns, native %.0f ns per sort\n", what, n, t[0], t[1]);
}

int main(void) {
    harness_mem();
    dt_load_pe("Engine.dll");
    dt_stack_floor = STACK_LO;
    dt_region(STACK_LO, 0x1100u);
    dt_region(ARR, ARR_SIZE);
    dt_region(OBJ, NOBJ * 0x80u);
    dt_region(LOG, LOG_SIZE);

    test_inline(30000);
    test_guest(15000);
    test_edges();
    test_off(500);
    printf("cases by comparator: asc+0x18 %u, desc+0x18 %u, int+0x64 %u, guest %u\n", by_kind[0], by_kind[1],
           by_kind[2], by_kind[3]);
    if (getenv("G3N_BENCH")) {
        bench_one("asc+0x18 (inline)", 0x103fd240u, 256u);
        bench_one("asc+0x18 (inline)", 0x103fd240u, 40u);
        bench_one("guest comparator", FAKE, 256u);
    }
    return dt_report("shadow");
}
