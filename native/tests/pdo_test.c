/* native/tests/pdo_test.c - native/pdo.c (group pdo) against the translated Engine code.
 *
 * Build and run (from the checkout root):
 *   NATIVE="pdo.c shadow.c particles.c" sh native/tests/build.sh pdo aux-engine/chunk_10564000.c \
 *       aux-engine/chunk_10568000.c aux-engine/chunk_103ac000.c aux-engine/chunk_103c0000.c \
 *       aux-sharedbase/chunk_10010000.c aux-sharedbase/chunk_10018000.c \
 *       aux-physxcore/chunk_11160000.c aux-physxcore/chunk_11178000.c \
 *       aux-physxcore/chunk_11188000.c aux-physxcore/chunk_11228000.c aux-physxcore/chunk_11288000.c
 * (particles.c for the native CalcValueLinear the vector scale calls).
 * G3N_BENCH=1 adds timings.
 *
 * Cases:
 *  - Engine's CRT rand 10564592, __getptd 1056bee7 and _getptd_noexit 1056be70. The originals run
 *    in full: their imports (GetLastError, TlsGetValue, SetLastError and the CRT's "FlsGetValue",
 *    which under the kit is TlsGetValue, found through a second TLS slot) are modelled the way the
 *    kit's shims behave, the TLS array at TEB + 0x1000. Then an FLS that is not the TLS slot: the
 *    first-use check must turn the direct read off and every result must still be the original's.
 *  - The depth sort: Engine's qsort 10564ec0 over particles (0xb4 bytes, position at +0xc) with
 *    the comparator 103af9c0. Under the original the translated qsort calls the translated
 *    comparator, which calls the translated SharedBase SetVector (100138d0), InvTranslate
 *    (1001ae70) and GetSquareMagnitude (10010e20) through Engine's import slots; under the native
 *    one native/shadow.c's qsort runs the comparator inline (pdo_cmp_depth). Positions are ordinary
 *    values, repeated ones, NaNs, infinities, denormals and signed zeros; 0 to 300 particles.
 *  - CalcExtents 103afae0: its particle loop runs the translated SharedBase leaves under the
 *    original (GetSquareMagnitude, GetInvTranslated 10011bd0, Merge 1001b9e0, GetTranslated
 *    10011140); the calls around it (Invalidate, IsValid, SetCoordinates, SetVector, the vtable
 *    call, GetInverted, Transform) are fakes that log what they were handed (return address, ESP,
 *    registers, x87 top/tags/IE, XMM0, arguments) to compared memory and answer from a hash of it.
 *    Particles alive or not, flagged, with zero or NaN sizes and extents; 0 to 63 of them or a
 *    negative count; this+0x84 zero or not. The master-instance path (this+0x22d == 1) runs the
 *    original in both and is not built here.
 *  - eCVectorScale::CalcValue 103c00c0 (no key, one, two to seven, eight or more, a negative
 *    count; CalcValueLinear and CalcValueBinary under it for real), CalcVelocityFactor 103af640 and
 *    CalcRevolutionFactor 103af6b0 (flag on or off, any speed and t), calling it.
 *  - PhysXCore's vector add 1122a320, subtract 11163d50, scale 1117a160 and the box centre and
 *    half extents 1118b390 / 1118b3d0, with outputs that overlap the inputs now and then.
 *  - The box-tree walk 11289670: random trees up to 8 levels (children inside their parent, now
 *    and then not, inverted or NaN boxes), leaves with and without indices, a ray of any centre,
 *    extents and direction, a list that is full now and then. The grow 11279800 it calls is a fake
 *    defined here that logs ESP, the return address, the registers and the x87 tags to compared
 *    memory.
 *  - The quantized tree walk 11289e10 the same way: int16/uint16 nodes, primitive children,
 *    the early stop flag at ray+4, a list at ray+0x90 that is full now and then.
 * Every case starts from a random context (scratch registers, x87 top with live values, stale
 * tags, precision and rounding control, XMM; difftest_rp.h) and compares registers, x87 state,
 * XMM and memory bitwise (the stack below the final ESP excepted). */
#include "difftest_rp.h"

#include <time.h>

void fn_10564592(X86 *c);
void g3n_10564592(X86 *c);
void fn_1056bee7(X86 *c);
void g3n_1056bee7(X86 *c);
void fn_1056be70(X86 *c);
void g3n_1056be70(X86 *c);
void fn_10564ec0(X86 *c);
void g3n_10564ec0(X86 *c);
void fn_103af9c0(X86 *c);
void fn_100138d0(X86 *c);
void fn_1001ae70(X86 *c);
void fn_10010e20(X86 *c);
extern int g3n_pdo_ptd_direct;

#define ESP0 0x0e800000u
#define STACK_LO (ESP0 - 0x1000u)
#define RET_SITE 0x103b6a00u /* somewhere in EmitParticles */

/* ------------------------------------------------------------------ rand */

#define FLSINDEX 5u
#define GETVALUEINDEX 6u
#define PTD 0x02000000u
#define PTD_FLS 0x02001000u /* what a real FLS would hold, for the mismatch case */

#define SHIM_GETLASTERROR 0x0ff00000u
#define SHIM_SETLASTERROR 0x0ff00010u
#define SHIM_TLSGETVALUE 0x0ff00020u
#define SHIM_FLSGETVALUE 0x0ff00030u

#define IAT_GETLASTERROR 0x105f631cu
#define IAT_SETLASTERROR 0x105f6274u
#define IAT_TLSGETVALUE 0x105f6264u
#define ENG_FLSINDEX 0x10835690u
#define ENG_GETVALUEINDEX 0x10835694u

/* the depth sort's SharedBase imports */
#define IAT_SETVECTOR 0x105f7158u
#define IAT_INVTRANSLATE 0x105f66ecu
#define IAT_GETSQUAREMAG 0x105f677cu
#define CMP_DEPTH 0x103af9c0u

static void call_frame(X86 *c, unsigned nargs, const uint32_t *args);

/* ------------------------------------------------------------------ CalcExtents */

void fn_103afae0(X86 *c);
void g3n_103afae0(X86 *c);
void fn_10011bd0(X86 *c);
void fn_10011140(X86 *c);
void fn_1001b9e0(X86 *c);

