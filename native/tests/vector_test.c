/* native/tests/vector_test.c - native SharedBase vector/matrix/cull leaves against the translation.
 *
 * Build and run (from the checkout root):
 *   NATIVE="vector.c rand.c" sh native/tests/build.sh vector aux-sharedbase/chunk_10010000.c \
 *     aux-sharedbase/chunk_10014000.c aux-sharedbase/chunk_10018000.c aux-sharedbase/chunk_10034000.c \
 *     aux-sharedbase/chunk_10038000.c aux-sharedbase/chunk_10040000.c aux-sharedbase/chunk_1000c000.c \
 *     aux-sharedbase/chunk_10084000.c aux-sharedbase/chunk_10074000.c aux-sharedbase/chunk_10070000.c \
 *     aux-sharedbase/chunk_1006c000.c
 * (the last three: the CRT sqrt's error path, __ctrlfp and _errno, which a NaN axis reaches)
 * G3N_BENCH=1 in the environment adds a timing of each pair (the -O1 test build, harness
 * recomp_return; the ratio is what counts).
 *
 * Every case runs the original and the native from the same state (random registers, x87 top,
 * live caller values, stale tags, rounding/precision control words, XMM) and compares registers,
 * the x87 state and memory bitwise (difftest_rp.h). Inputs mix ordinary values with 0, -0, NaN,
 * the indefinite, infinities, denormals and FLT_MAX; outputs that overlap inputs are covered.
 * The cylinder test's axis path calls the guest Normalize, the CRT's sqrt included.
 * Not covered: SetToProduct with this == a or b before its static temp exists (the native calls
 * the original then: its first use registers an atexit). */
#include "difftest_rp.h"

#include <time.h>

void fn_10011c00(X86 *c);
void fn_10010e20(X86 *c);
void fn_100103d0(X86 *c);
void fn_100103f0(X86 *c);
void fn_1001ae70(X86 *c);
void fn_100138d0(X86 *c);
void fn_10016b10(X86 *c);
void fn_1001b9e0(X86 *c);
void fn_1003a9b0(X86 *c);
void fn_10042f30(X86 *c);
void fn_10035fd0(X86 *c);
void g3n_10011c00(X86 *c);
void g3n_10010e20(X86 *c);
void g3n_100103d0(X86 *c);
void g3n_100103f0(X86 *c);
void g3n_1001ae70(X86 *c);
void g3n_100138d0(X86 *c);
void g3n_10016b10(X86 *c);
void g3n_1001b9e0(X86 *c);
void g3n_1003a9b0(X86 *c);
void g3n_10042f30(X86 *c);
void g3n_10035fd0(X86 *c);

#define DATA 0x03000000u
#define ESP0 0x0e800000u
#define RET_SITE 0x10032415u
#define PRODUCT_GUARD 0x1011dcc8u

/* The CRT's sqrt sets errno on a domain error, through _errno -> _getptd_noexit, whose imports
 * are modelled as in rand_test.c: the kit's TLS array at TEB + 0x1000, FlsGetValue = TlsGetValue. */
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

static uint32_t fbits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static void call_frame(X86 *c, unsigned nargs, const uint32_t *args) {
    for (unsigned i = nargs; i-- > 0;) {
        c->r[4] -= 4;
        wr32(c->r[4], args[i]);
    }
    c->r[4] -= 4;
    wr32(c->r[4], RET_SITE);
}

static void fill(uint32_t a, unsigned n) {
    for (unsigned i = 0; i < n; ++i)
        wrf32(a + 4u * i, dt_float());
}

/* A vector somewhere in the first 0x100 bytes of DATA, dword aligned. */
static uint32_t vec_at(void) {
    return DATA + 4u * (dt_rand32() % 48u);
}

static void ctx(X86 *c) {
    dt_context(c, ESP0, dt_rand32() % 4);
}

typedef void (*fn_t)(X86 *);

