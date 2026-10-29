/* native/tests/ao_test.c - native/ao.c (group ao) against the translated Engine and SharedBase code.
 *
 * Build and run (from the checkout root):
 *   NATIVE="ao.c pdo.c rand.c" sh native/tests/build.sh ao aux-engine/chunk_10358000.c aux-engine/chunk_1035c000.c \
 *       aux-engine/chunk_105a8000.c aux-sharedbase/chunk_10010000.c aux-sharedbase/chunk_10018000.c \
 *       aux-sharedbase/chunk_10030000.c aux-sharedbase/chunk_10084000.c aux-sharedbase/chunk_10074000.c \
 *       aux-sharedbase/chunk_10070000.c aux-sharedbase/chunk_1006c000.c aux-engine/chunk_105ac000.c \
 *       aux-engine/chunk_1056c000.c aux-engine/chunk_10564000.c aux-engine/chunk_10568000.c
 *
 * eCAOTreeNode::ComputeSphereOcclusionFactor (1035a0b0) and ComputeOcclusionFactor (10359e90) on random emitter trees:
 * up to four levels, nodes with and without content (flag 8), opened (eight children, some missing) or leaves (flag
 * 2) with up to twelve disk records reached through the node's index array, node radii around the "one disk"
 * threshold (radius * 8 against the square distance), zero and negative ones. The matrix is affine (the w column
 * exactly 0, 0, 0, 1), projective, projective with |w| under g_fEpsilon (the zero vector), now and then with a NaN.
 * The receiving disk faces the emitters mostly, its normal not always of length one; the harmonics start from random
 * values. A few cases put a NaN, an infinity or a negative zero into a position so that a square distance or a
 * square length the CRT's sqrt gets is not a plain one (native/ao.c then gives the call to the original); in a
 * normal that leaves the sphere walk's harmonics finite, so the undoing of what was already written shows. The x87
 * control word is each of difftest's. Each case runs the original (all groups off) and the native (all on) from the
 * same state and compares the registers, the x87 state, the harmonics, the disk counter and the stack from the final
 * ESP up. EAX, ECX, EDX and the XMM registers are cleared after both: the walks' callers (1046a420, 10359300, the
 * walks themselves) read none of them after the call. */
#include "difftest_rp.h"

void fn_1035a0b0(X86 *c);
void fn_10359e90(X86 *c);
void g3n_1035a0b0(X86 *c);
void g3n_10359e90(X86 *c);
extern unsigned g3n_ao_bails, g3n_ao_bails_late;
void fn_100138d0(X86 *c); /* SharedBase SetVector */
void fn_10010440(X86 *c); /* Transform */
void fn_10010480(X86 *c); /* TransformDirection */
void fn_10010060(X86 *c); /* Normalize */
void fn_1001ae70(X86 *c); /* InvTranslate */
void fn_10010e20(X86 *c); /* GetSquareMagnitude */
void fn_100103b0(X86 *c); /* Scale(float) */
void fn_1001b580(X86 *c); /* GetDotProduct */

#define ESP0 0x0e800000u
#define DATA 0x03000000u
#define DATA_LEN 0x00040000u
#define MAT (DATA + 0x0u)
#define DISK (DATA + 0x40u)
#define SH (DATA + 0x80u)
#define NODES (DATA + 0x1000u) /* 0x6c each */
#define RECS (DATA + 0x10000u) /* 0x20 each */
#define IDX (DATA + 0x20000u)
#define ARRS (DATA + 0x30000u) /* a leaf's array object: {data} */
#define COUNTER 0x10862db0u

/* Engine's CRT for the sqrt's error exits (errno through _getptd): the kit's TLS as pdo_test models it */
#define FLSINDEX 5u
#define GETVALUEINDEX 6u
#define PTD 0x02000000u
#define SHIM_GETLASTERROR 0x0ff00000u
#define SHIM_SETLASTERROR 0x0ff00010u
#define SHIM_TLSGETVALUE 0x0ff00020u
#define IAT_GETLASTERROR 0x105f631cu
#define IAT_SETLASTERROR 0x105f6274u
#define IAT_TLSGETVALUE 0x105f6264u
#define ENG_FLSINDEX 0x10835690u
#define ENG_GETVALUEINDEX 0x10835694u
static uint32_t last_error;
static uint32_t arg_at(X86 *c, unsigned i) {
    return rd32(c->r[4] + 4u + 4u * i);
}
static void ret_stdcall(X86 *c, unsigned n, uint32_t eax) {
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u + 4u * n;
    c->r[0] = eax;
}