#define CE_SELF 0x03100000u   /* the particle system */
#define CE_VT 0x03100800u     /* its vtable */
#define CE_MOBJ 0x03100c00u   /* what vtable +0x6c returns (matrix at +0x40) */
#define CE_BOX 0x03101000u
#define CE_PART 0x03110000u   /* the particles */
#define CE_MAXP 64u
#define CE_LOG 0x03130000u    /* what the fakes were handed, compared memory */
#define CE_LOG_SIZE 0x4000u

enum { CF_INVALIDATE, CF_ISVALID, CF_SETCOORDS, CF_SETVECTOR, CF_INVERTED, CF_TRANSFORM, CF_VT, CF_N };
#define CE_FAKE(i) (0x0ff20000u + 0x10u * (uint32_t)(i))

static uint64_t ce_mix(uint64_t h, uint32_t v) {
    h ^= v;
    h *= 0x100000001b3ull;
    return h ^ (h >> 29);
}

/* Appends what a caller can hand a fake (return address, ESP, the registers, the x87 top, tags
 * and IE, XMM0, two stack arguments) to the log and returns a hash of it. */
static uint64_t ce_log(X86 *c, unsigned which) {
    uint32_t n = rd32(CE_LOG);
    const uint32_t rec = CE_LOG + 4u + n * 64u;
    uint64_t h = 0xcbf29ce484222325ull ^ which;
    if (rec + 64u > CE_LOG + CE_LOG_SIZE)
        return h;
    const uint32_t v[16] = {which,          rd32(c->r[4]),       c->r[4],        c->r[0],
                            c->r[1],        c->r[2],             c->r[3],        c->r[5],
                            c->r[6],        c->r[7],             c->fpu_top,     c->fpu_tag | ((uint32_t)c->fpu_sw << 16),
                            c->xmm[0][0],   c->xmm[0][1] ^ c->xmm[0][2] ^ c->xmm[0][3],
                            rd32(c->r[4] + 4u), rd32(c->r[4] + 8u)};
    for (unsigned i = 0; i < 16; ++i) {
        wr32(rec + 4u * i, v[i]);
        h = ce_mix(h, v[i]);
    }
    wr32(CE_LOG, n + 1u);
    return h;
}

static void ce_ret(X86 *c, uint32_t args) {
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u + args;
}

static void ce_fake(X86 *c, unsigned which) {
    const uint64_t h = ce_log(c, which);
    const uint32_t self = c->r[1];
    switch (which) {
    case CF_INVALIDATE:
        for (unsigned i = 0; i < 3; ++i) {
            /* the fakes answer from the hash only: both runs must see the same */
            wrf32(self + 4u * i, (h >> (8 + i)) & 1 ? 3.0e38f : (float)((int32_t)((h >> (16 + 4 * i)) & 0xffffu) - 0x8000) / 16.0f);
            wrf32(self + 0xcu + 4u * i, (h >> (12 + i)) & 1 ? -3.0e38f : (float)((int32_t)((h >> (20 + 4 * i)) & 0xffffu) - 0x8000) / 16.0f);
        }
        c->r[0] = (uint32_t)(h >> 32);
        ce_ret(c, 0);
        break;
    case CF_ISVALID: {
        int valid = 1;
        for (unsigned i = 0; i < 3; ++i)
            if (!(rdf32(self + 4u * i) <= rdf32(self + 0xcu + 4u * i)))
                valid = 0;
        if ((h & 15u) == 0)
            valid = !valid;
        c->r[0] = ((uint32_t)(h >> 32) & 0xffffff00u) | (uint32_t)valid;
        c->r[2] = (uint32_t)h;
        ce_ret(c, 0);
        break;
    }
    case CF_SETCOORDS:
        for (unsigned i = 0; i < 3; ++i)
            wr32(self + 4u * i, rd32(c->r[4] + 4u + 4u * i));
        ce_ret(c, 0xc);
        break;
    case CF_SETVECTOR: {
        const uint32_t v = rd32(c->r[4] + 4u);
        for (unsigned i = 0; i < 3; ++i)
            wr32(self + 4u * i, rd32(v + 4u * i));
        c->r[0] = v;
        ce_ret(c, 4);
        break;
    }
    case CF_INVERTED: { /* GetInverted(out): out = some function of this */
        const uint32_t out = rd32(c->r[4] + 4u);
        for (unsigned i = 0; i < 16; ++i)
            wr32(out + 4u * i, rd32(self + 4u * i) ^ (uint32_t)(h >> (i & 31)));
        ce_ret(c, 4);
        break;
    }
    case CF_TRANSFORM: { /* Transform(m): the box takes a hash of m */
        const uint32_t m = rd32(c->r[4] + 4u);
        uint64_t g = h;
        for (unsigned i = 0; i < 16; ++i)
            g = ce_mix(g, rd32(m + 4u * i));
        for (unsigned i = 0; i < 6; ++i)
            wr32(self + 4u * i, rd32(self + 4u * i) ^ (uint32_t)(g >> i));
        ce_ret(c, 4);
        break;
    }
    case CF_VT:
        c->r[0] = CE_MOBJ;
        c->r[2] = (uint32_t)h;
        ce_ret(c, 0);
        break;
    }
}

static uint32_t last_error = 0x1234;
static uint32_t shim_calls;

static uint32_t arg(X86 *c, int i) {
    return rd32(c->r[4] + 4u + 4u * (uint32_t)i);
}
static void ret_stdcall(X86 *c, int nargs, uint32_t eax) {
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u + 4u * (uint32_t)nargs;
    c->r[0] = eax;
}