static void test_vec1(const char *name, fn_t orig, fn_t native, unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        fill(DATA, 0x80);
        const uint32_t self = vec_at();
        const uint32_t v = dt_rand32() % 6 == 0 ? self + 4u * (dt_rand32() % 3) : vec_at();
        ctx(&c);
        c.r[1] = self;
        call_frame(&c, 1, &v);
        dt_run(name, &c, orig, native);
    }
}

static void test_mul_scalar(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        fill(DATA, 0x80);
        const uint32_t self = vec_at();
        const uint32_t out = dt_rand32() % 6 == 0 ? self + 4u * (dt_rand32() % 3) : vec_at();
        const uint32_t args[2] = {out, fbits(dt_float())};
        ctx(&c);
        c.r[1] = self;
        call_frame(&c, 2, args);
        dt_run("operator*", &c, fn_10011c00, g3n_10011c00);
    }
}

static void test_sqmag(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        fill(DATA, 0x80);
        ctx(&c);
        c.r[1] = vec_at();
        call_frame(&c, 0, NULL);
        dt_run("GetSquareMagnitude", &c, fn_10010e20, g3n_10010e20);
    }
}

static void test_product(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        fill(DATA, 0xc0);
        uint32_t m[3];
        for (int k = 0; k < 3; ++k)
            m[k] = DATA + 0x40u * k;
        uint32_t self = m[0], a = m[1], b = m[2];
        switch (dt_rand32() % 8) {
        case 0: a = self; break;
        case 1: b = self; break;
        case 2: a = b; break;
        case 3: a = b = self; break;
        case 4: self = DATA + 0x20u; break; /* overlapping a, not equal */
        default: break;
        }
        wr32(PRODUCT_GUARD, rd32(PRODUCT_GUARD) | 1u);
        fill(0x1011dcd0u, 16);
        const uint32_t args[2] = {a, b};
        ctx(&c);
        c.r[1] = self;
        call_frame(&c, 2, args);
        dt_run("SetToProduct", &c, fn_10016b10, g3n_10016b10);
    }
}

static void test_merge(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        fill(DATA, 0x80);
        const uint32_t self = vec_at();
        uint32_t p = vec_at();
        if (dt_rand32() % 6 == 0)
            p = self + 4u * (dt_rand32() % 6); /* a corner of the box itself */
        ctx(&c);
        c.r[1] = self;
        call_frame(&c, 1, &p);
        dt_run("Merge", &c, fn_1001b9e0, g3n_1001b9e0);
    }
}

static void test_sphere(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        fill(DATA, 0x80);
        const uint32_t self = vec_at();
        const uint32_t p = vec_at();
        if (dt_rand32() % 2) /* a radius around the distance, so both answers come up */
            wrf32(self, (float)(dt_rand32() % 2000) * 0.5f);
        ctx(&c);
        c.r[1] = self;
        call_frame(&c, 1, &p);
        dt_run("Sphere::Cull", &c, fn_10042f30, g3n_10042f30);
    }
}

static void test_frustum(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        const uint32_t self = DATA;
        fill(DATA, 0x80);
        if (dt_rand32() % 2) /* planes the point is mostly inside of */
            for (int k = 0; k < 6; ++k)
                if (dt_rand32() % 8)
                    wrf32(self + 0x10u * k + 0xcu, 1e6f);
        const uint32_t p = DATA + 0x60u + 4u * (dt_rand32() % 16);
        ctx(&c);
        c.r[1] = self + 4u * (dt_rand32() % 2);
        call_frame(&c, 1, &p);
        dt_run("Frustum::Cull", &c, fn_10035fd0, g3n_10035fd0);
    }
}

/* [bl 0/1/other][end circle, axis, axis along z][result] */
static unsigned cyl_paths[3][3][2];

static float small(void) {
    return (float)((int32_t)(dt_rand32() % 2001u) - 1000) * 0.125f;
}

