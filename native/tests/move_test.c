/* native/tests/move_test.c - native/move.c against the translation.
 *
 * Build and run (from the checkout root):
 *   NATIVE=move.c sh native/tests/build.sh move aux-engine/chunk_10548000.c
 * G3N_BENCH=1 adds a timing.
 *
 * FUN_1054a160's callees are fakes that log what they were handed (return address, ESP, the
 * registers, their stack arguments) to a log in guest memory, which is compared: FUN_10295e20 (its
 * fast path as the real one runs it, logged only when it would make its object), _memset, and
 * the SharedBase memory admin reached through Engine's imports (GetInstance, Realloc from a bump
 * heap filled with noise, Free scribbling the block). Skeletons and motions are small, with ids
 * drawn from a few values so that tracks repeat, are taken already, or are missing; counts of 0
 * and negative ones too. Dead stack bytes below the final ESP are compared too. FUN_1054a3d0, which sizes the per-node array (Realloc when it grows),
 * runs for real. */
#include "difftest_rp.h"

#include <time.h>

void fn_1054a160(X86 *c);
void g3n_1054a160(X86 *c);

#define ESP0 0x0e800000u
#define THIS 0x03000000u
#define ACTOR 0x03000200u
#define MOTION 0x03000400u
#define NODES 0x03001000u  /* up to 16 * 400 */
#define TRACKS 0x03003000u /* up to 16 * 0x9c */
#define OUT 0x03004000u    /* up to 16 * 0x3c */
#define HEAP 0x03005000u   /* [HEAP] = next free */
#define LOG 0x03006000u
#define LOG_SIZE 0x1000u
#define OBJ 0x03007000u
#define INSTANCE 0x03007100u
#define COOKIE 0x10835674u
#define SINGLETON 0x1086d988u
#define RET_SITE 0x1054d4e5u

static uint32_t case_seed;
static unsigned paths[8];

static uint32_t mix(uint32_t h, uint32_t v) {
    h ^= v + 0x7f4a7c15u + (h << 6) + (h >> 2);
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    return h ^ (h >> 16);
}

static uint32_t log_call(X86 *c, uint32_t addr, unsigned nargs) {
    const uint32_t n = rd32(LOG);
    const uint32_t e = LOG + 4u + 48u * n;
    if (e + 48u > LOG + LOG_SIZE) {
        fprintf(stderr, "call log full\n");
        abort();
    }
    wr32(LOG, n + 1u);
    const uint32_t esp = c->r[4];
    uint32_t w[12] = {addr, rd32(esp), esp, c->r[0], c->r[1], c->r[2], c->r[3], c->r[5], c->r[6], c->r[7],
                      c->fpu_top | (uint32_t)c->fpu_tag << 16, 0};
    for (unsigned k = 0; k < nargs; ++k)
        w[11] = mix(w[11], rd32(esp + 4u + 4u * k));
    uint32_t h = mix(case_seed, n);
    for (unsigned i = 0; i < 12; ++i) {
        wr32(e + 4u * i, w[i]);
        h = mix(h, w[i]);
    }
    return h;
}

static void fake_ret(X86 *c, uint32_t h, uint32_t pop, int keep_cx) {
    if (!keep_cx)
        c->r[1] = mix(h, 1);
    c->r[2] = mix(h, 2);
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u + pop;
}

/* FUN_10295e20: the real fast path; the object's making logged */
void fn_10295e20(X86 *c) {
    const uint32_t esp = c->r[4];
    const uint32_t obj = rd32(SINGLETON);
    if (obj != 0u && rd32(obj + 8u) != 0u) {
        wr32(esp - 4u, c->r[6]);
        wr32(esp - 8u, c->r[7]);
        c->r[0] = rd32(obj + 8u);
        c->eip = rd32(esp);
        c->r[4] = esp + 4u;
        return;
    }
    const uint32_t h = log_call(c, 0x10295e20u, 0);
    wr32(SINGLETON, OBJ);
    wr32(OBJ + 8u, OBJ + 0x40u);
    c->r[0] = OBJ + 0x40u;
    fake_ret(c, h, 0, 0);
    ++paths[1];
}

/* _memset(dst, value, n), cdecl */
void fn_1056a710(X86 *c) {
    const uint32_t h = log_call(c, 0x1056a710u, 3);
    const uint32_t esp = c->r[4], d = rd32(esp + 4u), v = rd32(esp + 8u), n = rd32(esp + 12u);
    if (n < 0x1000u)
        for (uint32_t k = 0; k < n; ++k)
            wr8(d + k, (uint8_t)v);
    c->r[0] = d;
    fake_ret(c, h, 0, 0);
}

