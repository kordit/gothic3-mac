/* native/tests/leaf_test.c - native/leaf.c and scene.c's CullFrustum against the translation.
 *
 * Build and run (from the checkout root):
 *   NATIVE="leaf.c scene.c rand.c" sh native/tests/build.sh leaf aux-sharedbase/chunk_10010000.c \
 *     aux-sharedbase/chunk_10014000.c aux-sharedbase/chunk_1001c000.c \
 *     aux-sharedbase/chunk_10084000.c aux-sharedbase/chunk_10074000.c \
 *     aux-sharedbase/chunk_10070000.c aux-sharedbase/chunk_1006c000.c \
 *     aux-engine/chunk_10294000.c aux-engine/chunk_10298000.c aux-engine/chunk_1040c000.c
 * (chunk_10084000: the CRT sqrt Normalize calls; 10074000, 10070000, 1006c000 and rand.c's
 * _getptd_noexit: its error path, __ctrlfp and _errno, which a NaN square reaches.)
 * G3N_BENCH=1 in the environment adds a timing of each pair (the -O1 test build, harness
 * recomp_return; the ratio is what counts, and the game builds both sides at -O2, where the SSE
 * leaf 10299740 comes out faster and at -O1 slower).
 *
 * Every case runs the original and the native from the same state (random registers, EFLAGS,
 * x87 top, live caller values, stale tags, rounding/precision control words, XMM) and compares
 * registers, EFLAGS, the x87 state, XMM and memory bitwise (difftest_rp.h), and on top of that
 * the stack bytes below the final ESP, which the natives write as the originals leave them.
 * Inputs mix ordinary values with 0, -0, NaN, the indefinite, infinities, denormals and FLT_MAX;
 * outputs that overlap inputs are covered, and so are frames that overlap the data (the natives
 * call the original then). Normalize also runs with a pending precision exception and with the
 * CRT's fast flag set (both: the original's path).
 *
 * One relaxation: 10299740 (SSE) with a NaN in either matrix compares NaN lanes as NaN, not by
 * payload; see nan_relaxed. Half its cases have no NaN input and are compared exactly. */
#include "difftest_rp.h"

#include <time.h>

void fn_10010060(X86 *c);
void fn_10011bd0(X86 *c);
void fn_10015be0(X86 *c);
void fn_1001da30(X86 *c);
void fn_10295f40(X86 *c);
void fn_10299740(X86 *c);
void fn_102995d0(X86 *c);
void fn_1040c9d0(X86 *c);
void fn_100103b0(X86 *c); /* SharedBase bCVector::Scale(float), 102995d0's callee */
void g3n_10010060(X86 *c);
void g3n_10011bd0(X86 *c);
void g3n_10015be0(X86 *c);
void g3n_1001da30(X86 *c);
void g3n_10295f40(X86 *c);
void g3n_10299740(X86 *c);
void g3n_102995d0(X86 *c);
void g3n_1040c9d0(X86 *c);

#define DATA 0x03000000u
#define DATA_LEN 0x1000u
#define BOX (DATA + 0x800u)
#define ESP0 0x0e800000u
#define RET_SITE 0x10032415u
#define FASTFLAG 0x100d4118u
#define SCALE_IAT 0x105f6684u
#define SCALE_FN 0x100103b0u

/* The CRT's sqrt error path sets errno through _errno -> _getptd_noexit, whose imports are
 * modelled as in vector_test.c: the kit's TLS array at TEB + 0x1000, FlsGetValue = TlsGetValue. */
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
    case SCALE_FN:
        fn_100103b0(c);
        return;
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
    wr32(SCALE_IAT, SCALE_FN);
}

static uint32_t fbits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* Pushes the arguments and the return address; ESP0 moves by a few dwords so the aligned frames
 * (AND ESP,-8 / -16) land differently. */
