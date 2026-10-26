/* native/tests/anim_test.c - native/anim.c against the translation.
 *
 * Build and run (from the checkout root):
 *   NATIVE=anim.c sh native/tests/build.sh anim aux-engine/chunk_10548000.c aux-engine/chunk_10558000.c \
 *       aux-engine/chunk_10540000.c
 * G3N_BENCH=1 adds timings.
 *
 * Each case runs the original (all groups off) and the native (all on) from the same state: random
 * registers, x87 top with live caller values, stale tags, rounding and precision control words,
 * XMM (difftest_rp.h), and inputs that are mostly unit quaternions and positions in a game's
 * range, now and then NaN, infinities, denormals, signed zeros, quaternions on opposite
 * hemispheres and blend factors 0 and 1. Compared bitwise: registers, x87 state, XMM and memory
 * (the stack above the final ESP). */
#include "difftest_rp.h"

#include <math.h>
#include <time.h>

void fn_1054a020(X86 *c);
void g3n_1054a020(X86 *c);
void fn_1055bce0(X86 *c);
void g3n_1055bce0(X86 *c);

#define ESP0 0x0e800000u
static unsigned live_at_start, top_at_start; /* the caller's x87 values in the case's context */
#define DATA 0x03000000u /* inputs and outputs */
#define DATA_LEN 0x2000u
#define TRACKS 0x03001000u /* 0x3c-byte tracks */

static float qcomp(void) {
    if (dt_rand32() % 24u == 0)
        return dt_float();
    return (float)(int32_t)dt_rand32() / 2147483648.0f;
}

static void put_quat(uint32_t a) {
    float q[4], n = 0;
    for (int i = 0; i < 4; ++i) {
        q[i] = qcomp();
        n += q[i] * q[i];
    }
    const int unit = dt_rand32() % 4u != 0 && n > 1e-6f && n == n;
    for (int i = 0; i < 4; ++i)
        wrf32(a + 4u * i, unit ? q[i] / sqrtf(n) : q[i]);
}

static float blend(void) {
    switch (dt_rand32() % 8u) {
    case 0: return 0.0f;
    case 1: return 1.0f;
    case 2: return dt_float();
    default: return (float)(dt_rand32() >> 8) / 16777216.0f;
    }
}

static void fill(uint32_t a, uint32_t n) {
    for (uint32_t k = 0; k < n; k += 4)
        wr32(a + k, dt_rand32());
}

static void ctx(X86 *c, const uint32_t *args, unsigned n, uint32_t ret) {
    live_at_start = dt_rand32() % 7u;
    dt_context(c, ESP0, live_at_start);
    top_at_start = c->fpu_top;
    for (unsigned i = n; i-- > 0;) {
        c->r[4] -= 4u;
        wr32(c->r[4], args[i]);
    }
    c->r[4] -= 4u;
    wr32(c->r[4], ret);
}

static unsigned arc[2];

static void test_blend(unsigned cases) {
    X86 c;
    for (unsigned i = 0; i < cases; ++i) {
        fill(DATA, 0x100u);
        const uint32_t q0 = DATA, q1 = DATA + 0x20u;
        uint32_t out = DATA + 0x40u;
        put_quat(q0);
        put_quat(q1);
        if (dt_rand32() % 4u == 0) /* the opposite hemisphere: -q0 */
            for (uint32_t k = 0; k < 16u; k += 4u)
                wrf32(q1 + k, -rdf32(q0 + k));
        if (dt_rand32() % 16u == 0) /* the output over an input */
            out = dt_rand32() & 1 ? q0 : q1;
        float t = blend();
        uint32_t tb;
        memcpy(&tb, &t, 4);
        const uint32_t a[] = {q1, tb};
        ctx(&c, a, 2, 0x1055b8a0u);
        c.r[0] = q0;
        c.r[2] = out;
        float d = 0;
        for (uint32_t k = 0; k < 16u; k += 4u)
            d += rdf32(q0 + k) * rdf32(q1 + k);
        if (dt_run("FUN_1055bce0", &c, fn_1055bce0, g3n_1055bce0))
            arc[d < 0.0f]++;
    }
}

static unsigned pose_paths[2];

static void test_pose(unsigned cases) {
    X86 c;
    for (unsigned i = 0; i < cases; ++i) {
        fill(DATA, 0x100u);
        fill(TRACKS, 0x3c0u);
        const uint32_t self = DATA + 0x100u;
        wr32(self + 0x6cu, TRACKS);
        const uint32_t k = dt_rand32() % 16u;
        const int has = dt_rand32() % 4u != 0;
        wr32(TRACKS + 0x3cu * k + 4u, has ? dt_rand32() & 0x7fffffffu : dt_rand32() | 0x80000000u);
        for (uint32_t j = 0; j < 40u; j += 4u)
            wrf32(TRACKS + 0x3cu * k + 0x14u + j, dt_float());
        const uint32_t out = dt_rand32() % 16u ? DATA + 0x40u : TRACKS + 0x3cu * k + 0x18u; /* overlap now and then */
        const uint32_t a[] = {k, out};
        ctx(&c, a, 2, 0x1055b680u);
        c.r[1] = self;
        if (dt_run("FUN_1054a020", &c, fn_1054a020, g3n_1054a020))
            pose_paths[(c.r[0] & 0xffu) != 0]++;
    }
}