static void test_cylinder(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        const uint32_t self = DATA, p = DATA + 0x40u;
        if (dt_rand32() % 4 == 0) {
            fill(DATA, 0x20);
        } else {
            /* a cylinder and a point near it: the end circles, the axis between and beyond the
             * ends, the radius test both ways */
            float v[8];
            for (int k = 0; k < 8; ++k)
                v[k] = small();
            v[0] = fabsf(v[0]) * 0.25f;
            v[4] = fabsf(v[4]) * 0.25f;
            if (dt_rand32() % 6 == 0)
                v[5] = v[1]; /* an axis along z: the other division */
            if (dt_rand32() % 10 == 0)
                v[7] = v[3];
            if (dt_rand32() % 12 == 0)
                v[dt_rand32() % 8] = dt_float();
            for (int k = 0; k < 8; ++k)
                wrf32(self + 4u * k, v[k]);
            const float t = (float)((int32_t)(dt_rand32() % 1400u) - 200) / 1000.0f;
            wrf32(p, v[1] + (v[5] - v[1]) * t + small() * 0.1f);
            wrf32(p + 4u, small());
            wrf32(p + 8u, v[3] + (v[7] - v[3]) * t + small() * 0.1f);
            if (dt_rand32() % 16 == 0)
                wrf32(p + 4u * (dt_rand32() % 3), dt_float());
        }
        static const uint8_t bls[] = {0, 1, 0, 1, 0, 1, 2, 0xff};
        const uint32_t args[2] = {p, (dt_rand32() & 0xffffff00u) | bls[dt_rand32() % 8]};
        ctx(&c);
        c.r[1] = self;
        call_frame(&c, 2, args);
        dt_run("InfDoubleCylinder::Cull", &c, fn_1003a9b0, g3n_1003a9b0);
        /* Which path: an end circle leaves AH as p's (0), the axis path leaves LAHF's flags
         * (bit 1 set; ZF alone when the axis has no x extent). */
        const uint32_t ah = (c.r[0] >> 8) & 0xffu, al = c.r[0] & 0xffu;
        const unsigned bl = args[1] & 0xffu, b = bl > 1 ? 2 : bl;
        if (!(ah & 2u))
            ++cyl_paths[b][0][al & 1u];
        else
            ++cyl_paths[b][(ah & 0x44u) == 0x40u ? 2 : 1][al & 1u];
    }
}

static void cyl_report(void) {
    static const char *const kind[3] = {"end circle", "axis", "axis along z"};
    printf("cylinder paths (false/true):");
    for (int b = 0; b < 3; ++b)
        for (int k = 0; k < 3; ++k)
            printf("%s bl%s %s %u/%u", k || b ? "," : "", b == 2 ? "*" : b ? "1" : "0", kind[k], cyl_paths[b][k][0],
                   cyl_paths[b][k][1]);
    printf("\n");
    for (int b = 0; b < 2; ++b)
        for (int k = 0; k < 3; ++k)
            for (int r = 0; r < 2; ++r)
                if (!cyl_paths[b][k][r] && !(k == 0 && r == 0)) {
                    fprintf(stderr, "FAIL: cylinder path bl%d %s %d never taken\n", b, kind[k], r);
                    ++dt_fail;
                }
}

/* Times `n` calls of each from a fixed state (one case of the given setup). */
static void bench(const char *name, fn_t orig, fn_t native, void (*setup)(X86 *), unsigned n) {
    X86 c0, c;
    setup(&c0);
    double t[2];
    for (int w = 0; w < 2; ++w) {
        g3_opt = w ? 0xffffffffu : 0u;
        const fn_t f = w ? native : orig;
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (unsigned i = 0; i < n; ++i) {
            c = c0;
            f(&c);
        }
        clock_gettime(CLOCK_MONOTONIC, &b);
        t[w] = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / n;
    }
    printf("  %-24s translated %6.1f ns  native %6.1f ns  x%.1f\n", name, t[0], t[1], t[0] / t[1]);
}