static void call_frame(X86 *c, unsigned nargs, const uint32_t *args) {
    c->r[4] -= 4u * (dt_rand32() % 4u);
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

/* A context as dt_context makes it, with random EFLAGS fields too. */
static void ctx(X86 *c) {
    dt_context(c, ESP0, dt_rand32() % 4);
    const uint32_t f = dt_rand32();
    c->eflags_cf = f & 1u;
    c->eflags_zf = (f >> 1) & 1u;
    c->eflags_sf = (f >> 2) & 1u;
    c->eflags_of = (f >> 3) & 1u;
    c->eflags_pf = (f >> 4) & 1u;
    c->eflags_af = (f >> 5) & 1u;
}

/* dt_run, plus EFLAGS and the dead stack bytes below the final ESP. The native side runs with
 * only LEAF and SCENE on: the CRT error path a NaN length takes reaches rand.c's _getptd_noexit,
 * whose native leaves its own dead stack bytes unwritten. */
static unsigned dead_fail, flag_fail;

/* SSE results with a NaN input: the translation's MULPS/ADDPS are host float operations whose
 * NaN payload depends on the operand order clang picks, which moves with the optimisation level
 * (the translated 10299740 built at -O0, -O1 and -O2 disagree on 95%, 81% and 98% of random
 * matrices with NaNs in them, and on none without). With nan_relaxed set, a word of DATA or an
 * XMM lane that is a NaN on both sides counts as equal; a NaN on one side only still fails. */
static int nan_relaxed;
static int is_nan32(uint32_t w) {
    return (w & 0x7f800000u) == 0x7f800000u && (w & 0x007fffffu) != 0;
}
static void nan_canon(X86 *o, X86 *n) {
    for (int r = 0; r < 8; ++r)
        for (int i = 0; i < 4; ++i)
            if (is_nan32(o->xmm[r][i]) && is_nan32(n->xmm[r][i]))
                o->xmm[r][i] = n->xmm[r][i] = 0x7fc00000u;
    for (uint32_t k = 0; k + 4u <= dt_regions[1].len; k += 4u) {
        uint32_t ow, nw;
        memcpy(&ow, dt_regions[1].orig + k, 4);
        memcpy(&nw, g_mem + dt_regions[1].base + k, 4);
        if (is_nan32(ow) && is_nan32(nw)) {
            const uint32_t q = 0x7fc00000u;
            memcpy(dt_regions[1].orig + k, &q, 4);
            memcpy(g_mem + dt_regions[1].base + k, &q, 4);
        }
    }
}

static void run(const char *what, X86 *c, void (*orig)(X86 *), void (*native)(X86 *)) {
    X86 start = *c, o;
    dt_save(0);
    g3_opt = 0;
    orig(c);
    o = *c;
    dt_save(1);
    dt_restore();
    *c = start;
    g3_opt = G3OPT_LEAF | G3OPT_SCENE;
    native(c);
    const unsigned before = dt_fail;
    if (nan_relaxed)
        nan_canon(&o, c);
    dt_compare(what, &o, c);
    int bad = 0;
    /* The arithmetic flags are dead at a RET in the game's translation (flags_abi, D-030). */
    if (o.eflags_df != c->eflags_df || o.eflags_misc != c->eflags_misc) {
        if (flag_fail < 10)
            fprintf(stderr, "FAIL %s: eflags %08x/%08x\n", what, x86_get_eflags(&o), x86_get_eflags(c));
        ++flag_fail;
        bad = 1;
    }
    /* region 0 is the stack */
    for (uint32_t k = 0; k < dt_regions[0].len; ++k) {
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

/* A float in a game's range or, one time in four, any of dt_float's awkward values. */
static float smallf(void) {
    return (float)((int32_t)(dt_rand32() % 4001u) - 2000) * 0.0625f;
}
static float valf(void) {
    return dt_rand32() % 4 ? smallf() : dt_float();
}

/* A dword-aligned address from DATA+0x40 to DATA+0x240. */
static uint32_t at(void) {
    return DATA + 0x40u + 4u * (dt_rand32() % 128u);
}

/* ---- bCVector::Normalize ---------------------------------------------------------------- */
static unsigned norm_paths[3]; /* zero length, scaled, unordered (a NaN length) */
static unsigned norm_setups[3]; /* fast flag set, precision exception pending, vector in the frame */

static void test_normalize(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        fill(DATA, 0x80);
        uint32_t self = at();
        switch (dt_rand32() % 10) {
        case 0: /* zero or tiny: a zero length */
            for (int k = 0; k < 3; ++k)
                wrf32(self + 4u * k, dt_rand32() & 1 ? 0.0f : dt_bits(dt_rand32() % 0x400000u));
            break;
        case 1: /* huge: an infinite square */
            wrf32(self + 4u * (dt_rand32() % 3), dt_bits(0x7f000000u + dt_rand32() % 0x7fffffu));
            break;
        case 2: case 3: case 4: case 5: case 6:
            for (int k = 0; k < 3; ++k)
                wrf32(self + 4u * k, smallf());
            break;
        default: break; /* dt_float's mix */
        }
        ctx(&c);
        if (dt_rand32() % 16 == 0) {
            c.fpu_sw |= 0x0020u; /* a precision exception pending */
            ++norm_setups[1];
        }
        const uint32_t fast = dt_rand32() % 16 == 0;
        wr32(FASTFLAG, fast);
        norm_setups[0] += fast;
        call_frame(&c, 0, NULL);
        if (dt_rand32() % 32 == 0) { /* the vector in the frame */
            self = c.r[4] - 0x10u - 4u * (dt_rand32() % 4);
            for (int k = 0; k < 3; ++k)
                wrf32(self + 4u * k, smallf());
            ++norm_setups[2];
        }
        c.r[1] = self;
        run("Normalize", &c, fn_10010060, g3n_10010060);
        /* the last FCOMI len, 0 */
        if (c.eflags_pf)
            ++norm_paths[2];
        else if (c.eflags_zf)
            ++norm_paths[0];
        else
            ++norm_paths[1];
    }
    wr32(FASTFLAG, 0);
}

/* ---- bCEulerAngles::operator- ------------------------------------------------------------- */
static void test_minus(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        fill(DATA, 0x80);
        const uint32_t a = at();
        const uint32_t b = dt_rand32() % 6 == 0 ? a + 4u * (dt_rand32() % 3) : at();
        const uint32_t out = dt_rand32() % 4 == 0 ? (dt_rand32() & 1 ? a : b) + 4u * (dt_rand32() % 3) : at();
        const uint32_t args[2] = {out, b};
        ctx(&c);
        c.r[1] = a;
        call_frame(&c, 2, args);
        run("operator-", &c, fn_10011bd0, g3n_10011bd0);
    }
}

/* ---- bCMatrix copy constructor ---------------------------------------------------------- */
static void test_matcopy(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        for (uint32_t k = 0; k < 0x200u; k += 4u)
            wr32(DATA + k, dt_rand32());
        const uint32_t src = DATA + 0x80u + 4u * (dt_rand32() % 16u);
        uint32_t dst;
        switch (dt_rand32() % 4) {
        case 0: dst = src + 1u + dt_rand32() % 70u; break; /* just above: the copy repeats itself */
        case 1: dst = src - 1u - dt_rand32() % 70u; break;
        case 2: dst = src; break;
        default: dst = DATA + 0x100u + 4u * (dt_rand32() % 32u); break;
        }
        ctx(&c);
        c.r[1] = dst;
        call_frame(&c, 1, &src);
        run("bCMatrix(bCMatrix const &)", &c, fn_10015be0, g3n_10015be0);
    }
}

