/* native/tests/ai_geom_test.c - native/ai.c's CanEvade geometry (bCVector2::Normalize 1000f810)
 * against the translation. A test of its own because its chunk (SharedBase chunk_1000c000) holds
 * the GetInstance and Malloc that ai_test.c fakes.
 *
 * Build and run (from the checkout root):
 *   NATIVE="ai.c rand.c" sh native/tests/build.sh ai_geom aux-sharedbase/chunk_1000c000.c \
 *     aux-sharedbase/chunk_10010000.c aux-sharedbase/chunk_10038000.c aux-sharedbase/chunk_10034000.c \
 *     aux-sharedbase/chunk_10050000.c \
 *     aux-sharedbase/chunk_10084000.c aux-sharedbase/chunk_10074000.c \
 *     aux-sharedbase/chunk_10070000.c aux-sharedbase/chunk_1006c000.c
 * (chunk_10084000: the CRT sqrt; 10074000, 10070000, 1006c000 and rand.c's _getptd_noexit: its
 * error path, which a NaN square reaches; the CRT setup is leaf_test.c's.) G3N_BENCH=1 adds a
 * timing.
 *
 * Compares registers, x87 state, XMM, EFLAGS' absence aside, guest memory and the dead stack
 * bytes below the final ESP (the sqrt's frame the native writes as the original leaves it). */
#include "difftest_rp.h"

#include <time.h>

void fn_1000f810(X86 *c);
void g3n_1000f810(X86 *c);
void fn_10010e50(X86 *c);
void g3n_10010e50(X86 *c);
void fn_10010c90(X86 *c);
void g3n_10010c90(X86 *c);
void fn_10010c50(X86 *c);
void g3n_10010c50(X86 *c);
void fn_10039900(X86 *c);
void g3n_10039900(X86 *c);
void fn_10039d80(X86 *c);
void g3n_10039d80(X86 *c);

#define DATA 0x03000000u
#define DATA_LEN 0x1000u
#define ESP0 0x0e800000u
#define RET_SITE 0x10a4adc5u /* somewhere in IntersectsStretch */
#define FASTFLAG 0x100d4118u

#define FLSINDEX 5u
#define GETVALUEINDEX 6u
#define PTD 0x02000000u
#define SHIM_GETLASTERROR 0x0ff00000u
#define SHIM_SETLASTERROR 0x0ff00010u
#define SHIM_TLSGETVALUE 0x0ff00020u
static uint32_t last_error = 0x1234;

static uint32_t arg(X86 *c, int i) {
    return rd32(c->r[4] + 4u + 4u * (uint32_t)i);
}
static void ret_stdcall(X86 *c, int nargs, uint32_t eax) {
    c->r[4] += 4u + 4u * (uint32_t)nargs;
    c->r[0] = eax;
}

void recomp_call_ic(X86 *c, uint32_t target, uint64_t *ic) {
    (void)ic;
    switch (target) {
    case SHIM_GETLASTERROR:
        ret_stdcall(c, 0, last_error);
        return;
    case SHIM_SETLASTERROR:
        last_error = arg(c, 0);
        ret_stdcall(c, 1, 0);
        return;
    case SHIM_TLSGETVALUE: {
        const uint32_t i = arg(c, 0);
        ret_stdcall(c, 1, i < 64 ? rd32(c->fs_base + 0x1000u + 4u * i) : 0);
        return;
    }
    }
    fprintf(stderr, "unexpected indirect call to %08x\n", target);
    abort();
}

static void setup_crt(void) {
    wr32(0x100891a0u, SHIM_GETLASTERROR);
    wr32(0x100891a4u, SHIM_SETLASTERROR);
    wr32(0x100891d8u, SHIM_TLSGETVALUE);
    wr32(0x100d1158u, FLSINDEX);
    wr32(0x100d115cu, GETVALUEINDEX);
    wr32(GUEST_TEB_BASE + 0x1000u + 4u * FLSINDEX, PTD);
    wr32(GUEST_TEB_BASE + 0x1000u + 4u * GETVALUEINDEX, SHIM_TLSGETVALUE);
}

static unsigned dead_fail;