void fn_1054bf00(X86 *c);
void g3n_1054bf00(X86 *c);
void fn_1054b510(X86 *c);
void g3n_1054b510(X86 *c);

static unsigned arc2[2];

static void test_blend_inplace(unsigned cases) {
    X86 c;
    for (unsigned i = 0; i < cases; ++i) {
        fill(DATA, 0x100u);
        const uint32_t q0 = DATA, q1 = dt_rand32() % 16u ? DATA + 0x20u : DATA + 4u; /* overlapping now and then */
        put_quat(q0);
        put_quat(q1);
        if (dt_rand32() % 4u == 0 && q1 == DATA + 0x20u)
            for (uint32_t k = 0; k < 16u; k += 4u)
                wrf32(q1 + k, -rdf32(q0 + k));
        float t = blend();
        uint32_t tb;
        memcpy(&tb, &t, 4);
        const uint32_t a[] = {tb};
        ctx(&c, a, 1, 0x1054b5d0u);
        c.r[1] = q0;
        c.r[2] = q1;
        float d = 0;
        for (uint32_t k = 0; k < 16u; k += 4u)
            d += rdf32(q0 + k) * rdf32(q1 + k);
        if (dt_run("FUN_1054bf00", &c, fn_1054bf00, g3n_1054bf00))
            arc2[d < 0.0f]++;
    }
}

static void test_keys(unsigned cases) {
    X86 c;
    for (unsigned i = 0; i < cases; ++i) {
        fill(DATA, 0x400u);
        const uint32_t self = DATA + 0x300u, keys = DATA + 0x100u;
        wr32(self + 0xcu, keys);
        const uint32_t k = dt_rand32() % 16u;
        if (dt_rand32() % 2u) /* plausible packed unit quaternions */
            for (uint32_t j = 0; j < 8u; ++j)
                wr16(keys + 12u * k + (j < 4u ? 2u * j : 0xcu + 2u * (j - 4u)),
                     (uint16_t)(int16_t)((int32_t)(dt_rand32() % 65535u) - 32767));
        double t = (double)blend();
        if (dt_rand32() % 8u == 0)
            t = (double)dt_rand32() / 4294967296.0 * 1.000001; /* rounds to float per RC */
        uint32_t tw[2];
        memcpy(tw, &t, 8);
        const uint32_t out = DATA + 0x40u;
        const uint32_t a[] = {k, tw[0], tw[1], out};
        ctx(&c, a, 4, 0x1054a6f0u);
        c.r[1] = self;
        dt_run("FUN_1054b510", &c, fn_1054b510, g3n_1054b510);
    }
}

void fn_10540e00(X86 *c);
void g3n_10540e00(X86 *c);

static void test_box(unsigned cases) {
    X86 c;
    for (unsigned i = 0; i < cases; ++i) {
        fill(DATA, 0x100u);
        const uint32_t box = DATA, p = dt_rand32() % 16u ? DATA + 0x40u : DATA + 8u;
        for (uint32_t k = 0; k < 24u; k += 4u)
            wrf32(box + k, dt_rand32() % 8u ? (float)(int32_t)(dt_rand32() % 200u) - 100.0f : dt_float());
        for (uint32_t k = 0; k < 12u; k += 4u)
            if (p != DATA + 8u)
                wrf32(p + k, dt_rand32() % 4u ? rdf32(box + k + 0xcu * (dt_rand32() & 1u)) : dt_float());
        ctx(&c, NULL, 0, 0x1054a7a0u);
        c.r[1] = box;
        c.r[2] = p;
        dt_run("FUN_10540e00", &c, fn_10540e00, g3n_10540e00);
    }
}

/* ---- FUN_1054a580: the track's own blend (vtable +0xc) is a fake that logs what it gets ---- */

void fn_1054a580(X86 *c);
void g3n_1054a580(X86 *c);

#define TRK 0x03001800u  /* the track: +0 vtable, +4 key count, +0xc keys */
#define TVT 0x03001900u
#define KEYS 0x03001a00u /* 12-byte keys, up to 40 */
#define IDX 0x03001c00u  /* the cached key index */
#define ALOG 0x03001d00u /* the fake's log: 16 words */
#define F_BLEND 0x0fd00300u
static uint32_t blend_calls;

static uint32_t amix(uint32_t h, uint32_t v) {
    h ^= v + 0x7f4a7c15u + (h << 6) + (h >> 2);
    h *= 0x85ebca6bu;
    return h ^ (h >> 15);
}

/* The tags of the slots the function itself has pushed by the call, from TOP up to where the
 * case started. Elsewhere a slot's tag says only whether a push wrapped the stack over it (into the
 * caller's values, too: the context has up to six), which the translation's x87 locals never do
 * (difftest_rp.h); MSVC code calls with an empty stack. */