void recomp_call_ic(X86 *c, uint32_t target, uint64_t *ic) {
    (void)ic;
    switch (target) {
    case SHIM_GETLASTERROR:
        ++shim_calls;
        ret_stdcall(c, 0, last_error);
        return;
    case SHIM_SETLASTERROR:
        ++shim_calls;
        last_error = arg(c, 0);
        ret_stdcall(c, 1, 0);
        return;
    case SHIM_TLSGETVALUE: {
        ++shim_calls;
        const uint32_t i = arg(c, 0);
        ret_stdcall(c, 1, i < 64 ? rd32(c->fs_base + 0x1000u + 4u * i) : 0);
        return;
    }
    case SHIM_FLSGETVALUE:
        ++shim_calls;
        ret_stdcall(c, 1, arg(c, 0) == FLSINDEX ? PTD_FLS : 0);
        return;
    case CMP_DEPTH:
        fn_103af9c0(c);
        return;
    case 0x100138d0u:
        fn_100138d0(c);
        return;
    case 0x1001ae70u:
        fn_1001ae70(c);
        return;
    case 0x10010e20u:
        fn_10010e20(c);
        return;
    case 0x10011bd0u:
        fn_10011bd0(c);
        return;
    case 0x10011140u:
        fn_10011140(c);
        return;
    case 0x1001b9e0u:
        fn_1001b9e0(c);
        return;
    }
    if (target >= CE_FAKE(0) && target < CE_FAKE(CF_N) && (target & 0xfu) == 0u) {
        ce_fake(c, (target - CE_FAKE(0)) >> 4);
        return;
    }
    fprintf(stderr, "unexpected indirect call to %08x\n", target);
    abort();
}

static void setup_crt(uint32_t flsget) {
    wr32(IAT_GETLASTERROR, SHIM_GETLASTERROR);
    wr32(IAT_SETLASTERROR, SHIM_SETLASTERROR);
    wr32(IAT_TLSGETVALUE, SHIM_TLSGETVALUE);
    wr32(ENG_FLSINDEX, FLSINDEX);
    wr32(ENG_GETVALUEINDEX, GETVALUEINDEX);
    wr32(GUEST_TEB_BASE + 0x1000u + 4u * FLSINDEX, PTD);
    wr32(GUEST_TEB_BASE + 0x1000u + 4u * GETVALUEINDEX, flsget);
}

static void call_frame(X86 *c, unsigned nargs, const uint32_t *args) {
    for (unsigned i = nargs; i-- > 0;) {
        c->r[4] -= 4;
        wr32(c->r[4], args[i]);
    }
    c->r[4] -= 4;
    wr32(c->r[4], RET_SITE);
}

static void test_rand(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        wr32(PTD + 0x14u, dt_rand32());
        dt_context(&c, ESP0, dt_rand32() % 4);
        call_frame(&c, 0, NULL);
        dt_run("rand", &c, fn_10564592, g3n_10564592);
        call_frame(&c, 0, NULL);
        dt_run("rand again", &c, fn_10564592, g3n_10564592);
    }
}

static void test_getptd(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        dt_context(&c, ESP0, dt_rand32() % 4);
        call_frame(&c, 0, NULL);
        if (i & 1)
            dt_run("_getptd_noexit", &c, fn_1056be70, g3n_1056be70);
        else
            dt_run("__getptd", &c, fn_1056bee7, g3n_1056bee7);
    }
}

static void check(int ok, const char *what, int v) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s (%d)\n", what, v);
        ++dt_fail;
    }
}

static void test_crt(void) {
    setup_crt(SHIM_TLSGETVALUE);
    g3n_pdo_ptd_direct = 0;
    test_rand(3000);
    check(g3n_pdo_ptd_direct == 1, "direct TLS read not enabled", g3n_pdo_ptd_direct);
    shim_calls = 0;
    {
        X86 c;
        dt_context(&c, ESP0, 0);
        call_frame(&c, 0, NULL);
        g3_opt = 0xffffffffu;
        g3n_10564592(&c);
        call_frame(&c, 0, NULL);
        g3n_1056bee7(&c);
        call_frame(&c, 0, NULL);
        g3n_1056be70(&c);
        check(shim_calls == 0 && c.r[0] == PTD, "native rand/_getptd made shim calls", (int)shim_calls);
    }
    g3n_pdo_ptd_direct = 0;
    test_getptd(2000);
    check(g3n_pdo_ptd_direct == 1, "direct TLS read not enabled by __getptd", g3n_pdo_ptd_direct);

    setup_crt(SHIM_FLSGETVALUE);
    g3n_pdo_ptd_direct = 0;
    test_rand(500);
    test_getptd(500);
    check(g3n_pdo_ptd_direct == -1, "mismatch not detected", g3n_pdo_ptd_direct);
    g3n_pdo_ptd_direct = 0;
    test_getptd(500);
    test_rand(200);
    check(g3n_pdo_ptd_direct == -1, "mismatch not detected by __getptd", g3n_pdo_ptd_direct);
    setup_crt(SHIM_TLSGETVALUE);
    g3n_pdo_ptd_direct = 0;
}

/* ------------------------------------------------------------------ the depth sort */

#define ARR 0x03000000u
#define PSIZE 0xb4u
#define MAXP 300u
#define EYE 0x10868b6cu

static float coord(unsigned style) {
    switch (style) {
    case 0: return dt_float();
    case 1: return (float)(dt_rand32() % 5u);                 /* many equal distances */
    case 2: return (float)((int32_t)(dt_rand32() % 20001u) - 10000) / 7.0f; /* a world's range */
    case 3: return (dt_rand32() & 7u) ? (float)(dt_rand32() % 50u) : dt_bits(0x7fc00000u);
    case 4: return (dt_rand32() & 1u) ? 0.0f : -0.0f;
    default: return (float)(int32_t)dt_rand32() / 65536.0f;
    }
}

static unsigned pick_n(void) {
    switch (dt_rand32() % 8u) {
    case 0: return dt_rand32() % 4u;
    case 1: return 4u + dt_rand32() % 8u; /* around the shortsort threshold */
    case 2: return MAXP;
    default: return dt_rand32() % MAXP;
    }
}

static unsigned depth_cases[2];

static void depth_case(void) {
    const unsigned n = pick_n();
    const unsigned style = dt_rand32() % 6u;
    for (uint32_t a = ARR; a < ARR + MAXP * PSIZE; a += 4)
        wr32(a, dt_rand32());
    for (unsigned i = 0; i < n; ++i)
        for (unsigned k = 0; k < 3; ++k)
            wrf32(ARR + i * PSIZE + 0xcu + 4u * k, coord(style));
    if (n > 2 && dt_rand32() % 4 == 0) /* duplicates of whole positions */
        for (unsigned j = 0; j < n / 3; ++j) {
            const unsigned a = dt_rand32() % n, b = dt_rand32() % n;
            for (unsigned k = 0; k < 3; ++k)
                wr32(ARR + b * PSIZE + 0xcu + 4u * k, rd32(ARR + a * PSIZE + 0xcu + 4u * k));
        }
    for (unsigned k = 0; k < 3; ++k)
        wrf32(EYE + 4u * k, style == 0 && dt_rand32() % 8 == 0 ? dt_float() : coord(2));
    X86 c;
    dt_context(&c, ESP0, dt_rand32() % 5);
    const uint32_t args[4] = {ARR, n, PSIZE, CMP_DEPTH};
    call_frame(&c, 4, args);
    dt_run("depth sort", &c, fn_10564ec0, g3n_10564ec0);
    ++depth_cases[n > 8];
}