static void run(const char *what, X86 *c, void (*orig)(X86 *), void (*native)(X86 *)) {
    X86 start = *c, o;
    dt_save(0);
    g3_opt = 0;
    orig(c);
    o = *c;
    dt_save(1);
    dt_restore();
    *c = start;
    g3_opt = G3OPT_AI;
    native(c);
    const unsigned before = dt_fail;
    dt_compare(what, &o, c);
    int bad = 0;
    for (uint32_t k = 0; k < dt_regions[0].len; ++k) { /* region 0 is the stack */
        const uint32_t a = dt_regions[0].base + k;
        if (a >= o.r[4])
            break;
        if (dt_regions[0].orig[k] != g_mem[a]) {
            if (dead_fail < 10)
                fprintf(stderr, "FAIL %s: dead stack byte %08x %02x/%02x (final ESP %08x)\n", what, a,
                        dt_regions[0].orig[k], g_mem[a], o.r[4]);
            ++dead_fail;
            bad = 1;
            break;
        }
    }
    if (bad && dt_fail == before)
        ++dt_fail;
}

static float smallf(void) {
    return (float)((int32_t)(dt_rand32() % 4001u) - 2000) * 0.0625f;
}

static unsigned paths[4]; /* zero length, scaled, the whole original */

static void test_normalize2(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        for (unsigned k = 0; k < 0x80; ++k)
            wrf32(DATA + 4u * k, dt_float());
        uint32_t self = DATA + 0x40u + 4u * (dt_rand32() % 128u);
        switch (dt_rand32() % 10) {
        case 0:
            for (int k = 0; k < 2; ++k)
                wrf32(self + 4u * k, dt_rand32() & 1 ? (dt_rand32() & 1 ? 0.0f : -0.0f) : dt_bits(dt_rand32() % 0x400000u));
            break;
        case 1:
            wrf32(self + 4u * (dt_rand32() % 2), dt_bits(0x7f000000u + dt_rand32() % 0x7fffffu));
            break;
        case 2: case 3: case 4: case 5: case 6:
            for (int k = 0; k < 2; ++k)
                wrf32(self + 4u * k, smallf());
            break;
        default: break;
        }
        dt_context(&c, ESP0, dt_rand32() % 4);
        if (dt_rand32() % 16 == 0)
            c.fpu_sw |= 0x0020u;
        if (dt_rand32() % 8 == 0)
            c.fpu_cw &= (uint16_t)~0x0020u; /* precision exceptions unmasked */
        wr32(FASTFLAG, dt_rand32() % 16 == 0);
        c.r[4] -= 4u * (dt_rand32() % 4u);
        c.r[4] -= 4;
        wr32(c.r[4], RET_SITE);
        if (dt_rand32() % 32 == 0) { /* the vector in the frame: the original */
            self = c.r[4] - 0x10u - 4u * (dt_rand32() % 4);
            for (int k = 0; k < 2; ++k)
                wrf32(self + 4u * k, smallf());
        }
        c.r[1] = self;
        run("bCVector2::Normalize", &c, fn_1000f810, g3n_1000f810);
        const float x = rdf32(self), y = rdf32(self + 4u);
        ++paths[(x == 0.0f && y == 0.0f) ? 0 : 1];
    }
    wr32(FASTFLAG, 0);
}

/* bCVector::GetSquareMagnitude2D and GetMagnitude2D: the vector, the coordinate 0..2 (another one
 * is the original's assertion path, which the native leaves to it), the context. */
static void test_mag2d(unsigned n, int square) { /* square 2: GetMagnitude (3D, no argument) */
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        for (unsigned k = 0; k < 0x80; ++k)
            wrf32(DATA + 4u * k, dt_float());
        uint32_t self = DATA + 0x40u + 4u * (dt_rand32() % 128u);
        switch (dt_rand32() % 10) {
        case 0:
            for (int k = 0; k < 3; ++k)
                wrf32(self + 4u * k, dt_rand32() & 1 ? (dt_rand32() & 1 ? 0.0f : -0.0f) : dt_bits(dt_rand32() % 0x400000u));
            break;
        case 1:
            wrf32(self + 4u * (dt_rand32() % 3), dt_bits(0x7f000000u + dt_rand32() % 0x7fffffu));
            break;
        case 2: case 3: case 4: case 5: case 6:
            for (int k = 0; k < 3; ++k)
                wrf32(self + 4u * k, smallf());
            break;
        default: break;
        }
        dt_context(&c, ESP0, dt_rand32() % 4);
        if (dt_rand32() % 16 == 0)
            c.fpu_sw |= 0x0020u;
        if (dt_rand32() % 8 == 0)
            c.fpu_cw &= (uint16_t)~0x0020u; /* precision exceptions unmasked */
        wr32(FASTFLAG, dt_rand32() % 16 == 0);
        c.r[4] -= 4u * (dt_rand32() % 4u);
        if (square != 2) {
            c.r[4] -= 4;
            wr32(c.r[4], dt_rand32() % 3u);
        }
        c.r[4] -= 4;
        wr32(c.r[4], RET_SITE);
        if (square != 1 && dt_rand32() % 32 == 0) /* the vector in the frame */
            self = c.r[4] - 0x10u - 4u * (dt_rand32() % 4);
        c.r[1] = self;
        if (square == 2)
            run("GetMagnitude", &c, fn_10010c50, g3n_10010c50);
        else if (square)
            run("GetSquareMagnitude2D", &c, fn_10010e50, g3n_10010e50);
        else
            run("GetMagnitude2D", &c, fn_10010c90, g3n_10010c90);
    }
    wr32(FASTFLAG, 0);
}