void fn_1000c800(X86 *c) { /* GetInstance, static, RET */
    const uint32_t h = log_call(c, 0x1000c800u, 0);
    c->r[0] = INSTANCE;
    fake_ret(c, h, 0, 0);
}
void fn_1000c9c0(X86 *c) { /* Realloc(ptr, size), thiscall, RET 8 */
    const uint32_t h = log_call(c, 0x1000c9c0u, 2);
    const uint32_t size = rd32(c->r[4] + 8u);
    const uint32_t p = rd32(HEAP);
    wr32(HEAP, p + ((size + 15u) & ~7u));
    for (uint32_t k = 0; k < size && k < 0x100u; ++k)
        wr8(p + k, (uint8_t)mix(h, k));
    c->r[0] = p;
    fake_ret(c, h, 8, 0);
    ++paths[2];
}
void fn_1000c9e0(X86 *c) { /* Free(ptr), thiscall, RET 4 */
    const uint32_t h = log_call(c, 0x1000c9e0u, 1);
    const uint32_t p = rd32(c->r[4] + 4u);
    if (p >= HEAP && p < HEAP + 0x1000u)
        wr32(p, h);
    c->r[0] = mix(h, 4);
    fake_ret(c, h, 4, 0);
}

void recomp_call_ic(X86 *c, uint32_t target, uint64_t *ic) {
    (void)ic;
    switch (target) {
    case 0x1000c800u: fn_1000c800(c); return;
    case 0x1000c9c0u: fn_1000c9c0(c); return;
    case 0x1000c9e0u: fn_1000c9e0(c); return;
    }
    fprintf(stderr, "unexpected indirect call to %08x\n", target);
    abort();
}

/* dt_run, plus the dead stack bytes below the final ESP (region 0): the frame's locals and the
 * pushes of the callees done in place are written as the original writes them. */
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
    g3_opt = 0xffffffffu;
    native(c);
    const unsigned before = dt_fail;
    dt_compare(what, &o, c);
    for (uint32_t k = 0; k < dt_regions[0].len; ++k) {
        const uint32_t a = dt_regions[0].base + k;
        if (a >= o.r[4])
            break;
        if (dt_regions[0].orig[k] != g_mem[a]) {
            if (dead_fail < 10)
                fprintf(stderr, "FAIL %s: dead stack byte %08x %02x/%02x\n", what, a, dt_regions[0].orig[k], g_mem[a]);
            ++dead_fail;
            if (dt_fail == before)
                ++dt_fail;
            break;
        }
    }
}

static void setup_case(X86 *c) {
    case_seed = dt_rand32();
    wr32(LOG, 0);
    wr32(HEAP, HEAP + 0x10u);
    for (uint32_t k = 0; k < 0x200u; k += 4)
        wr32(THIS + k, dt_rand32());
    wr32(THIS + 0x44u, ACTOR);
    wr32(THIS + 0x60u, MOTION);
    /* the per-node array FUN_1054a3d0 (real) sizes: count, capacity, entries */
    wr32(THIS + 0x64u, dt_rand32() % 17u);
    wr32(THIS + 0x68u, dt_rand32() % 17u);
    wr32(THIS + 0x6cu, OUT);
    for (uint32_t k = 0; k < 0x400u; k += 4)
        wr32(OUT + k, dt_rand32());
    int32_t nn = (int32_t)(dt_rand32() % 13u), nt = (int32_t)(dt_rand32() % 13u);
    if (dt_rand32() % 16 == 0)
        nn = -(int32_t)(dt_rand32() % 3u);
    if (dt_rand32() % 16 == 0)
        nt = -(int32_t)(dt_rand32() % 3u);
    wr32(ACTOR + 0x1b4u, (uint32_t)nn);
    wr32(ACTOR + 0x1bcu, NODES);
    wr32(MOTION + 0x1cu, (uint32_t)nt);
    wr32(MOTION + 0x24u, TRACKS);
    const uint32_t ids = 1u + dt_rand32() % 10u;
    for (uint32_t i = 0; i < 16; ++i) {
        for (uint32_t k = 0; k < 400u; k += 4)
            wr32(NODES + 400u * i + k, dt_rand32());
        wr32(NODES + 400u * i + 0x188u, 0x100u + dt_rand32() % ids);
    }
    for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t t = TRACKS + 0x9cu * i;
        for (uint32_t k = 0; k < 0x9cu; k += 4)
            wrf32(t + k, dt_float());
        wr32(t + 0x10u, 0x100u + dt_rand32() % ids);
        wr32(t + 0x18u, dt_rand32() % 3u ? dt_rand32() % 5u - 2u : dt_rand32());
        wr32(t + 0x38u, dt_rand32() % 5u - 2u);
        wr32(t + 0x58u, dt_rand32() % 5u - 2u);
    }
    wr32(COOKIE, dt_rand32());
    if (dt_rand32() % 8 == 0) {
        wr32(SINGLETON, 0);
    } else {
        wr32(SINGLETON, OBJ);
        wr32(OBJ + 8u, OBJ + 0x40u);
    }
    dt_context(c, ESP0 - 4u, dt_rand32() % 6u);
    wr32(c->fs_base, dt_rand32());
    wr32(ESP0 - 4u, RET_SITE);
    c->r[1] = THIS;
}

