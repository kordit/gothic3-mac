/* native/tests/rand_test.c - native rand/srand/GetRandom against the translated SharedBase code.
 *
 * Build and run (from the checkout root):
 *   sh native/tests/build.sh rand aux-sharedbase/chunk_1006c000.c aux-sharedbase/chunk_10030000.c
 *
 * The originals run in full, _getptd and _getptd_noexit included: their import calls
 * (GetLastError, TlsGetValue, SetLastError, and the CRT's decoded "FlsGetValue", which under the kit
 * is TlsGetValue) are modelled below the way the kit's shims behave, the TLS array at TEB + 0x1000.
 * Not covered: a thread whose slot is still 0 (the original then allocates a ptd through the CRT's
 * calloc; the native code calls the same original for it). */
#include "difftest_rp.h"

void fn_1006eb6d(X86 *c);
void fn_1006eb60(X86 *c);
void fn_100323a0(X86 *c);
void g3n_1006eb6d(X86 *c);
void g3n_1006eb60(X86 *c);
void g3n_100323a0(X86 *c);
void fn_1006ffe8(X86 *c);
void fn_1006ff71(X86 *c);
void g3n_1006ffe8(X86 *c);
void g3n_1006ff71(X86 *c);
extern int g3n_rand_ptd_direct;

#define FLSINDEX 5u
#define GETVALUEINDEX 6u
#define PTD 0x02000000u
#define PTD_FLS 0x02001000u /* what a real FLS would hold, for the mismatch case */
#define DATA 0x03000000u
#define ESP0 0x0e800000u
#define RET_SITE 0x10032415u

#define SHIM_GETLASTERROR 0x0ff00000u
#define SHIM_SETLASTERROR 0x0ff00010u
#define SHIM_TLSGETVALUE 0x0ff00020u
#define SHIM_FLSGETVALUE 0x0ff00030u /* a separate FLS store (mismatch case) */

static uint32_t last_error = 0x1234;
static uint32_t shim_calls;

static uint32_t arg(X86 *c, int i) {
    return rd32(c->r[4] + 4u + 4u * (uint32_t)i);
}
static void ret_stdcall(X86 *c, int nargs, uint32_t eax) {
    c->r[4] += 4u + 4u * (uint32_t)nargs;
    c->r[0] = eax;
}

void recomp_call_ic(X86 *c, uint32_t target, uint64_t *ic) {
    (void)ic;
    ++shim_calls;
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
    case SHIM_FLSGETVALUE:
        ret_stdcall(c, 1, arg(c, 0) == FLSINDEX ? PTD_FLS : 0);
        return;
    }
    fprintf(stderr, "unexpected indirect call to %08x\n", target);
    abort();
}

static void setup_crt(uint32_t flsget) {
    wr32(0x100891a0u, SHIM_GETLASTERROR);
    wr32(0x100891a4u, SHIM_SETLASTERROR);
    wr32(0x100891d8u, SHIM_TLSGETVALUE);
    wr32(0x100d1158u, FLSINDEX);
    wr32(0x100d115cu, GETVALUEINDEX);
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
        dt_run("rand", &c, fn_1006eb6d, g3n_1006eb6d);
        /* the sequence goes on from the native state: run both again from there */
        call_frame(&c, 0, NULL);
        dt_run("rand again", &c, fn_1006eb6d, g3n_1006eb6d);
    }
}

static void test_srand(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        const uint32_t seed = dt_rand32();
        dt_context(&c, ESP0, 0);
        call_frame(&c, 1, &seed);
        dt_run("srand", &c, fn_1006eb60, g3n_1006eb60);
    }
}

/* __getptd and _getptd_noexit called directly, as the rest of the CRT calls them. */
static void test_getptd(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        dt_context(&c, ESP0, dt_rand32() % 4);
        call_frame(&c, 0, NULL);
        if (i & 1)
            dt_run("_getptd_noexit", &c, fn_1006ff71, g3n_1006ff71);
        else
            dt_run("__getptd", &c, fn_1006ffe8, g3n_1006ffe8);
    }
}