void recomp_call_ic(X86 *c, uint32_t target, uint64_t *ic) {
    (void)ic;
    switch (target) {
    case 0x100138d0u: fn_100138d0(c); return;
    case 0x10010440u: fn_10010440(c); return;
    case 0x10010480u: fn_10010480(c); return;
    case 0x10010060u: fn_10010060(c); return;
    case 0x1001ae70u: fn_1001ae70(c); return;
    case 0x10010e20u: fn_10010e20(c); return;
    case 0x100103b0u: fn_100103b0(c); return;
    case 0x1001b580u: fn_1001b580(c); return;
    case SHIM_GETLASTERROR: ret_stdcall(c, 0, last_error); return;
    case SHIM_SETLASTERROR: last_error = arg_at(c, 0); ret_stdcall(c, 1, 0); return;
    case SHIM_TLSGETVALUE: {
        const uint32_t i = arg_at(c, 0);
        ret_stdcall(c, 1, i < 64 ? rd32(c->fs_base + 0x1000u + 4u * i) : 0);
        return;
    }
    }
    fprintf(stderr, "unexpected indirect call to %08x\n", target);
    abort();
}
void recomp_call(X86 *c, uint32_t target) {
    recomp_call_ic(c, target, NULL);
}

static float uni(float lo, float hi) {
    return lo + (hi - lo) * (float)(dt_rand32() >> 8) / 16777216.0f;
}
static void wv(uint32_t a, float x, float y, float z) {
    wrf32(a, x);
    wrf32(a + 4u, y);
    wrf32(a + 8u, z);
}
static int one_in(uint32_t n) {
    return dt_rand32() % n == 0;
}
/* A special value now and then: NaN, infinity, negative zero. */
static int no_spoil; /* the bench: plain values only */
static float spoil(float v) {
    if (no_spoil || !one_in(40))
        return v;
    switch (dt_rand32() % 3) {
    case 0: return dt_bits(0x7fc00000u);
    case 1: return dt_bits(0x7f800000u);
    default: return dt_bits(0x80000000u);
    }
}

static uint32_t nnodes, nrecs, nidx, narrs;
static unsigned leaves, opened, sh_changed, ie_set;
static uint64_t disks;

static void emitter(uint32_t at, float spread) {
    wv(at, spoil(uni(-spread, spread)), spoil(uni(0.2f, 2.0f * spread)), uni(-spread, spread));
    /* a special value in a normal makes Normalize's sqrt give the call back without poisoning the harmonics: the
     * sphere walk never reads an emitter's normal */
    float nx = spoil(uni(-1, 1)), ny = uni(-1, 1), nz = spoil(uni(-1, 1));
    wv(at + 0xcu, nx, ny, nz);
    wrf32(at + 0x18u, one_in(8) ? 0.0f : one_in(10) ? -uni(0, 1) : uni(0.01f, 3.0f));
    wrf32(at + 0x1cu, uni(0.01f, 9.0f));
}

static uint32_t make_node(int depth) {
    const uint32_t n = NODES + 0x6cu * nnodes++;
    for (uint32_t i = 0; i < 0x6cu; i += 4)
        wr32(n + i, 0);
    emitter(n, 6.0f);
    const int leaf = depth >= 3 || one_in(3);
    uint32_t flags = one_in(12) ? 0u : 8u;
    if (leaf) {
        flags |= 2u;
        ++leaves;
        const uint32_t count = dt_rand32() % 13u;
        const uint32_t arr = ARRS + 4u * narrs++;
        wr32(arr, RECS);
        wr32(n + 0x58u, arr);
        wr32(n + 0x5cu, IDX + 4u * nidx);
        wr32(n + 0x60u, count);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t r = nrecs++;
            emitter(RECS + 0x20u * r, 8.0f);
            wr32(IDX + 4u * nidx++, r);
        }
    } else {
        ++opened;
        for (uint32_t k = 0x38u; k <= 0x54u; k += 4u)
            if (!one_in(3) && nnodes < 200u)
                wr32(n + k, make_node(depth + 1));
    }
    wr32(n + 0x68u, flags | (dt_rand32() & 0xf0u));
    return n;
}