/* bCInfCylinder::Cull(point, bool): cylinder and point in DATA (sometimes the same words), the
 * bool byte 1, 0 or another value, distances around the radius. */
static unsigned cull_paths[2];
static void test_cull(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        for (unsigned k = 0; k < 0x80; ++k)
            wrf32(DATA + 4u * k, dt_float());
        const uint32_t cyl = DATA + 0x40u + 4u * (dt_rand32() % 120u);
        uint32_t pt = DATA + 0x40u + 4u * (dt_rand32() % 120u);
        if (dt_rand32() % 4) {
            const float rad = dt_rand32() % 8 ? smallf() : dt_float();
            wrf32(cyl, rad);
            for (int k = 1; k < 4; ++k)
                wrf32(cyl + 4u * k, smallf());
            for (int k = 0; k < 3; ++k)
                wrf32(pt + 4u * k, rdf32(cyl + 4u + 4u * k) + (dt_rand32() % 3 ? rad * (float)((int)(dt_rand32() % 9) - 4) * 0.25f : smallf()));
        }
        dt_context(&c, ESP0, dt_rand32() % 5);
        if (dt_rand32() % 8 == 0)
            c.fpu_cw &= (uint16_t)~0x0020u;
        c.r[4] -= 4u * (dt_rand32() % 4u);
        const uint32_t flag = dt_rand32() % 3 ? (dt_rand32() & 1) : dt_rand32();
        c.r[4] -= 4;
        wr32(c.r[4], flag);
        c.r[4] -= 4;
        wr32(c.r[4], pt);
        c.r[4] -= 4;
        wr32(c.r[4], RET_SITE);
        if (dt_rand32() % 32 == 0) { /* the point in the frame */
            pt = c.r[4] - 0xcu + 4u * (dt_rand32() % 4);
            wr32(c.r[4] + 4u, pt);
        }
        c.r[1] = cyl;
        run("bCInfCylinder::Cull", &c, fn_10039900, g3n_10039900);
        ++cull_paths[c.r[0] & 1u];
    }
}

/* bCInfCylinder::Intersects(stretch, float &, bCVector2 &, bool, bool): stretches near the
 * cylinder, short ones (the epsilon exit), ones passing beside it (the second exit) and through it
 * (the original's own path, taken whole by the native; the first bool is set so that path needs
 * none of its callees). */
static unsigned isect_paths[3];
static void test_isect(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        for (unsigned k = 0; k < 0x80; ++k)
            wrf32(DATA + 4u * k, dt_float());
        const uint32_t cyl = DATA + 0x40u + 4u * (dt_rand32() % 100u);
        uint32_t stp = DATA + 0x40u + 4u * (dt_rand32() % 100u);
        const uint32_t outf = DATA + 0x200u, outv = DATA + 0x208u;
        if (dt_rand32() % 6) {
            const float rad = dt_rand32() % 8 ? smallf() : dt_float();
            wrf32(cyl, rad);
            for (int k = 1; k < 4; ++k)
                wrf32(cyl + 4u * k, smallf());
            const float off = rad * (float)((int)(dt_rand32() % 13) - 6) * 0.25f;
            for (int e = 0; e < 2; ++e) {
                wrf32(stp + 12u * e, rdf32(cyl + 4u) + (dt_rand32() % 4 ? off : smallf()) + (float)(e ? 3 : -3));
                wrf32(stp + 12u * e + 4u, smallf());
                wrf32(stp + 12u * e + 8u, rdf32(cyl + 12u) + (dt_rand32() % 4 ? off * 0.5f : smallf()));
            }
            if (dt_rand32() % 8 == 0) { /* short */
                wrf32(stp + 12u, rdf32(stp) + (float)(dt_rand32() % 3) * 1e-6f);
                wrf32(stp + 20u, rdf32(stp + 8u));
            }
        }
        dt_context(&c, ESP0, dt_rand32() % 3);
        if (dt_rand32() % 8 == 0)
            c.fpu_cw &= (uint16_t)~0x0020u;
        if (dt_rand32() % 16 == 0)
            c.fpu_sw |= 0x0020u;
        wr32(FASTFLAG, dt_rand32() % 32 == 0);
        c.r[4] -= 4u * (dt_rand32() % 4u);
        const uint32_t args[5] = {stp, outf, outv, 1u + dt_rand32() % 255u, dt_rand32()};
        for (int k = 4; k >= 0; --k) {
            c.r[4] -= 4;
            wr32(c.r[4], args[k]);
        }
        c.r[4] -= 4;
        wr32(c.r[4], RET_SITE);
        c.r[1] = cyl;
        run("bCInfCylinder::Intersects", &c, fn_10039d80, g3n_10039d80);
        ++isect_paths[c.r[0] & 1u];
    }
    wr32(FASTFLAG, 0);
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