static uint16_t live_tags(const X86 *c) {
    uint16_t t = 0xffffu;
    unsigned n = (top_at_start - c->fpu_top) & 7u;
    for (unsigned i = 0; i < n; ++i) {
        unsigned p = (c->fpu_top + i) & 7u;
        t = (uint16_t)((t & ~(3u << (2 * p))) | (ftag_of(c, p) << (2 * p)));
    }
    return t;
}

void recomp_call_ic(X86 *c, uint32_t target, uint64_t *ic) {
    (void)ic;
    if (target != F_BLEND) {
        fprintf(stderr, "unexpected indirect call to %08x\n", target);
        abort();
    }
    ++blend_calls;
    const uint32_t esp = c->r[4];
    const uint32_t w[16] = {rd32(esp), esp, c->r[0], c->r[1], c->r[2], c->r[3], c->r[5], c->r[6], c->r[7],
                            rd32(esp + 4u), rd32(esp + 8u), rd32(esp + 12u), rd32(esp + 16u),
                            c->fpu_top | ((uint32_t)live_tags(c) << 8), c->fpu_sw, rd32(esp + 20u)};
    uint32_t h = 0x1234u;
    for (unsigned i = 0; i < 16; ++i) {
        wr32(ALOG + 4u * i, w[i]);
        h = amix(h, w[i]);
    }
    c->r[0] = amix(h, 1);
    c->r[1] = amix(h, 2);
    c->r[2] = amix(h, 3);
    c->eip = rd32(esp);
    c->r[4] = esp + 4u + 0x10u;
}
void recomp_call(X86 *c, uint32_t target) {
    recomp_call_ic(c, target, NULL);
}

static unsigned trk_paths[4];

static void test_track(unsigned cases) {
    X86 c;
    for (unsigned i = 0; i < cases; ++i) {
        fill(DATA + 0x1800u, 0x600u);
        const uint32_t n = 1u + dt_rand32() % 30u;
        wr32(TRK, TVT);
        wr32(TVT + 0xcu, F_BLEND);
        wr32(TRK + 4u, n);
        wr32(TRK + 0xcu, KEYS);
        float tm = (float)(dt_rand32() % 4u);
        for (uint32_t k = 0; k < n + 1u; ++k) {
            wrf32(KEYS + 12u * k + 8u, dt_rand32() % 64u ? tm : dt_float());
            tm += dt_rand32() % 6u ? (float)(dt_rand32() % 100u) / 32.0f : 0.0f;
        }
        int32_t idx;
        switch (dt_rand32() % 6u) {
        case 0: idx = -1 - (int32_t)(dt_rand32() % 2u); break;
        case 1: idx = (int32_t)n + (int32_t)(dt_rand32() % 2u); break;
        default: idx = (int32_t)(dt_rand32() % n); break;
        }
        wr32(IDX, (uint32_t)idx);
        float t;
        const uint32_t pick = dt_rand32() % 8u;
        if (pick < 4u)
            t = rdf32(KEYS + 12u * (dt_rand32() % (n + 1u)) + 8u); /* exactly a key's time */
        else if (pick < 7u)
            t = (float)(dt_rand32() % 4000u) / 32.0f - 4.0f;
        else
            t = dt_float();
        uint32_t tb;
        memcpy(&tb, &t, 4);
        const uint32_t out = DATA + 0x40u;
        const uint32_t a[] = {tb, out};
        ctx(&c, a, 2, 0x1054a0f0u);
        c.r[0] = IDX;
        c.r[6] = TRK;
        if (dt_run("FUN_1054a580", &c, fn_1054a580, g3n_1054a580)) {
            const int32_t e = (int32_t)rd32(IDX);
            trk_paths[e < 0 ? 0 : e + 1 >= (int32_t)n ? 1 : idx < 0 || idx >= (int32_t)n ? 2 : 3]++;
        }
    }
}

int main(void) {
    harness_mem();
    dt_load_pe("Engine.dll");
    dt_stack_floor = ESP0 - 0x800u;
    dt_region(dt_stack_floor, 0x900u);
    dt_region(DATA, DATA_LEN);

    test_blend(300000);
    test_pose(100000);
    test_blend_inplace(200000);
    test_keys(200000);
    test_box(100000);
    test_track(200000);
    printf("FUN_1054a580: before the first key %u, past the last %u, searched anew %u, cached %u\n", trk_paths[0],
           trk_paths[1], trk_paths[2], trk_paths[3]);
    printf("FUN_1054bf00: same hemisphere %u, opposite %u\n", arc2[0], arc2[1]);
    printf("FUN_1055bce0: same hemisphere %u, opposite %u\n", arc[0], arc[1]);
    printf("FUN_1054a020: no pose %u, pose %u\n", pose_paths[0], pose_paths[1]);
    if (!arc[0] || !arc[1] || !pose_paths[0] || !pose_paths[1]) {
        fprintf(stderr, "FAIL: a path never taken\n");
        ++dt_fail;
    }
    return dt_report("anim_test");
}