static void test_depth(unsigned n) {
    wr32(IAT_SETVECTOR, 0x100138d0u);
    wr32(IAT_INVTRANSLATE, 0x1001ae70u);
    wr32(IAT_GETSQUAREMAG, 0x10010e20u);
    for (unsigned i = 0; i < n; ++i)
        depth_case();
}

static float ce_pos(void) {
    return dt_rand32() % 16 ? (float)((int32_t)(dt_rand32() % 20001u) - 10000) / 3.0f : dt_float();
}

static unsigned ce_paths[4]; /* invalid box, valid with transform, valid without, loop merged */

static void ce_case(void) {
    wr32(CE_LOG, 0);
    for (uint32_t a = CE_SELF; a < CE_SELF + 0x400u; a += 4)
        wr32(a, dt_rand32());
    wr32(CE_SELF, CE_VT);
    wr8(CE_SELF + 0x22du, 0);
    for (uint32_t a = CE_MOBJ; a < CE_MOBJ + 0x80u; a += 4)
        wr32(a, dt_rand32());
    const unsigned n = dt_rand32() % 8 == 0 ? 0 : dt_rand32() % CE_MAXP;
    wr32(CE_SELF + 0x31cu, dt_rand32() % 16 == 0 ? (uint32_t)-(int32_t)(dt_rand32() % 3) : n);
    wr32(CE_SELF + 0x318u, CE_PART);
    wr32(CE_SELF + 0x84u, dt_rand32() % 3 == 0 ? dt_rand32() : 0u);
    const unsigned style = dt_rand32() % 4; /* 0: mostly live particles, 3: anything */
    for (unsigned i = 0; i < CE_MAXP; ++i) {
        const uint32_t p = CE_PART + 0xb4u * i;
        for (uint32_t a = p; a < p + 0xb4u; a += 4)
            wr32(a, dt_rand32());
        const float life = (float)(dt_rand32() % 1000) / 100.0f;
        wrf32(p + 0xa4u, style == 3 && dt_rand32() % 2 ? dt_float() : life);
        wrf32(p + 0xa0u, style == 3 && dt_rand32() % 2 ? dt_float() : dt_rand32() % 6 ? life * 0.5f : life + 1.0f);
        wr32(p + 0xb0u, dt_rand32() % 5 ? dt_rand32() & ~1u : dt_rand32());
        wrf32(p + 0x88u, dt_rand32() % 6 ? 1.0f : style ? dt_float() : 0.0f);
        wrf32(p + 0x9cu, dt_rand32() % 6 ? 0.5f : style ? dt_float() : -1.0f);
        for (unsigned k = 0; k < 3; ++k) {
            wrf32(p + 0xcu + 4u * k, ce_pos());
            wrf32(p + 0x60u + 4u * k, dt_rand32() % 10 ? (float)(dt_rand32() % 100) / 8.0f : style ? dt_float() : 0.0f);
            wrf32(p + 0x6cu + 4u * k, dt_rand32() % 10 ? 1.0f : style ? dt_float() : 0.0f);
        }
    }
    for (unsigned i = 0; i < 6; ++i)
        wrf32(CE_BOX + 4u * i, dt_float());
    X86 c;
    dt_context(&c, ESP0 - 4u * (dt_rand32() % 4), dt_rand32() % 4);
    c.r[1] = CE_SELF;
    const uint32_t box = CE_BOX;
    call_frame(&c, 1, &box);
    dt_run("CalcExtents", &c, fn_103afae0, g3n_103afae0);
    const uint32_t nlog = rd32(CE_LOG);
    for (uint32_t i = 0; i < nlog; ++i) {
        const uint32_t which = rd32(CE_LOG + 4u + 64u * i);
        if (which == CF_SETCOORDS)
            ++ce_paths[0];
        if (which == CF_TRANSFORM)
            ++ce_paths[1];
    }
    if (rd32(CE_SELF + 0x394u) != 0)
        ++ce_paths[3];
}

static void test_calc_extents(unsigned n) {
    wr32(0x105f677cu, 0x10010e20u);
    wr32(0x105f6754u, 0x10011bd0u);
    wr32(0x105f6c34u, 0x1001b9e0u);
    wr32(0x105f6774u, 0x10011140u);
    wr32(0x105f66a8u, CE_FAKE(CF_INVALIDATE));
    wr32(0x105f66b4u, CE_FAKE(CF_ISVALID));
    wr32(0x105f649cu, CE_FAKE(CF_SETCOORDS));
    wr32(0x105f7158u, CE_FAKE(CF_SETVECTOR));
    wr32(0x105f68a8u, CE_FAKE(CF_INVERTED));
    wr32(0x105f6a64u, CE_FAKE(CF_TRANSFORM));
    wr32(CE_VT + 0x6cu, CE_FAKE(CF_VT));
    for (unsigned i = 0; i < n; ++i)
        ce_case();
    /* back to the depth sort's SetVector */
    wr32(0x105f7158u, 0x100138d0u);
}

/* ------------------------------------------------------------------ the vector scales */

void fn_103c00c0(X86 *c);
void g3n_103c00c0(X86 *c);
void fn_103af640(X86 *c);
void g3n_103af640(X86 *c);
void fn_103af6b0(X86 *c);
void g3n_103af6b0(X86 *c);

#define VS_OBJ 0x03140000u  /* the particle system (scales at +0x3bc, +0x3c8) or a scale */
#define VS_KEYS 0x03141000u
#define VS_OUT 0x03142000u