/* ---- bCSphere::Intersects ----------------------------------------------------------------- */
static unsigned sphere_hits[2];
static void test_sphere(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        fill(DATA, 0x100);
        uint32_t self = at(), o = dt_rand32() % 8 == 0 ? self + 4u * (dt_rand32() % 4) : at();
        if (dt_rand32() % 4) { /* radii around the distance, so both answers come up */
            for (int k = 0; k < 4; ++k) {
                wrf32(self + 4u * k, smallf());
                wrf32(o + 4u * k, smallf());
            }
            wrf32(self, fabsf(smallf()));
            wrf32(o, fabsf(smallf()));
        }
        ctx(&c);
        c.r[1] = self;
        call_frame(&c, 1, &o);
        if (dt_rand32() % 32 == 0) { /* a sphere in the frame */
            const uint32_t f = c.r[4] - 0x20u + 4u * (dt_rand32() % 4);
            fill(f, 4);
            if (dt_rand32() & 1)
                c.r[1] = f;
            else
                wr32(c.r[4] + 4u, f);
        }
        run("Sphere::Intersects", &c, fn_1001da30, g3n_1001da30);
        ++sphere_hits[c.r[0] & 1u];
    }
}

/* ---- FUN_10295f40: quaternion to matrix --------------------------------------------------- */
static void test_quat(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        fill(DATA, 0x80);
        const uint32_t q = DATA + 0x40u + 4u * (dt_rand32() % 64u);
        if (dt_rand32() % 4) {
            for (int k = 0; k < 4; ++k)
                wrf32(q + 4u * k, (float)((int32_t)(dt_rand32() % 2001u) - 1000) / 1000.0f);
        }
        uint32_t m = DATA + 0x100u + 4u * (dt_rand32() % 32u);
        if (dt_rand32() % 6 == 0)
            m = q - 0x40u + 4u * (dt_rand32() % 32u); /* overlapping the quaternion */
        ctx(&c);
        c.r[1] = q;
        c.r[0] = m;
        call_frame(&c, 0, NULL);
        if (dt_rand32() % 32 == 0) {
            const uint32_t f = c.r[4] - 0x20u;
            fill(f, 4);
            if (dt_rand32() & 1)
                c.r[1] = f;
            else
                c.r[0] = f - 0x30u;
        }
        run("QuatToMatrix 10295f40", &c, fn_10295f40, g3n_10295f40);
    }
}

