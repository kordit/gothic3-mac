/* native/tests/exec_test.c - native/exec.c against the translation.
 *
 * Build and run (from the checkout root):
 *   NATIVE="exec.c shadow.c vector.c pdo.c scene.c" sh native/tests/build.sh exec aux-engine/chunk_10564000.c \
 *       aux-engine/chunk_103ac000.c aux-sharedbase/chunk_10010000.c aux-sharedbase/chunk_10018000.c \
 *       aux-engine/chunk_10494000.c
 * G3N_BENCH=1 adds a timing of a particle sort.
 *
 * The particle depth sort as DepthSortParticles 103afa70 makes it: Engine's qsort over particles
 * of 0xb4 bytes with the comparator 103af9c0. The original is the translated qsort calling the
 * translated comparator, which calls the translated SharedBase SetVector, InvTranslate and
 * GetSquareMagnitude through Engine's import slots (answered below by address); the native is
 * shadow.c's qsort with the comparator in place (native/pdo.c's, which exec.c's became). Particles at random positions around a
 * random eye, with equal distances (copies, mirrored positions), NaN, infinities and denormals
 * now and then, 0 to 400 of them; now and then an import slot holds another address (a hooked
 * import), and the native must then make the guest call. Every case starts from a random context
 * (difftest_rp.h) and compares registers, x87 state, XMM and memory bitwise, the sorted array
 * included. */
#include "difftest_rp.h"

#include <math.h>
#include <time.h>

void fn_10564ec0(X86 *c);
void g3n_10564ec0(X86 *c);
void fn_103af9c0(X86 *c);
void fn_100138d0(X86 *c);
void fn_1001ae70(X86 *c);
void fn_10010e20(X86 *c);

#define ESP0 0x0e800000u
#define STACK_LO (ESP0 - 0x1000u)
#define ARR 0x03000000u /* the particles */
#define PSIZE 0xb4u
#define MAXN 400u
#define EYE 0x10868b6cu
#define HOOK 0x0fd00000u
#define RET_SITE 0x103afad3u /* DepthSortParticles' continuation after CALL qsort */
#define CMP 0x103af9c0u

static const struct {
    uint32_t slot, target;
    void (*f)(X86 *);
} imports[] = {
    {0x105f7158u, 0x100138d0u, fn_100138d0},
    {0x105f66ecu, 0x1001ae70u, fn_1001ae70},
    {0x105f677cu, 0x10010e20u, fn_10010e20},
};
#define NIMPORTS (sizeof imports / sizeof imports[0])
static int hooked = -1;
static unsigned long guest_cmp_calls;

/* ---- FUN_10494fc0's world: a shader state, its components, parameter entries, the device --- */

void fn_10494fc0(X86 *c);
void g3n_10494fc0(X86 *c);

#define SH 0x03100000u     /* the shader state, 0x1200 bytes */
#define DEVST 0x03102000u  /* +0x7bc the state bound per stage */
#define REND 0x03103000u   /* +0xc the device wrapper */
#define WRAP 0x03103100u   /* [WRAP] = DVT */
#define DVT 0x03103200u    /* +0x18c.. +0x1bc the flush methods */
#define LIST 0x03103400u   /* {entries, count} (ECX) */
#define COMPS 0x03104000u  /* component entries, 0x38 bytes */
#define PARAMS 0x03106000u /* parameter entries, 0x38 bytes */
#define OBJS 0x03108000u   /* the objects the entries point to, 0x10 bytes; [obj] = OVT */
#define OVT 0x03109000u    /* +4 the parameter method, +8 the component method */
#define CLOG 0x0310a000u   /* the fakes' log: count, then 16 words per call */
#define CLOG_SIZE 0x8000u
#define F_COMP 0x0fd00100u
#define F_PAR 0x0fd00110u
#define F_DEV 0x0fd00200u /* + 0x10 * k for the slots below */
static const uint32_t dev_slots[6] = {0x18cu, 0x190u, 0x194u, 0x1b4u, 0x1b8u, 0x1bcu};
static uint32_t case_seed;

static uint32_t mix(uint32_t h, uint32_t v) {
    h ^= v + 0x7f4a7c15u + (h << 6) + (h >> 2);
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    return h ^ (h >> 16);
}

/* A thiscall method popping `nargs`: logs target, return address, ESP, all registers but ESP and
 * its arguments; clobbers EAX, ECX, EDX and XMM0; now and then writes a constant range (the
 * caller must read the ranges back after its calls). */