/* A vector scale at `self`: {keys, count}, keys {t, x, y, z}; returns a t worth asking about. */
static float vs_scale(uint32_t self) {
    int32_t count;
    switch (dt_rand32() % 8) {
    case 0: count = 0; break;
    case 1: count = 1; break;
    case 2: count = -(int32_t)(dt_rand32() % 3) - 1; break;
    case 3: count = 8 + (int32_t)(dt_rand32() % 8); break;
    default: count = 2 + (int32_t)(dt_rand32() % 6); break;
    }
    wr32(self, VS_KEYS);
    wr32(self + 4u, (uint32_t)count);
    const int n = count > 0 ? count : 2;
    float tk = 0.0f;
    for (int i = 0; i < n; ++i) {
        if (i == 0 || dt_rand32() % 6)
            tk += (float)(dt_rand32() % 1000) / 1000.0f;
        wrf32(VS_KEYS + 16u * (uint32_t)i, dt_rand32() % 10 ? tk : dt_float());
        for (unsigned v = 0; v < 3; ++v)
            wrf32(VS_KEYS + 16u * (uint32_t)i + 4u + 4u * v,
                  dt_rand32() % 6 ? (float)(dt_rand32() % 2000) / 1000.0f : dt_float());
    }
    switch (dt_rand32() % 5) {
    case 0: return dt_float();
    case 1: return tk + 1.0f;
    case 2: return -0.5f;
    default: return (float)(dt_rand32() % 1000) / 1000.0f * (tk + 0.5f);
    }
}

static void vs_frame(X86 *c, float t, uint32_t out) {
    uint32_t tb;
    memcpy(&tb, &t, 4);
    const uint32_t args[2] = {tb, out};
    call_frame(c, 2, args);
}

static void test_vector_scales(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        const uint32_t out = VS_OUT + 4u * (dt_rand32() % 4);
        switch (dt_rand32() % 3) {
        case 0: {
            const float t = vs_scale(VS_OBJ);
            dt_context(&c, ESP0, dt_rand32() % 4);
            c.r[1] = VS_OBJ;
            vs_frame(&c, t, out);
            dt_run("CalcValue", &c, fn_103c00c0, g3n_103c00c0);
            break;
        }
        case 1:
        case 2: {
            const int rev = dt_rand32() & 1;
            for (uint32_t a = VS_OBJ; a < VS_OBJ + 0x400u; a += 4)
                wr32(a, dt_rand32());
            vs_scale(VS_OBJ + (rev ? 0x3bcu : 0x3c8u));
            const uint32_t flag = rev ? 0x194u : 0xd0u;
            wr8(VS_OBJ + flag, dt_rand32() % 5 ? 1u : 0u);
            wrf32(VS_OBJ + flag + 4u, dt_rand32() % 6 ? (float)(dt_rand32() % 4000) / 1000.0f : dt_float());
            const float t = dt_rand32() % 6 ? (float)(dt_rand32() % 1000) / 999.0f : dt_float();
            dt_context(&c, ESP0, dt_rand32() % 4);
            c.r[1] = VS_OBJ;
            vs_frame(&c, t, out);
            if (rev)
                dt_run("CalcRevolutionFactor", &c, fn_103af6b0, g3n_103af6b0);
            else
                dt_run("CalcVelocityFactor", &c, fn_103af640, g3n_103af640);
            break;
        }
        }
    }
}

/* ------------------------------------------------------------------ PhysX leaves */

void fn_1122a320(X86 *c);
void g3n_1122a320(X86 *c);
void fn_11163d50(X86 *c);
void g3n_11163d50(X86 *c);
void fn_1117a160(X86 *c);
void g3n_1117a160(X86 *c);
void fn_1118b390(X86 *c);
void g3n_1118b390(X86 *c);
void fn_1118b3d0(X86 *c);
void g3n_1118b3d0(X86 *c);

#define PX_DATA 0x03150000u /* vectors and boxes, outputs among them (overlaps too) */

static float px_float(void) {
    return dt_rand32() % 8 ? (float)((int32_t)(dt_rand32() % 200001u) - 100000) / 64.0f : dt_float();
}

static void test_physx_leaves(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        for (uint32_t a = PX_DATA; a < PX_DATA + 0x40u; a += 4)
            wrf32(a, px_float());
        const uint32_t self = PX_DATA + 4u * (dt_rand32() % 4);
        const uint32_t b = PX_DATA + 0x10u + 4u * (dt_rand32() % 4);
        const uint32_t out = dt_rand32() % 4 ? PX_DATA + 0x30u : PX_DATA + 4u * (dt_rand32() % 12); /* may overlap */
        dt_context(&c, ESP0, dt_rand32() % 4);
        c.r[1] = self;
        switch (dt_rand32() % 5) {
        case 0: {
            const uint32_t args[2] = {out, b};
            call_frame(&c, 2, args);
            dt_run("PhysX add", &c, fn_1122a320, g3n_1122a320);
            break;
        }
        case 1: {
            const uint32_t args[2] = {out, b};
            call_frame(&c, 2, args);
            dt_run("PhysX sub", &c, fn_11163d50, g3n_11163d50);
            break;
        }
        case 2: {
            const float s = dt_rand32() % 3 ? 0.5f : px_float();
            uint32_t sb;
            memcpy(&sb, &s, 4);
            const uint32_t args[2] = {out, sb};
            call_frame(&c, 2, args);
            dt_run("PhysX scale", &c, fn_1117a160, g3n_1117a160);
            break;
        }
        case 3:
        case 4: {
            const uint32_t args[1] = {out};
            call_frame(&c, 1, args);
            if (dt_rand32() & 1)
                dt_run("PhysX box centre", &c, fn_1118b390, g3n_1118b390);
            else
                dt_run("PhysX box extents", &c, fn_1118b3d0, g3n_1118b3d0);
            break;
        }
        }
    }
}

/* ------------------------------------------------------------------ PhysX box tree walk */

void fn_11289670(X86 *c);
void g3n_11289670(X86 *c);

#define TW_RAY 0x03160000u
#define TW_LIST 0x03160100u
#define TW_NODES 0x03161000u /* 0x24 bytes each */
#define TW_MAXN 512u
#define TW_PRIMS 0x03170000u
#define TW_DATA 0x03172000u /* the list's data, room for 0x800 dwords */
#define TW_LOG 0x03180000u  /* what the grow fake was handed */

static unsigned tw_next, tw_grows;