static void bench(void) {
    double t[2];
    for (int v = 0; v < 2; ++v) {
        g3_opt = v ? G3OPT_AI : 0u;
        const int n = 2000000;
        const double t0 = now();
        X86 tmpl;
        dt_context(&tmpl, ESP0 - 4u, 0);
        tmpl.fpu_cw = 0x027f;
        for (int i = 0; i < n; ++i) {
            X86 c = tmpl;
            wr32(ESP0 - 4u, RET_SITE);
            wrf32(DATA, 3.0f + (float)(i & 7));
            wrf32(DATA + 4u, 4.0f);
            c.r[1] = DATA;
            g3n_1000f810(&c);
        }
        t[v] = (now() - t0) * 1e9 / n;
    }
    printf("bench bCVector2::Normalize, ns per call: translation %.1f, native %.1f\n", t[0],
           t[1]);
    /* the others: GetMagnitude2D(1), GetMagnitude, Cull(point, 0) */
    static const char *names[] = {"GetMagnitude2D", "GetMagnitude", "bCInfCylinder::Cull"};
    void (*fns[])(X86 *) = {g3n_10010c90, g3n_10010c50, g3n_10039900};
    for (int f = 0; f < 3; ++f) {
        for (int v = 0; v < 2; ++v) {
            g3_opt = v ? G3OPT_AI : 0u;
            const int n = 2000000;
            X86 tmpl;
            dt_context(&tmpl, ESP0 - 12u, 0);
            tmpl.fpu_cw = 0x027f;
            wrf32(DATA + 0x100u, 2.0f);
            wrf32(DATA + 0x104u, 1.0f);
            wrf32(DATA + 0x108u, 2.0f);
            wrf32(DATA + 0x10cu, 3.0f);
            const double t0 = now();
            for (int i = 0; i < n; ++i) {
                X86 c = tmpl;
                wrf32(DATA, 3.0f + (float)(i & 7));
                wrf32(DATA + 4u, 4.0f);
                wrf32(DATA + 8u, 5.0f);
                wr32(ESP0 - 12u, RET_SITE);
                wr32(ESP0 - 8u, f == 2 ? DATA : 1u);
                wr32(ESP0 - 4u, 0u);
                c.r[1] = f == 2 ? DATA + 0x100u : DATA;
                fns[f](&c);
            }
            t[v] = (now() - t0) * 1e9 / n;
        }
        printf("bench %s, ns per call: translation %.1f, native %.1f\n", names[f], t[0], t[1]);
    }
}

int main(void) {
    harness_mem();
    dt_load_pe("SharedBase.dll");
    dt_stack_floor = ESP0 - 0x400u;
    dt_region(dt_stack_floor, 0x400u);
    dt_region(DATA, DATA_LEN);
    dt_region(0x100d0000u, 0x5000u);
    dt_region(PTD, 0x400u);
    setup_crt();
    test_normalize2(300000);
    test_mag2d(200000, 1);
    test_mag2d(300000, 0);
    test_mag2d(300000, 2);
    test_cull(400000);
    test_isect(400000);
    printf("Intersects false/true %u/%u\n", isect_paths[0], isect_paths[1]);
    printf("Cull false/true %u/%u\n", cull_paths[0], cull_paths[1]);
    printf("paths: zero or not scaled %u, scaled %u\n", paths[0], paths[1]);
    if (getenv("G3N_BENCH"))
        bench();
    return dt_report("ai_geom_test");
}
