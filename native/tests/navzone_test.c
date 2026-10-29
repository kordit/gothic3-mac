/* native/tests/navzone_test.c - native/navzone.c (group navzone) against the translated Game.dll and SharedBase code.
 *
 * Build and run (from the checkout root):
 *   NATIVE="navzone.c uv.c leaf.c vector.c rand.c" sh native/tests/build.sh navzone aux-game/chunk_10a24000.c \
 *     aux-sharedbase/chunk_10010000.c aux-sharedbase/chunk_10018000.c aux-sharedbase/chunk_10030000.c \
 *     aux-sharedbase/chunk_10084000.c aux-sharedbase/chunk_10064000.c aux-sharedbase/chunk_10074000.c \
 *     aux-sharedbase/chunk_10070000.c aux-sharedbase/chunk_1006c000.c
 * (uv.c, leaf.c and vector.c for the SharedBase leaves the original reaches through their overrides, rand.c and the
 * CRT chunks for the CRT's error paths). G3N_BENCH=1 adds timings.
 *
 * Zones as the game has them (a ring of points in x and z around a rotated and moved frame) and the odd ones (no
 * points, one, repeated points, a point on the tested position, a projective or NaN matrix, huge values), positions
 * inside and outside, the game's control word 003f and others, a pending precision flag now and then. Compared
 * bitwise: AL, EBX, ESP, EBP, ESI, EDI, EIP, the x87 state, XMM2-7 and memory; EAX above AL, ECX, EDX and XMM0-1 are
 * the original's scratch values, which navzone.c does not reproduce and no caller reads. */
#include "difftest_rp.h"

#include <math.h>
#include <time.h>

void fn_10a27180(X86 *c);
void g3n_10a27180(X86 *c);
void fn_10019ed0(X86 *c);
void g3n_10011bd0(X86 *c);
void fn_10010c90(X86 *c);
void g3n_100103b0(X86 *c);
void fn_10011570(X86 *c);
void g3n_10010570(X86 *c);
void g3n_10010e20(X86 *c);
void fn_10010c50(X86 *c);
extern unsigned g3n_navzone_native, g3n_navzone_original;

#define ESP0 0x0e800000u
#define STACK_LO (ESP0 - 0x1000u)
#define RET_SITE 0x10a3c700u
#define ZONE 0x03000000u
#define POINTS 0x03000400u
#define POS 0x03000300u

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

/* Game.dll's import slots for SharedBase, as the loader fills them. */
static const uint32_t iat[][2] = {
    {0x10b5dc8cu, 0x10019ed0u}, {0x10b5de34u, 0x10011bd0u}, {0x10b5dd74u, 0x10010c90u}, {0x10b5de2cu, 0x100103b0u},
    {0x10b5d9a0u, 0x10011570u}, {0x10b5ddf8u, 0x10010570u}, {0x10b5de3cu, 0x10010e20u}, {0x10b5dcf0u, 0x10010c50u},
    {0x10b5de0cu, 0x1008b490u}, {0x10b5de20u, 0x1008b494u},
};