static int bench_fast; /* the fakes only return */
static void fake_method(X86 *c, uint32_t target, unsigned nargs) {
    if (bench_fast) {
        c->eip = rd32(c->r[4]);
        c->r[4] += 4u + 4u * nargs;
        return;
    }
    const uint32_t n = rd32(CLOG);
    const uint32_t e = CLOG + 4u + 64u * n;
    if (e + 64u > CLOG + CLOG_SIZE) {
        fprintf(stderr, "call log full\n");
        abort();
    }
    wr32(CLOG, n + 1u);
    const uint32_t esp = c->r[4];
    const uint32_t w[16] = {target, rd32(esp), esp, c->r[0], c->r[1], c->r[2], c->r[3], c->r[5], c->r[6], c->r[7],
                            rd32(esp + 4u), rd32(esp + 8u), nargs > 2 ? rd32(esp + 12u) : 0u, 0, 0, 0};
    uint32_t h = mix(case_seed, n);
    for (unsigned i = 0; i < 16; ++i) {
        wr32(e + 4u * i, w[i]);
        h = mix(h, w[i]);
    }
    if ((h & 7u) == 1u) {
        const uint32_t k = (h >> 3) % 3u;
        const uint32_t lo = (h >> 8) % 20u;
        wr32(SH + 0x20u + 8u * k, lo);
        wr32(SH + 0x24u + 8u * k, lo + (h >> 16) % 8u);
    }
    c->r[0] = mix(h, 1);
    c->r[1] = mix(h, 2);
    c->r[2] = mix(h, 3);
    for (int i = 0; i < 4; ++i)
        c->xmm[0][i] = mix(h, 4u + (uint32_t)i);
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u + 4u * nargs;
}

static int fake_dispatch(X86 *c, uint32_t target) {
    if (target == F_COMP) {
        fake_method(c, target, 2);
        return 1;
    }
    if (target == F_PAR) {
        fake_method(c, target, 3);
        return 1;
    }
    for (unsigned k = 0; k < 6; ++k)
        if (target == F_DEV + 0x10u * k) {
            fake_method(c, target, 3);
            return 1;
        }
    return 0;
}

void recomp_call_ic(X86 *c, uint32_t target, uint64_t *ic) {
    (void)ic;
    if (fake_dispatch(c, target))
        return;
    if (target == CMP) {
        ++guest_cmp_calls;
        fn_103af9c0(c);
        return;
    }
    if (target == HOOK && hooked >= 0) {
        imports[hooked].f(c);
        return;
    }
    for (unsigned i = 0; i < NIMPORTS; ++i)
        if (imports[i].target == target) {
            imports[i].f(c);
            return;
        }
    fprintf(stderr, "unexpected indirect call to %08x\n", target);
    abort();
}
void recomp_call(X86 *c, uint32_t target) {
    recomp_call_ic(c, target, NULL);
}