/* The grow (thiscall list, (count), RET 4) the translated walk calls directly: logs ESP, its
 * return address, the registers and the x87 tags, makes room, clobbers the scratch registers. */
void fn_11279800(X86 *c) {
    const uint32_t n = rd32(TW_LOG);
    const uint32_t rec = TW_LOG + 4u + 48u * n;
    if (n < 64u) {
        const uint32_t v[12] = {rd32(c->r[4]), c->r[4], c->r[0], c->r[1], c->r[2], c->r[3],
                                c->r[5], c->r[6], c->r[7], rd32(c->r[4] + 4u), c->fpu_tag | ((uint32_t)c->fpu_top << 16),
                                c->fpu_sw};
        for (unsigned i = 0; i < 12; ++i)
            wr32(rec + 4u * i, v[i]);
        wr32(TW_LOG, n + 1u);
    }
    const uint32_t list = c->r[1], cnt = rd32(c->r[4] + 4u);
    wr32(list, rd32(list + 4u) + cnt + (rd32(list) & 3u));
    c->r[0] = 0x5a5a0000u ^ cnt;
    c->r[1] = 0x1234u;
    c->r[2] = rd32(list);
    c->eip = rd32(c->r[4]);
    c->r[4] += 8u;
    ++tw_grows;
}

static float tw_coord(void) {
    return dt_rand32() % 32 ? (float)((int32_t)(dt_rand32() % 2001u) - 1000) / 10.0f : dt_float();
}

/* A node inside the box lo..hi (sometimes not), children while depth allows. */
static void tw_node(uint32_t node, const float lo[3], const float hi[3], unsigned depth) {
    float a[3], b[3];
    for (int k = 0; k < 3; ++k) {
        const float w = hi[k] - lo[k];
        float u = lo[k] + w * (float)(dt_rand32() % 1000) / 1000.0f, v = lo[k] + w * (float)(dt_rand32() % 1000) / 1000.0f;
        if (dt_rand32() % 40 == 0)
            u = tw_coord();
        if (u > v && dt_rand32() % 8) {
            const float t = u;
            u = v;
            v = t;
        }
        a[k] = u;
        b[k] = v;
        wrf32(node + 4u * (uint32_t)k, u);
        wrf32(node + 0xcu + 4u * (uint32_t)k, v);
    }
    if (depth > 0 && tw_next + 2u <= TW_MAXN && dt_rand32() % 6) {
        const uint32_t kids = TW_NODES + 0x24u * tw_next;
        tw_next += 2u;
        wr32(node + 0x18u, kids | (dt_rand32() & 1u));
        wr32(node + 0x1cu, dt_rand32());
        wr32(node + 0x20u, dt_rand32());
        tw_node(kids, a, b, depth - 1u);
        tw_node(kids + 0x24u, a, b, depth - 1u);
    } else {
        wr32(node + 0x18u, dt_rand32() & 1u);
        wr32(node + 0x1cu, dt_rand32() % 6 ? TW_PRIMS + 4u * (dt_rand32() % 256) : 0u);
        wr32(node + 0x20u, dt_rand32() % 6 ? 1u + dt_rand32() % 4 : 0u);
    }
}

static void test_tree_walk(unsigned n) {
    for (uint32_t a = TW_PRIMS; a < TW_PRIMS + 0x800u; a += 4)
        wr32(a, dt_rand32());
    X86 c;
    unsigned visits = 0;
    for (unsigned i = 0; i < n; ++i) {
        wr32(TW_LOG, 0);
        tw_next = 1;
        const float lo[3] = {-100.0f, -100.0f, -100.0f}, hi[3] = {100.0f, 100.0f, 100.0f};
        tw_node(TW_NODES, lo, hi, 1u + dt_rand32() % 7);
        /* the ray: a segment's centre (+0x40), half direction (+0x34) and its absolute value
         * (+0x28), as the caller sets them up (OPCODE's segment test); now and then unrelated */
        for (int k = 0; k < 3; ++k) {
            wrf32(TW_RAY + 0x40u + 4u * (uint32_t)k, (float)((int32_t)(dt_rand32() % 2001u) - 1000) / 10.0f);
            const float d = dt_rand32() % 16 ? (float)((int32_t)(dt_rand32() % 2001u) - 1000) / (i % 3 == 0 ? 1000.0f : (i & 1) ? 10.0f : 100.0f) : dt_float();
            wrf32(TW_RAY + 0x34u + 4u * (uint32_t)k, d);
            /* a third of the rays wide enough to reach the leaves */
            wrf32(TW_RAY + 0x28u + 4u * (uint32_t)k,
                  i % 3 == 0 ? (i % 6 ? 10000.0f : 100.0f) + (float)(dt_rand32() % 2000u) / 10.0f
                             : dt_rand32() % 8 ? fabsf(d) : (float)(dt_rand32() % 3000u) / 10.0f);
        }
        wr32(TW_RAY + 0x60u, dt_rand32() % 4 ? dt_rand32() % 100 : dt_rand32());
        wr32(TW_LIST, dt_rand32() % 3 ? 0x700u : dt_rand32() % 8);
        wr32(TW_LIST + 4u, dt_rand32() % 4);
        wr32(TW_LIST + 8u, TW_DATA);
        dt_context(&c, ESP0, dt_rand32() % 3);
        c.r[1] = TW_RAY;
        const uint32_t args[2] = {TW_NODES, TW_LIST};
        call_frame(&c, 2, args);
        const uint32_t cnt0 = rd32(TW_RAY + 0x60u);
        dt_run("box tree walk", &c, fn_11289670, g3n_11289670);
        visits += rd32(TW_RAY + 0x60u) - cnt0;
        if (getenv("TW_DEBUG") && i == 3)
            for (unsigned k = 0; k < tw_next; ++k) {
                const uint32_t nd = TW_NODES + 0x24u * k;
                printf("node %u: %g %g %g / %g %g %g kid %08x prims %08x n %u\n", k, rdf32(nd), rdf32(nd + 4), rdf32(nd + 8),
                       rdf32(nd + 12), rdf32(nd + 16), rdf32(nd + 20), rd32(nd + 0x18), rd32(nd + 0x1c), rd32(nd + 0x20));
            }
        if (getenv("TW_DEBUG") && i < 40)
            printf("case %u: %u nodes in tree, %u visited, list size %u\n", i, tw_next, rd32(TW_RAY + 0x60u) - cnt0, rd32(TW_LIST + 4u));
    }
    printf("box tree walk: %u nodes visited, %u grows\n", visits, tw_grows);
}