/* ---- FUN_10299740: matrix product with SSE ------------------------------------------------ */
static unsigned matmul_strict;
static void test_matmul(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        fill(DATA, 0x100);
        if (dt_rand32() % 4) /* mostly ordinary values, some odd ones */
            for (int k = 0; k < 0x100; ++k)
                if (dt_rand32() % 8)
                    wrf32(DATA + 4u * k, smallf());
        const uint32_t a = DATA + 0x40u + 4u * (dt_rand32() % 64u);
        uint32_t b;
        switch (dt_rand32() % 4) {
        case 0: b = a; break;
        case 1: b = a + 4u * (dt_rand32() % 32u) - 0x40u; break;
        default: b = DATA + 0x200u + 4u * (dt_rand32() % 64u); break;
        }
        ctx(&c);
        c.r[0] = a;
        c.r[1] = b;
        call_frame(&c, 0, NULL);
        if (dt_rand32() % 32 == 0) {
            const uint32_t f = (c.r[4] & ~0xfu) - 0x50u;
            fill(f, 16);
            c.r[0] = f;
        }
        /* half the cases without a NaN in either matrix: there the comparison is exact */
        nan_relaxed = dt_rand32() & 1;
        if (!nan_relaxed) {
            for (uint32_t k = 0; k < 0x40u; k += 4u) {
                if (is_nan32(rd32(c.r[0] + k)))
                    wrf32(c.r[0] + k, smallf());
                if (is_nan32(rd32(c.r[1] + k)))
                    wrf32(c.r[1] + k, smallf());
            }
            ++matmul_strict;
        }
        run("MatMul 10299740", &c, fn_10299740, g3n_10299740);
        nan_relaxed = 0;
    }
}

/* ---- FUN_102995d0: set and Scale --------------------------------------------------------- */
static void test_setscale(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        fill(DATA, 0x100);
        const uint32_t src = at();
        const uint32_t dst = dt_rand32() % 5 == 0 ? src + 4u * (dt_rand32() % 5u) - 8u : at();
        const uint32_t args[1] = {fbits(valf())};
        ctx(&c);
        c.r[0] = src;
        c.r[6] = dst;
        call_frame(&c, 1, args);
        if (dt_rand32() % 32 == 0) { /* the vector over the pushed slot or the argument */
            static const uint32_t offs[3] = {0xfffffff0u, 0xfffffff4u, 4u};
            c.r[6] = c.r[4] + offs[dt_rand32() % 3u];
        }
        run("SetScale 102995d0", &c, fn_102995d0, g3n_102995d0);
    }
}

/* ---- eCFrustumFilter::CullFrustum --------------------------------------------------------- */
static unsigned cull_results[3]; /* 0, 1, 15 */