static void test_getrandom(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        const uint32_t self = DATA + 4u * (dt_rand32() % 4);
        uint32_t out = DATA + 0x40u + 4u * (dt_rand32() % 4);
        if (dt_rand32() % 8 == 0)
            out = self + 4u * (dt_rand32() % 4); /* out overlapping the range */
        float v[6];
        for (int k = 0; k < 6; ++k)
            v[k] = dt_float();
        if (dt_rand32() % 3 == 0) /* min == max, or nearly */
            for (int k = 0; k < 3; ++k)
                v[k + 3] = dt_rand32() % 5 ? v[k] : (v[k] == 0.0f ? -v[k] : dt_float());
        for (int k = 0; k < 6; ++k)
            wrf32(self + 4u * k, v[k]);
        wr32(PTD + 0x14u, dt_rand32());
        dt_context(&c, ESP0, dt_rand32() % 4);
        c.r[1] = self;
        call_frame(&c, 1, &out);
        dt_run("GetRandom", &c, fn_100323a0, g3n_100323a0);
    }
}

int main(void) {
    harness_mem();
    dt_load_pe("SharedBase.dll");
    dt_stack_floor = ESP0 - 0x200u;
    dt_region(dt_stack_floor, 0x200u);
    dt_region(PTD, 0x2000u);
    dt_region(DATA, 0x100u);
    dt_region(GUEST_TEB_BASE, 0x1100u);
    dt_region(0x10089000u, 0x1000u);
    dt_region(0x100d0000u, 0x5000u);

    /* The kit's arrangement: the CRT's FlsGetValue is TlsGetValue. */
    setup_crt(SHIM_TLSGETVALUE);
    g3n_rand_ptd_direct = 0;
    test_rand(2000);
    if (g3n_rand_ptd_direct != 1) {
        fprintf(stderr, "FAIL: direct TLS read not enabled (%d)\n", g3n_rand_ptd_direct);
        ++dt_fail;
    }
    shim_calls = 0;
    {
        X86 c;
        dt_context(&c, ESP0, 0);
        call_frame(&c, 0, NULL);
        g3n_1006eb6d(&c);
        if (shim_calls != 0) {
            fprintf(stderr, "FAIL: native rand made %u shim calls\n", shim_calls);
            ++dt_fail;
        }
    }
    test_srand(2000);
    test_getrandom(20000);

    /* The CRT entries on their own: the first one checks the read, then no shim calls. */
    g3n_rand_ptd_direct = 0;
    test_getptd(2000);
    if (g3n_rand_ptd_direct != 1) {
        fprintf(stderr, "FAIL: direct TLS read not enabled by __getptd (%d)\n", g3n_rand_ptd_direct);
        ++dt_fail;
    }
    shim_calls = 0;
    {
        X86 c;
        dt_context(&c, ESP0, 0);
        call_frame(&c, 0, NULL);
        g3n_1006ffe8(&c);
        call_frame(&c, 0, NULL);
        g3n_1006ff71(&c);
        if (shim_calls != 0 || c.r[0] != PTD) {
            fprintf(stderr, "FAIL: native __getptd/_getptd_noexit made %u shim calls, gave %08x\n", shim_calls,
                    c.r[0]);
            ++dt_fail;
        }
    }

    /* An FLS that is not the TLS slot: the check must turn the direct read off, and every result
     * must still be the original's. */
    setup_crt(SHIM_FLSGETVALUE);
    g3n_rand_ptd_direct = 0;
    test_rand(500);
    test_srand(200);
    test_getrandom(2000);
    test_getptd(500);
    if (g3n_rand_ptd_direct != -1) {
        fprintf(stderr, "FAIL: mismatch not detected (%d)\n", g3n_rand_ptd_direct);
        ++dt_fail;
    }
    /* The same, with __getptd the first to look. */
    g3n_rand_ptd_direct = 0;
    test_getptd(500);
    test_rand(200);
    if (g3n_rand_ptd_direct != -1) {
        fprintf(stderr, "FAIL: mismatch not detected by __getptd (%d)\n", g3n_rand_ptd_direct);
        ++dt_fail;
    }
    return dt_report("rand_test");
}