static void b_vec(X86 *c) {
    for (int k = 0; k < 64; ++k)
        wrf32(DATA + 4u * k, (float)(k % 7) * 0.75f + 1.0f);
    dt_context(c, ESP0, 0);
    c->fpu_cw = 0x007f;
    c->r[1] = DATA;
    const uint32_t args[2] = {DATA + 0x80u, fbits(1.5f)};
    call_frame(c, 2, args);
}
static void b_product(X86 *c) {
    b_vec(c);
    c->r[4] += 12u;
    const uint32_t args[2] = {DATA + 0x40u, DATA + 0x80u};
    call_frame(c, 2, args);
}
static void b_cyl(X86 *c, uint32_t bl, float px) {
    const float v[8] = {1.0f, 0.0f, 0.0f, 0.0f, 2.0f, 10.0f, 0.0f, 5.0f};
    for (int k = 0; k < 8; ++k)
        wrf32(DATA + 4u * k, v[k]);
    wrf32(DATA + 0x40u, px);
    wrf32(DATA + 0x44u, 0.0f);
    wrf32(DATA + 0x48u, 2.0f);
    dt_context(c, ESP0, 0);
    c->fpu_cw = 0x007f;
    c->r[1] = DATA;
    const uint32_t args[2] = {DATA + 0x40u, bl};
    call_frame(c, 2, args);
}
static void b_cyl_end(X86 *c) {
    b_cyl(c, 0, 0.0f);
    wrf32(DATA + 0x48u, 0.5f);
}
static void b_cyl_axis(X86 *c) {
    b_cyl(c, 1, 5.0f);
}
static void b_frustum(X86 *c) {
    for (int k = 0; k < 24; ++k)
        wrf32(DATA + 4u * k, (k & 3) == 3 ? 100.0f : 0.5f);
    dt_context(c, ESP0, 0);
    c->fpu_cw = 0x007f;
    c->r[1] = DATA;
    const uint32_t p = DATA + 0x80u;
    call_frame(c, 1, &p);
}

int main(void) {
    harness_mem();
    dt_load_pe("SharedBase.dll");
    dt_stack_floor = ESP0 - 0x400u;
    dt_region(dt_stack_floor, 0x400u);
    dt_region(DATA, 0x200u);
    dt_region(0x1011dc00u, 0x200u);
    dt_region(0x100d0000u, 0x5000u);
    dt_region(PTD, 0x400u);
    setup_crt();

    test_mul_scalar(50000);
    test_sqmag(50000);
    test_vec1("Scale", fn_100103d0, g3n_100103d0, 30000);
    test_vec1("Translate", fn_100103f0, g3n_100103f0, 30000);
    test_vec1("InvTranslate", fn_1001ae70, g3n_1001ae70, 30000);
    test_vec1("ModifyXAxis", fn_100138d0, g3n_100138d0, 30000);
    test_product(20000);
    test_merge(50000);
    test_sphere(50000);
    test_frustum(50000);
    test_cylinder(100000);
    cyl_report();

    if (getenv("G3N_BENCH")) {
        printf("bench (ns per call):\n");
        const unsigned n = 2000000;
        bench("operator*", fn_10011c00, g3n_10011c00, b_vec, n);
        bench("GetSquareMagnitude", fn_10010e20, g3n_10010e20, b_vec, n);
        bench("Scale", fn_100103d0, g3n_100103d0, b_vec, n);
        bench("Translate", fn_100103f0, g3n_100103f0, b_vec, n);
        bench("InvTranslate", fn_1001ae70, g3n_1001ae70, b_vec, n);
        bench("ModifyXAxis", fn_100138d0, g3n_100138d0, b_vec, n);
        bench("SetToProduct", fn_10016b10, g3n_10016b10, b_product, n);
        bench("Merge", fn_1001b9e0, g3n_1001b9e0, b_vec, n);
        bench("Sphere::Cull", fn_10042f30, g3n_10042f30, b_vec, n);
        bench("Frustum::Cull (6 planes)", fn_10035fd0, g3n_10035fd0, b_frustum, n);
        bench("Cylinder::Cull end", fn_1003a9b0, g3n_1003a9b0, b_cyl_end, n);
        bench("Cylinder::Cull axis", fn_1003a9b0, g3n_1003a9b0, b_cyl_axis, n);
    }
    return dt_report("vector_test");
}