/* ------------------------------------------------------------------ PhysX quantized box tree walk */

void fn_11289e10(X86 *c);
void g3n_11289e10(X86 *c);

#define QW_RAY 0x03190000u   /* the ray, its list at +0x90 */
#define QW_NODES 0x03191000u /* 0x14 bytes each */
#define QW_MAXN 512u
#define QW_DATA 0x03194000u  /* the list's data, room for 0x800 dwords */

static unsigned qw_next;

static uint32_t qw_child(unsigned depth, int lo[3], int hi[3]);

/* A quantized node inside lo..hi (int16 centre, uint16 half extents), children while depth allows. */
static void qw_node(uint32_t node, int lo[3], int hi[3], unsigned depth) {
    int a[3], b[3];
    for (int k = 0; k < 3; ++k) {
        int u = lo[k] + (int)(dt_rand32() % (uint32_t)(hi[k] - lo[k] + 1));
        int v = lo[k] + (int)(dt_rand32() % (uint32_t)(hi[k] - lo[k] + 1));
        if (u > v) {
            const int t = u;
            u = v;
            v = t;
        }
        a[k] = u;
        b[k] = v;
        int ctr = (u + v) / 2, ext = (v - u) / 2 + 1;
        if (dt_rand32() % 50 == 0)
            ctr = (int16_t)dt_rand32(), ext = (uint16_t)dt_rand32();
        wr16(node + 2u * (uint32_t)k, (uint16_t)(int16_t)ctr);
        wr16(node + 6u + 2u * (uint32_t)k, (uint16_t)ext);
    }
    wr32(node + 0xcu, qw_child(depth, a, b));
    wr32(node + 0x10u, qw_child(depth, a, b));
}

static uint32_t qw_child(unsigned depth, int lo[3], int hi[3]) {
    if (depth == 0 || qw_next >= QW_MAXN || dt_rand32() % 5 == 0)
        return (dt_rand32() % 1000u) * 2u + 1u;
    const uint32_t n = QW_NODES + 0x14u * qw_next++;
    qw_node(n, lo, hi, depth - 1u);
    return n;
}

static void test_qtree_walk(unsigned n) {
    X86 c;
    unsigned visits = 0, appended = 0;
    for (unsigned i = 0; i < n; ++i) {
        wr32(TW_LOG, 0);
        qw_next = 1;
        int lo[3] = {-30000, -30000, -30000}, hi[3] = {30000, 30000, 30000};
        qw_node(QW_NODES, lo, hi, 1u + dt_rand32() % 7);
        for (int k = 0; k < 3; ++k) {
            const float s = dt_rand32() % 16 ? 0.01f : dt_float();
            wrf32(QW_RAY + 0x6cu + 4u * (uint32_t)k, s);
            wrf32(QW_RAY + 0x78u + 4u * (uint32_t)k, dt_rand32() % 16 ? s : dt_float());
            wrf32(QW_RAY + 0x40u + 4u * (uint32_t)k, (float)((int32_t)(dt_rand32() % 6001u) - 3000) / 10.0f);
            const float d = dt_rand32() % 16 ? (float)((int32_t)(dt_rand32() % 2001u) - 1000) / (i % 3 == 0 ? 1000.0f : 10.0f) : dt_float();
            wrf32(QW_RAY + 0x34u + 4u * (uint32_t)k, d);
            wrf32(QW_RAY + 0x28u + 4u * (uint32_t)k, i % 3 == 0 ? 3000.0f : dt_rand32() % 8 ? fabsf(d) : (float)(dt_rand32() % 3000u) / 10.0f);
        }
        wr32(QW_RAY + 4u, dt_rand32() % 4 ? dt_rand32() & ~4u : dt_rand32() | 5u);
        wr32(QW_RAY + 0x60u, dt_rand32() % 100);
        const uint32_t size = dt_rand32() % 4;
        wr32(QW_RAY + 0x90u, dt_rand32() % 3 ? 0x700u : size + dt_rand32() % 3);
        wr32(QW_RAY + 0x94u, size);
        wr32(QW_RAY + 0x98u, QW_DATA);
        dt_context(&c, ESP0, dt_rand32() % 3);
        c.r[1] = QW_RAY;
        const uint32_t args[1] = {QW_NODES};
        call_frame(&c, 1, args);
        const uint32_t cnt0 = rd32(QW_RAY + 0x60u);
        dt_run("quantized box tree walk", &c, fn_11289e10, g3n_11289e10);
        visits += rd32(QW_RAY + 0x60u) - cnt0;
        appended += rd32(QW_RAY + 0x94u) - size;
    }
    printf("quantized box tree walk: %u nodes visited, %u primitives appended\n", visits, appended);
}

/* ------------------------------------------------------------------ timing */

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

/* CalcExtents over 60 live particles, the fakes answering as above (no log once it is full). */
/* The box-tree walk over a full tree of 127 nodes that the ray reaches everywhere. */
static void bench_walk(void) {
    for (int native = 0; native < 3; ++native) {
        g3_opt = native == 1 ? 0xffffffffu : native == 2 ? ~(uint32_t)(G3OPT_PDO | G3OPT_EXEC) : 0u;
        double t = 0;
        const unsigned reps = 20000;
        for (unsigned r = 0; r < reps; ++r) {
            if (native == 0 && r == 0) {
                tw_next = 1;
                const float lo[3] = {-100.0f, -100.0f, -100.0f}, hi[3] = {100.0f, 100.0f, 100.0f};
                tw_node(TW_NODES, lo, hi, 6);
                for (int k = 0; k < 3; ++k) {
                    wrf32(TW_RAY + 0x40u + 4u * (uint32_t)k, 0.0f);
                    wrf32(TW_RAY + 0x34u + 4u * (uint32_t)k, 0.01f);
                    wrf32(TW_RAY + 0x28u + 4u * (uint32_t)k, 5000.0f);
                }
            }
            wr32(TW_LIST, 0x700u);
            wr32(TW_LIST + 4u, 0u);
            wr32(TW_LIST + 8u, TW_DATA);
            X86 c;
            dt_context(&c, ESP0, 0);
            c.fpu_cw = 0x007f;
            c.r[1] = TW_RAY;
            const uint32_t args[2] = {TW_NODES, TW_LIST};
            call_frame(&c, 2, args);
            const double t0 = now();
            (native == 1 ? g3n_11289670 : fn_11289670)(&c);
            t += now() - t0;
        }
        printf("bench box tree walk (%u nodes) %s: %.2f us\n", tw_next, native == 1 ? "native" : native == 2 ? "pdo off" : "original",
               t * 1e6 / reps);
    }
}