void recomp_call_ic(X86 *c, uint32_t target, uint64_t *ic) {
    (void)ic;
    switch (target) {
    case 0x10019ed0u:
        fn_10019ed0(c);
        return;
    case 0x10011bd0u:
        g3n_10011bd0(c);
        return;
    case 0x10010c90u:
        fn_10010c90(c);
        return;
    case 0x100103b0u:
        g3n_100103b0(c);
        return;
    case 0x10011570u:
        fn_10011570(c);
        return;
    case 0x10010570u:
        g3n_10010570(c);
        return;
    case 0x10010e20u:
        g3n_10010e20(c);
        return;
    case 0x10010c50u:
        fn_10010c50(c);
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

static void setup(void) {
    for (unsigned i = 0; i < sizeof iat / sizeof iat[0]; ++i)
        wr32(iat[i][0], iat[i][1]);
    wr32(0x100891a0u, SHIM_GETLASTERROR);
    wr32(0x100891a4u, SHIM_SETLASTERROR);
    wr32(0x100891d8u, SHIM_TLSGETVALUE);
    wr32(0x100d1158u, FLSINDEX);
    wr32(0x100d115cu, GETVALUEINDEX);
    wr32(GUEST_TEB_BASE + 0x1000u + 4u * FLSINDEX, PTD);
    wr32(GUEST_TEB_BASE + 0x1000u + 4u * GETVALUEINDEX, SHIM_TLSGETVALUE);
}

static float frand(float lo, float hi) {
    return lo + (hi - lo) * (float)(dt_rand32() & 0xffffffu) / (float)0x1000000;
}

/* A zone: its frame at ZONE+0x104 (rotation about y and a translation, now and then something else), count points
 * at POINTS, and the tested position at POS. */
static void make_case(void) {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float a = frand(-3.2f, 3.2f), r = frand(50.0f, 3000.0f);
    m[0] = cosf(a);
    m[2] = -sinf(a);
    m[8] = sinf(a);
    m[10] = cosf(a);
    m[12] = frand(-50000.0f, 50000.0f);
    m[13] = frand(-2000.0f, 2000.0f);
    m[14] = frand(-50000.0f, 50000.0f);
    const unsigned kind = dt_rand32() % 100;
    if (kind < 3)
        m[3] = frand(-0.01f, 0.01f); /* projective */
    else if (kind < 5)
        m[15] = 0.0f; /* projective, |w| under the limit */
    else if (kind < 7)
        m[(dt_rand32() % 16)] = dt_float(); /* anything, NaN and infinities too */
    else if (kind < 10)
        for (int i = 0; i < 16; ++i)
            m[i] = (dt_rand32() & 3) ? m[i] : frand(-2.0f, 2.0f);
    for (int i = 0; i < 16; ++i)
        wrf32(ZONE + 0x104u + 4u * (uint32_t)i, m[i]);

    const unsigned shape = dt_rand32() % 100;
    int32_t count = shape < 2 ? 0 : shape < 4 ? 1 : shape < 7 ? 2 : 3 + (int32_t)(dt_rand32() % 14);
    if (shape == 99)
        count = -1;
    wr32(ZONE + 0x14u, POINTS);
    wr32(ZONE + 0x18u, (uint32_t)count);
    float ang = frand(-3.2f, 3.2f);
    for (int32_t i = 0; i < (count > 0 ? count : 0); ++i) {
        ang += frand(0.05f, 1.2f) * ((dt_rand32() % 10) ? 1.0f : -1.0f);
        const float rr = r * frand(0.3f, 1.0f);
        float x = rr * cosf(ang), y = frand(-500.0f, 500.0f), z = rr * sinf(ang);
        if (i > 0 && dt_rand32() % 30 == 0) { /* a repeated point */
            x = rdf32(POINTS + 12u * (uint32_t)(i - 1));
            z = rdf32(POINTS + 12u * (uint32_t)(i - 1) + 8u);
        }
        if (dt_rand32() % 200 == 0)
            x = dt_float();
        if (dt_rand32() % 300 == 0)
            y = dt_float();
        wrf32(POINTS + 12u * (uint32_t)i, x);
        wrf32(POINTS + 12u * (uint32_t)i + 4u, y);
        wrf32(POINTS + 12u * (uint32_t)i + 8u, z);
    }
    /* the position: in world space, near the zone (inside about half the time) */
    const float pa = frand(-3.2f, 3.2f), pr = r * frand(0.0f, 1.6f);
    const float lx = pr * cosf(pa), lz = pr * sinf(pa);
    float px = m[12] + lx, py = frand(-3000.0f, 3000.0f), pz = m[14] + lz;
    if (count > 0 && dt_rand32() % 25 == 0) { /* exactly on a zone point, if the frame is the identity */
        for (int i = 0; i < 16; ++i)
            wrf32(ZONE + 0x104u + 4u * (uint32_t)i, (i % 5) ? 0.0f : 1.0f);
        px = rdf32(POINTS);
        pz = rdf32(POINTS + 8u);
    }
    if (dt_rand32() % 200 == 0)
        pz = dt_float();
    wrf32(POS, px);
    wrf32(POS + 4u, py);
    wrf32(POS + 8u, pz);
}

static const uint16_t cws[] = {0x003f, 0x003f, 0x003f, 0x003f, 0x027f, 0x037f, 0x007f, 0x0c3f, 0x013f};

static void frame(X86 *c, int fallback) {
    dt_context(c, ESP0, dt_rand32() % 3);
    c->fpu_cw = cws[dt_rand32() % (sizeof cws / sizeof cws[0])];
    if (dt_rand32() % 20 == 0)
        c->fpu_sw |= 0x20u;
    c->r[1] = ZONE;
    const uint32_t args[2] = {POS, (dt_rand32() & 0xffffff00u) | (uint32_t)fallback};
    for (int i = 1; i >= 0; --i) {
        c->r[4] -= 4;
        wr32(c->r[4], args[i]);
    }
    c->r[4] -= 4;
    wr32(c->r[4], RET_SITE);
}

static unsigned trues, falses;

static void test(unsigned n) {
    X86 c, start, o;
    for (unsigned i = 0; i < n; ++i) {
        make_case();
        frame(&c, (int)(dt_rand32() & 1));
        start = c;
        dt_save(0);
        g3_opt = 0;
        fn_10a27180(&c);
        o = c;
        dt_save(1);
        dt_restore();
        c = start;
        g3_opt = dt_native_opt;
        g3n_10a27180(&c);
        /* the original's scratch values, not reproduced (see the header) */
        c.r[0] = (o.r[0] & ~0xffu) | (c.r[0] & 0xffu);
        c.r[1] = o.r[1];
        c.r[2] = o.r[2];
        memcpy(c.xmm[0], o.xmm[0], sizeof c.xmm[0]);
        memcpy(c.xmm[1], o.xmm[1], sizeof c.xmm[1]);
        dt_compare("Test3DPointInZone", &o, &c);
        if (o.r[0] & 0xffu)
            ++trues;
        else
            ++falses;
    }
}

static void bench(void) {
    X86 c;
    make_case();
    for (int32_t k = 0; k < 16; ++k) { /* an ordinary zone of 10 points */
        const float a = 0.6f * (float)k;
        wrf32(POINTS + 12u * (uint32_t)k, 800.0f * cosf(a));
        wrf32(POINTS + 12u * (uint32_t)k + 4u, 0.0f);
        wrf32(POINTS + 12u * (uint32_t)k + 8u, 800.0f * sinf(a));
    }
    wr32(ZONE + 0x18u, 10u);
    for (int i = 0; i < 16; ++i)
        wrf32(ZONE + 0x104u + 4u * (uint32_t)i, (i % 5) ? 0.0f : 1.0f);
    wrf32(POS, 100.0f);
    wrf32(POS + 4u, 0.0f);
    wrf32(POS + 8u, 50.0f);
    frame(&c, 0);
    c.fpu_cw = 0x003f;
    c.fpu_sw = 0;
    const X86 base = c;
    for (int pass = 0; pass < 2; ++pass) {
        g3_opt = pass ? dt_native_opt : 0;
        const clock_t t0 = clock();
        const int loops = getenv("G3N_BENCH_N") ? atoi(getenv("G3N_BENCH_N")) : 200000;
        for (int i = 0; i < loops; ++i) {
            c.r[4] = base.r[4];
            c.r[1] = ZONE;
            c.fpu_sw = 0;
            if (pass)
                g3n_10a27180(&c);
            else
                fn_10a27180(&c);
        }
        printf("%s: %.2f us a call\n", pass ? "native" : "translated", (double)(clock() - t0) / CLOCKS_PER_SEC * 1e6 / loops);
    }
}

int main(void) {
    harness_mem();
    dt_load_pe("SharedBase.dll");
    dt_load_pe("Game.dll");
    dt_stack_floor = STACK_LO;
    dt_region(STACK_LO, 0x1100u);
    dt_region(ZONE, 0x1000u);
    dt_region(PTD, 0x1000u);
    dt_region(GUEST_TEB_BASE, 0x1100u);
    dt_region(0x100d0000u, 0x5000u);
    setup();
    test(200000);
    printf("results: %u true, %u false; native %u, the original instead %u\n", trues, falses, g3n_navzone_native,
           g3n_navzone_original);
    if (getenv("G3N_BENCH"))
        bench();
    return dt_report("navzone");
}