static void test_cull(unsigned n) {
    X86 c;
    const uint32_t self = DATA + 0x400u;
    for (unsigned i = 0; i < n; ++i) {
        /* a box {min[3], max[3]} and planes around it */
        float bx[6];
        for (int k = 0; k < 3; ++k) {
            const float lo = smallf(), ext = fabsf(smallf()) * 0.5f;
            bx[k] = lo;
            bx[k + 3] = lo + ext;
        }
        for (uint32_t k = 0; k < 0x100u; ++k)
            wrf32(BOX + 4u * k, dt_float());
        for (int k = 0; k < 6; ++k)
            wrf32(BOX + 4u * (uint32_t)k, bx[k]);
        if (dt_rand32() % 16 == 0)
            wrf32(BOX + 4u * (dt_rand32() % 6u), dt_float());
        for (uint32_t k = 0; k < 0x60u; k += 4u)
            wr32(self + k, dt_rand32());
        for (uint32_t p = 0; p < 6u; ++p) {
            const uint32_t pl = self + 0x60u + 0x10u * p;
            float nx = (float)((int32_t)(dt_rand32() % 201u) - 100) / 100.0f;
            float ny = (float)((int32_t)(dt_rand32() % 201u) - 100) / 100.0f;
            float nz = (float)((int32_t)(dt_rand32() % 201u) - 100) / 100.0f;
            float d = smallf() * (dt_rand32() % 3 ? 1.0f : 8.0f);
            if (dt_rand32() % 3 == 0)
                d = 1e5f; /* a plane the box is well inside of */
            if (dt_rand32() % 40 == 0)
                nx = dt_float();
            if (dt_rand32() % 40 == 0)
                d = dt_float();
            wrf32(pl, nx);
            wrf32(pl + 4u, ny);
            wrf32(pl + 8u, nz);
            wrf32(pl + 0xcu, d);
            /* the far and near corners as a real filter picks them, sometimes any byte */
            const uint8_t far[3] = {(uint8_t)(nx >= 0 ? 3 : 0), (uint8_t)(ny >= 0 ? 4 : 1), (uint8_t)(nz >= 0 ? 5 : 2)};
            for (uint32_t k = 0; k < 3u; ++k) {
                uint8_t f = far[k], nr = (uint8_t)(far[k] >= 3 ? far[k] - 3 : far[k] + 3);
                if (dt_rand32() % 50 == 0)
                    f = (uint8_t)dt_rand32();
                if (dt_rand32() % 50 == 0)
                    nr = (uint8_t)dt_rand32();
                wr8(self + 0xc0u + 3u * p + k, f);
                wr8(self + 0xd2u + 3u * p + k, nr);
            }
        }
        for (uint32_t k = 0xd2u + 18u; k < 0x100u; ++k)
            wr8(self + k, (uint8_t)dt_rand32());
        ctx(&c);
        c.r[1] = self;
        uint32_t box = BOX;
        call_frame(&c, 1, &box);
        if (dt_rand32() % 32 == 0) { /* the box in the frame */
            const uint32_t f = c.r[4] - 0x20u;
            for (int k = 0; k < 6; ++k)
                wrf32(f + 4u * (uint32_t)k, bx[k]);
            wr32(c.r[4] + 4u, f);
            for (uint32_t k = 0; k < 18u; ++k)
                wr8(self + 0xc0u + k, (uint8_t)(dt_rand32() % 6u));
            for (uint32_t k = 0; k < 18u; ++k)
                wr8(self + 0xd2u + k, (uint8_t)(dt_rand32() % 6u));
        }
        run("CullFrustum", &c, fn_1040c9d0, g3n_1040c9d0);
        ++cull_results[c.r[0] == 15u ? 2 : c.r[0] & 1u];
    }
}

/* ---- timing ------------------------------------------------------------------------------- */
typedef void (*fn_t)(X86 *);

static void bench(const char *name, fn_t orig, fn_t native, void (*setup)(X86 *), unsigned n) {
    X86 c0, c;
    setup(&c0);
    dt_save(0);
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
        dt_restore();
    }
    printf("  %-26s translated %6.1f ns  native %6.1f ns  x%.1f\n", name, t[0], t[1], t[0] / t[1]);
}