static void test_match(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        setup_case(&c);
        run("1054a160", &c, fn_1054a160, g3n_1054a160);
        uint32_t matched = 0;
        const int32_t nn = (int32_t)rd32(ACTOR + 0x1b4u);
        const uint32_t out = rd32(THIS + 0x6cu);
        for (int32_t k = 0; k < nn; ++k)
            matched += rd32(out + 0x3cu * (uint32_t)k + 4u) != 0xffffffffu;
        paths[matched ? 4 : 3]++;
    }
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

/* 60 nodes, 60 tracks (a G3 skeleton's size, ? not measured), all ids present, in reverse order */
static void bench(void) {
    double t[2];
    for (int v = 0; v < 2; ++v) {
        g3_opt = v ? 0xffffffffu : 0u;
        const int n = 20000;
        X86 tmpl;
        setup_case(&tmpl);
        const uint32_t nb = 60u, bn = 0x04000000u, bt = 0x04100000u;
        wr32(ACTOR + 0x1b4u, nb);
        wr32(ACTOR + 0x1bcu, bn);
        wr32(MOTION + 0x1cu, nb);
        wr32(MOTION + 0x24u, bt);
        wr32(THIS + 0x64u, nb);
        wr32(THIS + 0x68u, nb);
        wr32(THIS + 0x6cu, 0x04200000u);
        for (uint32_t i = 0; i < nb; ++i) {
            wr32(bn + 400u * i + 0x188u, 0x200u + i);
            wr32(bt + 0x9cu * (nb - 1u - i) + 0x10u, 0x200u + i);
        }
        wr32(SINGLETON, OBJ);
        wr32(OBJ + 8u, OBJ + 0x40u);
        const double t0 = now();
        for (int i = 0; i < n; ++i) {
            X86 c = tmpl;
            wr32(LOG, 0);
            wr32(HEAP, HEAP + 0x10u);
            wr32(ESP0 - 4u, RET_SITE);
            g3n_1054a160(&c);
        }
        t[v] = (now() - t0) * 1e9 / n;
    }
    printf("bench 1054a160 (60 nodes, 60 tracks, fakes log), ns per call: translation %.1f, native %.1f\n", t[0], t[1]);
}

int main(void) {
    harness_mem();
    dt_load_pe("Engine.dll");
    wr32(0x105f7230u, 0x1000c800u);
    wr32(0x105f71d4u, 0x1000c9c0u);
    wr32(0x105f64b0u, 0x1000c9e0u);
    dt_stack_floor = ESP0 - 0x400u;
    dt_region(dt_stack_floor, 0x400u);
    dt_region(THIS, 0x600u);
    dt_region(NODES, 16u * 400u);
    dt_region(TRACKS, 16u * 0x9cu);
    dt_region(OUT, 0x400u);
    dt_region(HEAP, 0x2000u);
    dt_region(LOG, LOG_SIZE);
    dt_region(OBJ, 0x200u);
    dt_region(COOKIE, 4u);
    dt_region(SINGLETON, 4u);
    dt_region(GUEST_TEB_BASE, 8u);
    test_match(200000);
    printf("paths: none matched %u, some %u; singleton made %u, Realloc %u\n", paths[3], paths[4], paths[1],
           paths[2]);
    if (getenv("G3N_BENCH"))
        bench();
    return dt_report("move_test");
}