static void bench_calc_extents(void) {
    test_calc_extents(0);
    wr32(0x105f7158u, CE_FAKE(CF_SETVECTOR));
    for (int native = 0; native < 3; ++native) {
        g3_opt = native == 1 ? 0xffffffffu : native == 2 ? ~(uint32_t)(G3OPT_PDO | G3OPT_EXEC) : 0u;
        wr32(CE_SELF + 0x31cu, 60u);
        wr32(CE_SELF + 0x84u, 0u);
        for (unsigned i = 0; i < 60u; ++i) {
            const uint32_t p = CE_PART + 0xb4u * i;
            wrf32(p + 0xa4u, 2.0f);
            wrf32(p + 0xa0u, 1.0f);
            wr32(p + 0xb0u, 0u);
            wrf32(p + 0x88u, 1.0f);
            wrf32(p + 0x9cu, 1.0f);
            for (unsigned k = 0; k < 3; ++k) {
                wrf32(p + 0xcu + 4u * k, (float)(i * 3 + k));
                wrf32(p + 0x60u + 4u * k, 0.5f);
                wrf32(p + 0x6cu + 4u * k, 1.0f);
            }
        }
        const unsigned reps = 20000;
        X86 c;
        double t = 0;
        for (unsigned r = 0; r < reps; ++r) {
            wr32(CE_LOG, CE_LOG_SIZE / 64u);
            dt_context(&c, ESP0, 0);
            c.fpu_cw = 0x007f;
            c.r[1] = CE_SELF;
            const uint32_t box = CE_BOX;
            call_frame(&c, 1, &box);
            const double t0 = now();
            (native == 1 ? g3n_103afae0 : fn_103afae0)(&c);
            t += now() - t0;
        }
        printf("bench CalcExtents of 60 %s: %.2f us\n", native == 1 ? "native" : native == 2 ? "pdo off" : "original",
               t * 1e6 / reps);
    }
    wr32(0x105f7158u, 0x100138d0u);
}

static void bench(void) {
    X86 c;
    for (int native = getenv("G3N_BENCH_NATIVE") ? 1 : 0; native < 3; ++native) {
        g3_opt = native == 1 ? 0xffffffffu : native == 2 ? ~(uint32_t)(G3OPT_PDO | G3OPT_EXEC) : 0u; /* 2: the game before pdo */
        dt_context(&c, ESP0, 0);
        c.fpu_cw = 0x007f;
        const unsigned reps = 200000;
        double t0 = now();
        for (unsigned i = 0; i < reps; ++i) {
            call_frame(&c, 0, NULL);
            (native ? g3n_10564592 : fn_10564592)(&c);
        }
        double t1 = now();
        printf("bench rand %s: %.1f ns\n", native == 1 ? "native" : native == 2 ? "pdo off" : "original", (t1 - t0) * 1e9 / reps);
        const unsigned n = 200, sorts = getenv("G3N_SORTS") ? (unsigned)atoi(getenv("G3N_SORTS")) : 300u;
        double tsum = 0;
        for (unsigned s = 0; s < sorts; ++s) {
            for (unsigned i = 0; i < n; ++i)
                for (unsigned k = 0; k < 3; ++k)
                    wrf32(ARR + i * PSIZE + 0xcu + 4u * k, coord(2));
            for (unsigned k = 0; k < 3; ++k)
                wrf32(EYE + 4u * k, coord(2));
            const uint32_t args[4] = {ARR, n, PSIZE, CMP_DEPTH};
            dt_context(&c, ESP0, 0);
            c.fpu_cw = 0x007f;
            call_frame(&c, 4, args);
            t0 = now();
            (native ? g3n_10564ec0 : fn_10564ec0)(&c);
            tsum += now() - t0;
        }
        printf("bench depth sort of %u %s: %.1f us\n", n, native == 1 ? "native" : native == 2 ? "shadow qsort, guest comparator" : "original", tsum * 1e6 / sorts);
    }
}

int main(void) {
    harness_mem();
    dt_load_pe("Engine.dll");
    dt_stack_floor = STACK_LO;
    dt_region(STACK_LO, 0x1100u);
    dt_region(PTD, 0x2000u);
    dt_region(GUEST_TEB_BASE, 0x1100u);
    dt_region(0x10835000u, 0x1000u);
    dt_region(ARR, MAXP * PSIZE);
    dt_region(EYE & ~0xfffu, 0x1000u);
    dt_region(CE_SELF, 0x1100u);
    dt_region(CE_PART, CE_MAXP * 0xb4u);
    dt_region(CE_LOG, CE_LOG_SIZE);
    dt_region(VS_OBJ, 0x3000u);
    dt_region(PX_DATA, 0x40u);
    dt_region(TW_RAY, 0x200u);
    dt_region(TW_NODES, TW_MAXN * 0x24u);
    dt_region(TW_DATA, 0x2000u);
    dt_region(TW_LOG, 0x1000u);
    dt_region(QW_RAY, QW_DATA + 0x2000u - QW_RAY); /* the ray, the nodes and the data in one (DT_MAX_REGIONS) */

    test_crt();
    test_depth(6000);
    test_calc_extents(30000);
    test_vector_scales(30000);
    test_physx_leaves(30000);
    test_tree_walk(20000);
    test_qtree_walk(20000);
    printf("CalcExtents paths: invalid box %u, transformed %u, a particle merged %u\n", ce_paths[0], ce_paths[1],
           ce_paths[3]);
    printf("depth sort cases: %u up to 8 particles, %u more\n", depth_cases[0], depth_cases[1]);
    if (getenv("G3N_BENCH")) {
        bench();
        bench_calc_extents();
        bench_walk();
    }
    return dt_report("pdo");
}