static void matrix(void) {
    for (int i = 0; i < 16; ++i)
        wrf32(MAT + 4u * (uint32_t)i, 0.0f);
    const float a = uni(0, 6.2831f), s = one_in(5) ? uni(0.2f, 3.0f) : 1.0f;
    wrf32(MAT + 0x00u, s * cosf(a));
    wrf32(MAT + 0x08u, -s * sinf(a));
    wrf32(MAT + 0x14u, s);
    wrf32(MAT + 0x20u, s * sinf(a));
    wrf32(MAT + 0x28u, s * cosf(a));
    wv(MAT + 0x30u, uni(-3, 3), uni(-1, 1), uni(-3, 3));
    wrf32(MAT + 0x3cu, 1.0f);
    switch (dt_rand32() % 10u) {
    case 0: /* projective */
        wrf32(MAT + 0x0cu, uni(-0.05f, 0.05f));
        wrf32(MAT + 0x1cu, uni(-0.05f, 0.05f));
        wrf32(MAT + 0x3cu, uni(0.5f, 2.0f));
        break;
    case 1: /* |w| under g_fEpsilon */
        wrf32(MAT + 0x3cu, 0.0f);
        wrf32(MAT + 0x2cu, one_in(2) ? 0.0f : 1e-30f);
        break;
    case 2:
        wrf32(MAT + 4u * (dt_rand32() % 16u), dt_bits(0x7fc00000u));
        break;
    default:
        break;
    }
}

static void orig_sphere(X86 *c) {
    fn_1035a0b0(c);
    c->r[0] = c->r[1] = c->r[2] = 0;
    memset(c->xmm, 0, sizeof c->xmm);
}
static void native_sphere(X86 *c) {
    g3n_1035a0b0(c);
    c->r[0] = c->r[1] = c->r[2] = 0;
    memset(c->xmm, 0, sizeof c->xmm);
}
static void orig_oriented(X86 *c) {
    fn_10359e90(c);
    c->r[0] = c->r[1] = c->r[2] = 0;
    memset(c->xmm, 0, sizeof c->xmm);
}
static void native_oriented(X86 *c) {
    g3n_10359e90(c);
    c->r[0] = c->r[1] = c->r[2] = 0;
    memset(c->xmm, 0, sizeof c->xmm);
}

static void one_case(int oriented) {
    for (uint32_t a = DATA; a < DATA + DATA_LEN; a += 4)
        wr32(a, 0);
    nnodes = nrecs = nidx = narrs = 0;
    const uint32_t root = make_node(0);
    matrix();
    wv(DISK, uni(-1, 1), spoil(uni(-1, 0.5f)), uni(-1, 1));
    const float nx = uni(-0.3f, 0.3f), ny = uni(0.6f, 1.2f), nz = uni(-0.3f, 0.3f);
    wv(DISK + 0xcu, nx, ny, nz);
    for (uint32_t i = 0; i < 9; ++i)
        wrf32(SH + 4u * i, uni(-0.5f, 0.5f));
    wr32(COUNTER, dt_rand32());
    X86 c;
    dt_context(&c, ESP0 - 0x10u, 0);
    wr32(c.r[4], 0x1046a457u);
    wr32(c.r[4] + 4u, DISK);
    wr32(c.r[4] + 8u, SH);
    wr32(c.r[4] + 0xcu, MAT);
    c.r[1] = root;
    uint32_t sh0[9];
    memcpy(sh0, g_mem + SH, sizeof sh0);
    const uint32_t count0 = rd32(COUNTER);
    const uint16_t sw0 = c.fpu_sw;
    dt_run(oriented ? "ComputeOcclusionFactor" : "ComputeSphereOcclusionFactor", &c,
           oriented ? orig_oriented : orig_sphere, oriented ? native_oriented : native_sphere);
    if (memcmp(sh0, g_mem + SH, sizeof sh0))
        ++sh_changed;
    disks += rd32(COUNTER) - count0;
    if ((c.fpu_sw & ~sw0) & 1u)
        ++ie_set;
}