static void b_base(X86 *c) {
    for (int k = 0; k < 0x100; ++k)
        wrf32(DATA + 4u * k, (float)(k % 7) * 0.75f + 1.0f);
    dt_context(c, ESP0, 0);
    c->fpu_cw = 0x007f;
}
static void b_norm(X86 *c) {
    b_base(c);
    c->r[1] = DATA;
    c->r[4] -= 4;
    wr32(c->r[4], RET_SITE);
}
static void b_minus(X86 *c) {
    b_base(c);
    c->r[1] = DATA;
    const uint32_t args[2] = {DATA + 0x40u, DATA + 0x20u};
    for (int i = 1; i >= 0; --i) {
        c->r[4] -= 4;
        wr32(c->r[4], args[i]);
    }
    c->r[4] -= 4;
    wr32(c->r[4], RET_SITE);
}
static void b_matcopy(X86 *c) {
    b_minus(c);
    c->r[1] = DATA + 0x100u;
}
static void b_sphere(X86 *c) {
    b_minus(c);
}
static void b_quat(X86 *c) {
    b_base(c);
    wrf32(DATA, 0.5f), wrf32(DATA + 4, 0.5f), wrf32(DATA + 8, 0.5f), wrf32(DATA + 12, 0.5f);
    c->r[1] = DATA;
    c->r[0] = DATA + 0x100u;
    c->r[4] -= 4;
    wr32(c->r[4], RET_SITE);
}
static void b_matmul(X86 *c) {
    b_base(c);
    c->r[0] = DATA + 0x100u;
    c->r[1] = DATA;
    c->r[4] -= 4;
    wr32(c->r[4], RET_SITE);
}
static void b_setscale(X86 *c) {
    b_base(c);
    c->r[0] = DATA;
    c->r[6] = DATA + 0x100u;
    c->r[4] -= 4;
    wr32(c->r[4], fbits(1.5f));
    c->r[4] -= 4;
    wr32(c->r[4], RET_SITE);
}
static void b_cull(X86 *c) {
    const uint32_t self = DATA + 0x400u;
    b_base(c);
    for (int k = 0; k < 6; ++k)
        wrf32(BOX + 4u * (uint32_t)k, k < 3 ? -1.0f : 1.0f);
    for (uint32_t p = 0; p < 6u; ++p) {
        const uint32_t pl = self + 0x60u + 0x10u * p;
        wrf32(pl, p == 0 ? 1.0f : 0.0f);
        wrf32(pl + 4u, p == 1 ? 1.0f : 0.0f);
        wrf32(pl + 8u, p >= 2 ? 1.0f : 0.0f);
        wrf32(pl + 0xcu, 100.0f);
        for (uint32_t k = 0; k < 3u; ++k) {
            wr8(self + 0xc0u + 3u * p + k, (uint8_t)(k + 3));
            wr8(self + 0xd2u + 3u * p + k, (uint8_t)k);
        }
    }
    c->r[1] = self;
    c->r[4] -= 4;
    wr32(c->r[4], BOX);
    c->r[4] -= 4;
    wr32(c->r[4], RET_SITE);
}

int main(void) {
    harness_mem();
    dt_load_pe("SharedBase.dll");
    dt_load_pe("Engine.dll");
    dt_stack_floor = ESP0 - 0x400u;
    dt_region(dt_stack_floor, 0x400u);
    dt_region(DATA, DATA_LEN);
    dt_region(0x100d0000u, 0x5000u);
    dt_region(PTD, 0x400u);
    setup_crt();

    test_normalize(200000);
    test_minus(50000);
    test_matcopy(50000);
    test_sphere(100000);
    test_quat(100000);
    test_matmul(100000);
    test_setscale(50000);
    test_cull(200000);

    printf("Normalize: zero length %u, scaled %u, NaN length %u; set up: fast flag %u, PE pending %u, "
           "vector in the frame %u\n",
           norm_paths[0], norm_paths[1], norm_paths[2], norm_setups[0], norm_setups[1], norm_setups[2]);
    printf("MatMul: %u of 100000 without a NaN input (exact), the rest NaN-relaxed\n", matmul_strict);
    printf("Sphere::Intersects: false %u, true %u\n", sphere_hits[0], sphere_hits[1]);
    printf("CullFrustum: 0 %u, 1 %u, 15 %u\n", cull_results[0], cull_results[1], cull_results[2]);
    if (!norm_paths[0] || !norm_paths[1] || !norm_paths[2] || !sphere_hits[0] || !sphere_hits[1] ||
        !cull_results[0] || !cull_results[1] || !cull_results[2]) {
        fprintf(stderr, "FAIL: a path never taken\n");
        ++dt_fail;
    }
    printf("eflags failures %u, dead stack failures %u\n", flag_fail, dead_fail);

    if (getenv("G3N_BENCH")) {
        printf("bench (ns per call):\n");
        const unsigned n = 2000000;
        bench("Normalize", fn_10010060, g3n_10010060, b_norm, n);
        bench("operator-", fn_10011bd0, g3n_10011bd0, b_minus, n);
        bench("bCMatrix copy", fn_10015be0, g3n_10015be0, b_matcopy, n);
        bench("Sphere::Intersects", fn_1001da30, g3n_1001da30, b_sphere, n);
        bench("QuatToMatrix 10295f40", fn_10295f40, g3n_10295f40, b_quat, n);
        bench("MatMul 10299740", fn_10299740, g3n_10299740, b_matmul, n);
        bench("SetScale 102995d0", fn_102995d0, g3n_102995d0, b_setscale, n);
        bench("CullFrustum (6 planes)", fn_1040c9d0, g3n_1040c9d0, b_cull, n);
    }
    return dt_report("leaf_test");
}
