/* native/tests/uv_test.c - native/uv.c (group uv) against the translated SharedBase code.
 *
 * Build and run (from the checkout root):
 *   NATIVE="uv.c leaf.c rand.c" sh native/tests/build.sh uv aux-sharedbase/chunk_10010000.c \
 *     aux-sharedbase/chunk_10064000.c aux-sharedbase/chunk_10084000.c aux-sharedbase/chunk_10074000.c \
 *     aux-sharedbase/chunk_10070000.c aux-sharedbase/chunk_1006c000.c
 * (leaf.c for the native Normalize GetNormalized calls; the CRT chunks and rand.c for what the translated originals
 * reach through them). G3N_BENCH=1 adds timings.
 *
 * Each leaf from a random context (difftest_rp.h: scratch registers, x87 top with live values,
 * stale tags, precision and rounding control, XMM), on ordinary values, signed zeros, NaN,
 * infinities, denormals and any bit pattern, with outputs that sometimes overlap the inputs; the
 * registers, x87 state, XMM and memory are compared bitwise (the stack below the final ESP
 * excepted). */
#include "difftest_rp.h"

#include <math.h>
#include <time.h>

void fn_100103b0(X86 *c);
void g3n_100103b0(X86 *c);
void fn_10010570(X86 *c);
void g3n_10010570(X86 *c);
void fn_10011140(X86 *c);
void g3n_10011140(X86 *c);
void fn_10011080(X86 *c);
void g3n_10011080(X86 *c);
void fn_10011040(X86 *c);
void g3n_10011040(X86 *c);

#define ESP0 0x0e800000u
#define STACK_LO (ESP0 - 0x1000u)
#define RET_SITE 0x103b4000u
#define DATA 0x03000000u

/* Normalize (under GetNormalized) calls Scale(float) through Engine's import slot, and the CRT's
 * sqrt error path (a NaN square) sets errno through _errno -> _getptd_noexit, whose imports are
 * modelled as in leaf_test.c: the kit's TLS array at TEB + 0x1000, FlsGetValue = TlsGetValue. */
#define FLSINDEX 5u
#define GETVALUEINDEX 6u
#define PTD 0x02000000u
#define SHIM_GETLASTERROR 0x0ff00000u
#define SHIM_SETLASTERROR 0x0ff00010u
#define SHIM_TLSGETVALUE 0x0ff00020u
#define SCALE_IAT 0x105f6684u
#define SCALE_FN 0x100103b0u
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
        g3n_100103b0(c); /* the translated one while the groups are off */
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

static void call_frame(X86 *c, unsigned nargs, const uint32_t *args) {
    for (unsigned i = nargs; i-- > 0;) {
        c->r[4] -= 4;
        wr32(c->r[4], args[i]);
    }
    c->r[4] -= 4;
    wr32(c->r[4], RET_SITE);
}

static float uv_float(void) {
    return dt_rand32() % 4 ? (float)((int32_t)(dt_rand32() % 200001u) - 100000) / 128.0f : dt_float();
}

static uint32_t vec_at(void) {
    return DATA + 4u * (dt_rand32() % 12);
}

static unsigned per[5];

static void test_leaves(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        for (uint32_t a = DATA; a < DATA + 0x40u; a += 4)
            wrf32(a, uv_float());
        const uint32_t self = vec_at(), a = vec_at(), b = vec_at();
        const uint32_t out = dt_rand32() % 3 ? DATA + 0x30u : vec_at();
        dt_context(&c, ESP0, dt_rand32() % 4);
        c.r[1] = self;
        const unsigned k = dt_rand32() % 5;
        ++per[k];
        switch (k) {
        case 0: {
            const float s = uv_float();
            uint32_t sb;
            memcpy(&sb, &s, 4);
            call_frame(&c, 1, &sb);
            dt_run("Scale(float)", &c, fn_100103b0, g3n_100103b0);
            break;
        }
        case 1: {
            const uint32_t args[2] = {a, b};
            call_frame(&c, 2, args);
            dt_run("SetCrossProduct", &c, fn_10010570, g3n_10010570);
            break;
        }
        case 2: {
            const uint32_t args[2] = {out, a};
            call_frame(&c, 2, args);
            dt_run("GetTranslated", &c, fn_10011140, g3n_10011140);
            break;
        }
        case 3: {
            call_frame(&c, 1, &out);
            dt_run("GetNormalized", &c, fn_10011080, g3n_10011080);
            break;
        }
        case 4: {
            call_frame(&c, 1, &out);
            dt_run("operator-", &c, fn_10011040, g3n_10011040);
            break;
        }
        }
    }
}

int main(void) {
    harness_mem();
    dt_load_pe("SharedBase.dll");
    dt_stack_floor = STACK_LO;
    dt_region(STACK_LO, 0x1100u);
    dt_region(DATA, 0x100u);
    dt_region(PTD, 0x1000u);
    dt_region(GUEST_TEB_BASE, 0x1100u);
    dt_region(0x100d0000u, 0x5000u);
    setup_crt();
    test_leaves(100000);
    printf("cases: Scale %u, SetCrossProduct %u, GetTranslated %u, GetNormalized %u, operator- %u\n", per[0], per[1],
           per[2], per[3], per[4]);
    return dt_report("uv");
}