static float coord(void) {
    if (dt_rand32() % 64u == 0)
        return dt_float();
    return ((float)(int32_t)dt_rand32() / 2147483648.0f) * 3000.0f;
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

static void setup(X86 *c, unsigned n) {
    const float ex = coord(), ey = coord(), ez = coord();
    wrf32(EYE, ex);
    wrf32(EYE + 4u, ey);
    wrf32(EYE + 8u, ez);
    for (uint32_t a = ARR; a < ARR + PSIZE * n; a += 4)
        wr32(a, dt_rand32());
    const unsigned style = dt_rand32() % 4u;
    for (unsigned i = 0; i < n; ++i) {
        const uint32_t p = ARR + PSIZE * i;
        float x = coord(), y = coord(), z = coord();
        if (style == 1 && i && dt_rand32() % 2u) { /* a copy of an earlier one: an equal distance */
            const uint32_t q = ARR + PSIZE * (dt_rand32() % i);
            x = rdf32(q + 0xcu);
            y = rdf32(q + 0x10u);
            z = rdf32(q + 0x14u);
        } else if (style == 2) { /* few distinct distances: the eye plus small integers */
            x = ex + (float)(dt_rand32() % 3u);
            y = ey;
            z = ez - (float)(dt_rand32() % 3u);
        }
        wrf32(p + 0xcu, x);
        wrf32(p + 0x10u, y);
        wrf32(p + 0x14u, z);
    }
    for (unsigned i = 0; i < NIMPORTS; ++i)
        wr32(imports[i].slot, imports[i].target);
    hooked = -1;
    if (dt_rand32() % 32u == 0) {
        hooked = (int)(dt_rand32() % NIMPORTS);
        wr32(imports[hooked].slot, HOOK);
    }
    dt_context(c, ESP0, dt_rand32() % 6u);
    for (uint32_t a = STACK_LO; a < ESP0; a += 4)
        wr32(a, dt_rand32());
    wr32(ESP0, RET_SITE);
    wr32(ESP0 + 4u, ARR);
    wr32(ESP0 + 8u, n);
    wr32(ESP0 + 12u, PSIZE);
    wr32(ESP0 + 16u, CMP);
}

static void test_sort(unsigned cases) {
    X86 c;
    for (unsigned i = 0; i < cases; ++i) {
        setup(&c, pick_n());
        dt_run("particle sort", &c, fn_10564ec0, g3n_10564ec0);
    }
}

/* With the group off the native qsort calls the guest comparator. */
static void test_off(unsigned cases) {
    X86 c;
    for (unsigned i = 0; i < cases; ++i) {
        setup(&c, pick_n());
        X86 start = c, o;
        dt_save(0);
        g3_opt = 0;
        fn_10564ec0(&c);
        o = c;
        dt_save(1);
        dt_restore();
        c = start;
        g3_opt = ~(uint32_t)(G3OPT_EXEC | G3OPT_PDO);
        const unsigned long before = guest_cmp_calls;
        g3n_10564ec0(&c);
        dt_compare("particle sort (exec off)", &o, &c);
        if (rd32(ESP0 + 8u) > 1u && guest_cmp_calls == before) {
            fprintf(stderr, "FAIL: exec off made no guest comparator call\n");
            ++dt_fail;
        }
    }
    g3_opt = 0xffffffffu;
}

static unsigned sh_paths[4], sh_calls[8];

static void setup_shader(X86 *c) {
    case_seed = dt_rand32();
    wr32(CLOG, 0u);
    for (uint32_t a = SH; a < SH + 0x1200u; a += 4)
        wr32(a, dt_rand32());
    const uint32_t stage = dt_rand32() % 4u ? 0u : dt_rand32() % 8u;
    wr32(SH + 4u, DEVST);
    wr32(SH + 8u, REND);
    wr32(SH + 0xcu, stage);
    for (uint32_t k = 0; k < 8u; ++k)
        wr32(DEVST + 0x7bcu + 4u * k, dt_rand32() % 2u ? SH : dt_rand32());
    wr32(REND + 0xcu, WRAP);
    wr32(WRAP, DVT);
    for (unsigned k = 0; k < 6; ++k)
        wr32(DVT + dev_slots[k], F_DEV + 0x10u * k);
    wr32(OVT + 4u, F_PAR);
    wr32(OVT + 8u, F_COMP);
    for (uint32_t i = 0; i < 16u; ++i)
        wr32(OBJS + 0x10u * i, OVT);
    /* counts: 0, a few, or a negative one (the loops are signed) */
    const uint32_t nc = dt_rand32() % 8u == 0 ? 0xfffffffeu : dt_rand32() % 6u;
    const uint32_t np = dt_rand32() % 8u == 0 ? 0x80000001u : dt_rand32() % 6u;
    wr32(LIST, COMPS);
    wr32(LIST + 4u, nc);
    wr32(SH + 0x14u, PARAMS);
    wr32(SH + 0x18u, np);
    for (uint32_t i = 0; i < 8u; ++i) {
        for (uint32_t k = 0; k < 0x38u; k += 4)
            wr32(COMPS + 0x38u * i + k, dt_rand32());
        for (uint32_t k = 0; k < 0x38u; k += 4)
            wr32(PARAMS + 0x38u * i + k, dt_rand32());
        wr32(COMPS + 0x38u * i, OBJS + 0x10u * (dt_rand32() % 16u));
        wr32(PARAMS + 0x38u * i, OBJS + 0x10u * (dt_rand32() % 16u));
    }
    dt_context(c, ESP0, dt_rand32() % 8u);
    c->r[1] = LIST;
    c->r[6] = SH;
    c->r[4] -= 4u;
    wr32(c->r[4], 0x104a6230u); /* a continuation in FUN_104a61a0 */
}

static void test_shader(unsigned cases) {
    X86 c;
    for (unsigned i = 0; i < cases; ++i) {
        setup_shader(&c);
        const int fresh = rd32(DEVST + 0x7bcu + 4u * rd32(SH + 0xcu)) != SH;
        if (dt_run("FUN_10494fc0", &c, fn_10494fc0, g3n_10494fc0)) {
            sh_paths[(fresh ? 1 : 0) + (rd32(SH + 0xcu) ? 2 : 0)]++;
            const uint32_t n = rd32(CLOG);
            sh_calls[n < 7u ? n : 7u]++;
        }
    }
}

static double now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

/* G3N_BENCH=1: one particle array sorted again and again from the same order. */
static void bench(unsigned n) {
    enum { REP = 300 };
    static uint8_t saved[PSIZE * MAXN];
    X86 start;
    do
        setup(&start, n);
    while (hooked >= 0);
    memcpy(saved, g_mem + ARR, PSIZE * n);
    double t[3];
    /* the translation; shadow.c's qsort with exec.c's comparator; shadow.c's qsort with the guest
     * comparator (the game before this group) */
    void (*const f[3])(X86 *) = {fn_10564ec0, g3n_10564ec0, g3n_10564ec0};
    const uint32_t opt[3] = {0u, 0xffffffffu, ~(uint32_t)(G3OPT_EXEC | G3OPT_PDO)};
    for (int k = 0; k < 3; ++k) {
        g3_opt = opt[k];
        const double t0 = now_ns();
        for (unsigned rep = 0; rep < REP; ++rep) {
            memcpy(g_mem + ARR, saved, PSIZE * n);
            X86 c = start;
            f[k](&c);
        }
        t[k] = (now_ns() - t0) / REP;
    }
    g3_opt = 0xffffffffu;
    printf("bench particle sort, %u particles: translation %.1f us, native qsort with the guest comparator %.1f us, "
           "all native %.1f us per sort\n",
           n, t[0] / 1000.0, t[2] / 1000.0, t[1] / 1000.0);
}

/* G3N_BENCH=1: FUN_10494fc0 over the same cases (state, entries and log restored each time); the
 * fakes cost the same on both sides. */
static void bench_shader(void) {
    enum { NC = 64, REP = 2000 };
    static X86 start[NC];
    static uint8_t mem[NC][0x1200 + 0x800];
    static uint32_t seeds[NC];
    for (unsigned i = 0; i < NC; ++i) {
        setup_shader(&start[i]);
        seeds[i] = case_seed;
        memcpy(mem[i], g_mem + SH, 0x1200);
        memcpy(mem[i] + 0x1200, g_mem + COMPS, 0x400);
        memcpy(mem[i] + 0x1600, g_mem + PARAMS, 0x400);
    }
    double t[3];
    static volatile uint32_t sink;
    bench_fast = 1;
    for (int k = 0; k < 3; ++k) {
        g3_opt = k == 1 ? 0xffffffffu : 0u;
        const double t0 = now_ns();
        for (unsigned rep = 0; rep < REP; ++rep)
            for (unsigned i = 0; i < NC; ++i) {
                memcpy(g_mem + SH, mem[i], 0x1200);
                memcpy(g_mem + COMPS, mem[i] + 0x1200, 0x400);
                memcpy(g_mem + PARAMS, mem[i] + 0x1600, 0x400);
                wr32(CLOG, 0u);
                case_seed = seeds[i];
                X86 c = start[i];
                if (k < 2)
                    (k ? g3n_10494fc0 : fn_10494fc0)(&c);
                sink += c.r[0];
            }
        t[k] = (now_ns() - t0) / (double)(REP * NC);
    }
    g3_opt = 0xffffffffu;
    bench_fast = 0;
    printf("bench FUN_10494fc0, ns per call less the restore (%.1f ns), with fakes that only return: translation %.1f, native "
           "%.1f\n",
           t[2], t[0] - t[2], t[1] - t[2]);
}

int main(void) {
    harness_mem();
    dt_load_pe("SharedBase.dll");
    dt_load_pe("Engine.dll");
    dt_stack_floor = STACK_LO;
    dt_region(STACK_LO, 0x1100u);
    dt_region(ARR, PSIZE * MAXN);
    dt_region(EYE, 0x10u);

    dt_region(SH, 0x1200u);
    dt_region(DEVST, 0x800u);
    dt_region(COMPS, 0x400u);
    dt_region(PARAMS, 0x400u);
    dt_region(CLOG, CLOG_SIZE);

    test_sort(20000);
    test_off(300);
    test_shader(200000);
    printf("FUN_10494fc0: bound/new at stage 0 %u/%u, other stage %u/%u; guest calls per case:", sh_paths[0],
           sh_paths[1], sh_paths[2], sh_paths[3]);
    for (unsigned k = 0; k < 8; ++k)
        printf(" %u:%u", k, sh_calls[k]);
    printf("\n");
    if (getenv("G3N_BENCH")) {
        bench(64);
        bench(256);
        bench_shader();
    }
    return dt_report("exec_test");
}