#include <time.h>
static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
/* AO_BENCH=1: the same 300 trees through the original and the native, the time of each (G3N_OPT=2 as the game). */
static void bench(void) {
    double t[2] = {0, 0};
    no_spoil = 1;
    const unsigned bails0 = g3n_ao_bails;
    for (int i = 0; i < 300; ++i) {
        for (uint32_t a = DATA; a < DATA + DATA_LEN; a += 4)
            wr32(a, 0);
        nnodes = nrecs = nidx = narrs = 0;
        const uint32_t root = make_node(0);
        matrix();
        wrf32(MAT + 0x0cu, 0.0f), wrf32(MAT + 0x1cu, 0.0f), wrf32(MAT + 0x2cu, 0.0f), wrf32(MAT + 0x3cu, 1.0f);
        for (uint32_t k = 0; k < 16; ++k)
            if (rdf32(MAT + 4u * k) != rdf32(MAT + 4u * k))
                wrf32(MAT + 4u * k, 0.5f);
        wv(DISK, 0, -0.5f, 0);
        wv(DISK + 0xcu, 0, 1, 0);
        for (int pass = 0; pass < 2; ++pass) {
            X86 c;
            dt_context(&c, ESP0 - 0x10u, 0);
            c.fpu_cw = 0x007f;
            wr32(c.r[4], 0x1046a457u);
            wr32(c.r[4] + 4u, DISK);
            wr32(c.r[4] + 8u, SH);
            wr32(c.r[4] + 0xcu, MAT);
            c.r[1] = root;
            g3_opt = pass ? dt_native_opt : 0;
            const double t0 = now_ms();
            for (int r = 0; r < 20; ++r) {
                X86 k = c;
                (pass ? g3n_1035a0b0 : fn_1035a0b0)(&k);
            }
            t[pass] += now_ms() - t0;
        }
    }
    printf("bench: original %.1f ms, native %.1f ms (%.1fx), %u given back; %llu disks\n", t[0], t[1], t[0] / t[1],
           g3n_ao_bails - bails0, (unsigned long long)rd32(COUNTER));
}

int main(void) {
    harness_mem();
    dt_load_pe("Engine.dll");
    dt_load_pe("SharedBase.dll");
    wr32(0x105f7158u, 0x100138d0u); /* SetVector */
    wr32(0x105f6840u, 0x10010440u); /* Transform */
    wr32(0x105f684cu, 0x10010480u); /* TransformDirection */
    wr32(0x105f6904u, 0x10010060u); /* Normalize */
    wr32(0x105f66ecu, 0x1001ae70u); /* InvTranslate */
    wr32(0x105f677cu, 0x10010e20u); /* GetSquareMagnitude */
    wr32(0x105f6684u, 0x100103b0u); /* Scale(float) */
    wr32(0x105f6854u, 0x1001b580u); /* GetDotProduct */
    wr32(0x105f6864u, 0x1008b4b0u); /* g_fEpsilon */
    wr32(0x105f63fcu, 0x1008b490u); /* g_fPI */
    wr32(IAT_GETLASTERROR, SHIM_GETLASTERROR);
    wr32(IAT_SETLASTERROR, SHIM_SETLASTERROR);
    wr32(IAT_TLSGETVALUE, SHIM_TLSGETVALUE);
    wr32(ENG_FLSINDEX, FLSINDEX);
    wr32(ENG_GETVALUEINDEX, GETVALUEINDEX);
    wr32(GUEST_TEB_BASE + 0x1000u + 4u * FLSINDEX, PTD);
    wr32(GUEST_TEB_BASE + 0x1000u + 4u * GETVALUEINDEX, SHIM_TLSGETVALUE);
    wr32(0x100891a0u, SHIM_GETLASTERROR); /* SharedBase's CRT (Normalize's sqrt), the same TLS */
    wr32(0x100891a4u, SHIM_SETLASTERROR);
    wr32(0x100891d8u, SHIM_TLSGETVALUE);
    wr32(0x100d1158u, FLSINDEX);
    wr32(0x100d115cu, GETVALUEINDEX);
    /* pdo's and rand's native _getptd read the TLS directly; both runs take the translated CRT here */
    dt_native_opt = 0xffffffffu & ~(uint32_t)(G3OPT_PDO | G3OPT_RAND);
    dt_stack_floor = ESP0 - 0x8000u;
    dt_region(dt_stack_floor, 0x8100u);
    dt_region(DATA, DATA_LEN);
    dt_region(COUNTER, 4u);
    dt_region(PTD, 0x100u);
    dt_region(GUEST_TEB_BASE, 0x10u);
    if (getenv("AO_BENCH")) {
        bench();
        return 0;
    }
    for (int i = 0; i < 6000; ++i)
        one_case(i & 1);
    printf("nodes opened %u, leaves %u; cases with the harmonics changed %u, with IE raised %u; disks %llu; "
           "given to the original %u (%u after a harmonics write)\n",
           opened, leaves, sh_changed, ie_set, (unsigned long long)disks, g3n_ao_bails, g3n_ao_bails_late);
    return dt_report("ao_test");
}
