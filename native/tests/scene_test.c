/* native/tests/scene_test.c - native/scene.c against the translation.
 *
 * Build and run (from the checkout root):
 *   NATIVE="scene.c cull.c" sh native/tests/build.sh scene aux-engine/chunk_103fc000.c aux-engine/chunk_10494000.c
 * (cull.c because the override header sends ExtractFromEntity's CullVPTHC, UpdateNodeData and
 * CalculateLoDArea calls to its g3n_; the test keeps the cull group off, so they reach the fakes)
 * (the first chunk holds the translated ExtractFromEntity 103ff970 and FilterEntityCullCallback
 * 103fd330, which runs for real, its callbacks being indirect calls answered here; the second
 * FUN_104956f0). G3N_BENCH=1 adds a timing of ExtractFromEntity.
 *
 * ExtractFromEntity's other callees are fakes defined below. Each fake appends a record of what it
 * was handed to a log in guest memory (target, return address, ESP, EAX..EDI, its stack arguments,
 * the word an argument points to, the x87 top, tag and status words), then answers from a hash of
 * the case seed, its position in the call sequence and those inputs: AL, a clobbered ECX/EDX/XMM0,
 * a float in ST0 for the two that return one, and writes into the item and entity fields
 * ExtractFromEntity reads after the call. The log and the call counter are compared memory, so a
 * call made in another order, with another argument, ESP or register, or one too many or too few,
 * fails the case, and a value the native kept in a register instead of reading back after a call
 * gives a different branch.
 *
 * Every case runs the original and the native from the same state (random registers, x87 top,
 * live caller values, stale tags, rounding control words, XMM; difftest_rp.h) and compares
 * registers, x87 state and memory bitwise. Inputs are biased so that every exit is taken: the
 * frame stamp, the enable bits and the two static masks, the callbacks, the cull status, the
 * small-object factor around epsilon (NaN, denormals, values that round differently under each
 * rounding control), the dynamic-query and occluder paths. */
#include "difftest_rp.h"

#include <time.h>

void fn_103ff970(X86 *c);
void g3n_103ff970(X86 *c);
void fn_104956f0(X86 *c);
void g3n_104956f0(X86 *c);

#define ESP0 0x0e800000u
#define DB 0x03000000u     /* eCFrustumDatabase, 0x300 bytes */
#define ENT 0x03001000u    /* eCEntity, 0x200 bytes; its frustum item at +0x14c */
#define ITEM (ENT + 0x14cu)
#define CBS 0x03002000u    /* the database's filter callback array */
#define EPS 0x03002100u    /* g_fEpsilon */
#define LOG 0x03010000u    /* the fakes' call log: count, then 16 words per call */
#define LOG_SIZE 0x1000u
#define CB0 0x0fd00000u    /* filter callbacks, stdcall (filter, entity) */
#define CB1 0x0fd00010u
#define RET_SITE 0x103fe56du /* RenderNode's continuation after CALL ExtractFromEntity */

/* FUN_104956f0's world */
#define RENDERER 0x03003000u /* +4 device state, +8 device wrapper */
#define DEVSTATE 0x03004000u /* +0x7c4 image per stage, +0x804 0x18 bytes of sampler states per stage */
#define WRAPPER 0x03005000u  /* +0xc the object with SetSamplerState, +0x48 the default value */
#define SSOBJ 0x03005100u
#define SSVTBL 0x03005200u
#define STAGE 0x03006000u    /* +0x28 the stage number */
#define WANT 0x03007000u     /* EDI: the six wanted states */
#define IMG 0x03008000u
#define SETSS 0x0fd00100u    /* [SSVTBL+0x1f8] */

#define GUARD 0x10874c0cu
#define REQUIRE 0x10874c08u
#define EXCLUDE 0x10874c04u
#define EPS_IAT 0x105f6864u

static uint32_t case_seed;
static int bench_fast; /* the fakes skip the log and hash only the seed and the call number */

/* per-case path counters: [exit kind] */
static unsigned paths[32];

static uint32_t mix(uint32_t h, uint32_t v) {
    h ^= v + 0x7f4a7c15u + (h << 6) + (h >> 2);
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    return h ^ (h >> 16);
}

static uint16_t fpu_tagword(const X86 *c) {
    return c->fpu_tag;
}

/* Logs the call and returns its hash. */
static uint32_t fake_enter(X86 *c, uint32_t addr) {
    const uint32_t n = rd32(LOG);
    if (bench_fast) {
        wr32(LOG, n + 1u);
        return mix(case_seed ^ addr, n);
    }
    const uint32_t e = LOG + 4u + 64u * n;
    if (e + 64u > LOG + LOG_SIZE) {
        fprintf(stderr, "call log full\n");
        abort();
    }
    wr32(LOG, n + 1u);
    const uint32_t esp = c->r[4];
    const uint32_t a1 = rd32(esp + 4u), a2 = rd32(esp + 8u), a3 = rd32(esp + 12u);
    const uint32_t w[16] = {addr, rd32(esp), esp, c->r[0], c->r[1], c->r[2], c->r[3], c->r[5], c->r[6], c->r[7],
                            a1, a2, a3, c->fpu_top | ((uint32_t)fpu_tagword(c) << 16), c->fpu_sw, 0};
    for (unsigned i = 0; i < 16; ++i)
        wr32(e + 4u * i, w[i]);
    uint32_t h = mix(case_seed, addr);
    h = mix(h, n);
    for (unsigned i = 1; i < 13; ++i)
        h = mix(h, w[i]);
    return h;
}

/* The word a pointer argument points to, into the last log record's spare slot. */
static void fake_note(uint32_t v) {
    const uint32_t n = rd32(LOG);
    wr32(LOG + 4u + 64u * (n - 1u) + 60u, v);
}

static void fake_leave(X86 *c, uint32_t h, uint32_t al, uint32_t retn) {
    c->r[0] = (mix(h, 1) & 0xffffff00u) | (al & 0xffu);
    c->r[1] = mix(h, 2);
    c->r[2] = mix(h, 3);
    for (int i = 0; i < 4; ++i)
        c->xmm[0][i] = mix(h, 4u + (uint32_t)i);
    if ((h & 0x3ffu) == 0x155u)
        c->fpu_sw |= 0x0001u;
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u + retn;
}

/* AL: mostly 1, sometimes 0, now and then another value (not 1, so "false" to CMP AL,1). */
static uint32_t pick_bool(uint32_t h, unsigned true_in_8) {
    const unsigned k = (h >> 20) & 7u;
    if (k < true_in_8)
        return 1u;
    return (h >> 24) % 5u == 0 ? 2u + ((h >> 8) & 0x7du) : 0u;
}

static double pick_float(uint32_t h, double around) {
    switch ((h >> 16) % 16u) {
    case 0: return 0.0;
    case 1: return -0.0;
    case 2: return (double)dt_bits(0x7fc00000u);
    case 3: return around;
    case 4: return (double)nextafterf((float)around, 1e30f);
    case 5: return (double)nextafterf((float)around, -1e30f);
    case 6: return 1.0;
    case 7: return -1.0;
    case 8: return (double)dt_bits(1u + (h & 0xffffu)); /* denormal */
    case 9: return around + (double)((int32_t)mix(h, 9)) * 1e-17; /* rounds per RC */
    case 10: return (double)dt_bits(0x7f800000u);
    case 11: return 1.0 / 3.0;
    default: return (double)((int32_t)mix(h, 11)) / 1073741824.0;
    }
}

/* CullVPTHC(item = ECX, db), RET 4: rotates the item's vertex bits as the real one does. */
void fn_104030c0(X86 *c) {
    const uint32_t h = fake_enter(c, 0x104030c0u);
    const uint32_t it = c->r[1];
    wr16(it + 0x38u, (uint16_t)(rd16(it + 0x38u) ^ ((h >> 4) & 0xeu)));
    fake_leave(c, h, pick_bool(h, 5), 4);
}

/* UpdateNodeData(item = ECX, db, &flags), RET 8: area into item+0, and now and then a changed
 * flag word (0x2000000) or cull byte (bit 5), which the caller must read back. */
void fn_10403390(X86 *c) {
    const uint32_t h = fake_enter(c, 0x10403390u);
    const uint32_t it = c->r[1], flags = rd32(c->r[4] + 8u);
    fake_note(rd32(flags));
    wrf32(it, (float)pick_float(h, 0.5));
    if ((h & 7u) == 1u)
        wr32(flags, rd32(flags) ^ 0x2000000u);
    if (((h >> 3) & 7u) == 2u)
        wr8(it + 0x38u, (uint8_t)(rd8(it + 0x38u) ^ 0x20u));
    fake_leave(c, h, h >> 9, 8);
}

/* CullSmallObject(item = ECX, db, bool), RET 8: the factor in ST0 (and item+4). */
void fn_10402f30(X86 *c) {
    const uint32_t h = fake_enter(c, 0x10402f30u);
    const double v = pick_float(h, (double)rdf32(EPS));
    wrf32(c->r[1] + 4u, (float)v);
    fake_leave(c, h, h >> 9, 8);
    fpush(c, v);
}

/* CalculateLoDArea(item = ECX, db), RET 4. */
void fn_10402f00(X86 *c) {
    const uint32_t h = fake_enter(c, 0x10402f00u);
    wrf32(c->r[1] + 8u, (float)pick_float(h, 2.0));
    if ((h & 15u) == 3u)
        wr32(ENT + 0x3cu, rd32(ENT + 0x3cu) ^ 0x2000000u);
    fake_leave(c, h, h >> 9, 4);
}

/* GetNearPlaneDistance(filter = ECX, box), RET 4: the distance in ST0. */
void fn_1040caf0(X86 *c) {
    const uint32_t h = fake_enter(c, 0x1040caf0u);
    fake_note(rd32(rd32(c->r[4] + 4u)));
    const double d = pick_float(h, 0.0);
    fake_leave(c, h, h >> 9, 4);
    fpush(c, d);
}

/* push_back(array = EAX, &value), RET 4: logs the value, grows the count; sometimes a new
 * query result in the item (+0x1c), which the caller compares after the call. */
void fn_10445780(X86 *c) {
    const uint32_t h = fake_enter(c, 0x10445780u);
    const uint32_t arr = c->r[0], p = rd32(c->r[4] + 4u);
    const uint32_t v = rd32(p);
    fake_note(v);
    wr32(arr + 4u, rd32(arr + 4u) + 1u);
    if ((h & 3u) == 0u && v == ITEM)
        wr32(ITEM + 0x1cu, mix(h, 7) % 64u);
    fake_leave(c, h, h >> 9, 4);
}

/* ResetDynamicQuery(item = ECX), RET. */
void fn_10403580(X86 *c) {
    const uint32_t h = fake_enter(c, 0x10403580u);
    wr32(c->r[1] + 0x18u, 0u);
    wr32(c->r[1] + 0x1cu, 0xffffffffu);
    fake_leave(c, h, h >> 9, 0);
}

/* CheckIfOccluder(item = ECX), RET. */
void fn_10402ec0(X86 *c) {
    const uint32_t h = fake_enter(c, 0x10402ec0u);
    fake_leave(c, h, pick_bool(h, 4), 0);
}

/* RenderZPass(item = ECX), RET. */
void fn_10403820(X86 *c) {
    const uint32_t h = fake_enter(c, 0x10403820u);
    fake_leave(c, h, h >> 9, 0);
}

/* The filter callbacks reached through FilterEntityCullCallback's CALL EDX, stdcall, RET 8. */
static void fake_set_sampler_state(X86 *c);

void recomp_call_ic(X86 *c, uint32_t target, uint64_t *ic) {
    (void)ic;
    if (target == CB0 || target == CB1) {
        const uint32_t h = fake_enter(c, target);
        fake_leave(c, h, pick_bool(h, target == CB0 ? 3 : 6), 8);
        return;
    }
    if (target == SETSS) {
        fake_set_sampler_state(c);
        return;
    }
    fprintf(stderr, "unexpected indirect call to %08x\n", target);
    abort();
}

static uint32_t r8(unsigned n_in_8, uint8_t yes) {
    return (dt_rand32() & 7u) < n_in_8 ? yes : (uint8_t)dt_rand32();
}

static void fill_random(uint32_t a, uint32_t n) {
    for (uint32_t k = 0; k < n; k += 4)
        wr32(a + k, dt_rand32());
}

static void setup_case(X86 *c) {
    case_seed = dt_rand32();
    fill_random(DB, 0x300u);
    fill_random(ENT, 0x200u);
    memset(g_mem + LOG, 0, LOG_SIZE);

    const uint32_t frame = dt_rand32() % 1000u;
    wr32(DB + 4u, frame);
    /* the stamp: already this frame now and then */
    wr32(ITEM + 0x10u, dt_rand32() % 5u == 0 ? frame : frame - 1u - dt_rand32() % 3u);

    /* the two function-static masks: initialised or not, the real values or others */
    wr32(GUARD, (dt_rand32() & ~3u & 0xffu) | (dt_rand32() % 6u == 0 ? dt_rand32() & 3u : 3u));
    wr32(REQUIRE, dt_rand32() % 8u == 0 ? dt_rand32() & 0x0011010cu : 0x10010cu);
    wr32(EXCLUDE, dt_rand32() % 8u == 0 ? dt_rand32() & 0x41u : 0x40u);

    uint32_t flags = dt_rand32();
    if (dt_rand32() % 8u) {
        flags |= 0x10010cu;
        flags &= ~0x40u;
    }
    if (dt_rand32() % 2u)
        flags &= ~0x2000000u;
    wr32(ENT + 0x3cu, flags);
    wr8(ITEM + 0x38u, (uint8_t)dt_rand32());
    wr8(DB + 0x1bdu, r8(4, 1));
    wr8(DB + 0x1afu, r8(5, 1));
    wr8(DB + 0x192u, r8(5, 1));
    wr8(DB + 0x1ceu, (uint8_t)(dt_rand32() % 3u));
    wr32(ITEM + 0x18u, dt_rand32() % 2u ? 0u : dt_rand32());
    wr32(ITEM + 0x1cu, dt_rand32() % 64u);
    wr32(DB + 0x1b4u, dt_rand32() % 64u);

    /* filter callbacks: none (true), or one or two */
    const uint32_t ncb = dt_rand32() % 4u == 0 ? 1u + dt_rand32() % 2u : 0u;
    wr16(DB + 0x216u, (uint16_t)ncb);
    wr32(DB + 0x218u, CBS);
    wr32(CBS, CB0);
    wr32(CBS + 4u, CB1);

    wrf32(EPS, dt_rand32() % 4u ? 1.1920929e-07f : dt_float());
    wr32(EPS_IAT, EPS);

    dt_context(c, ESP0, dt_rand32() % 6u);
    c->r[1] = DB;
    const uint32_t status = dt_rand32() % 3u == 0 ? 0u : (dt_rand32() % 4u == 0 ? 0xfu : 1u + dt_rand32() % 2u);
    c->r[4] -= 4u;
    wr32(c->r[4], status);
    c->r[4] -= 4u;
    wr32(c->r[4], ENT);
    c->r[4] -= 4u;
    wr32(c->r[4], RET_SITE);
}

/* What the case reached: the last call logged and how it ended. */
static void count_path(const X86 *c) {
    const uint32_t n = rd32(LOG);
    const uint32_t last = n ? rd32(LOG + 4u + 64u * (n - 1u)) : 0u;
    unsigned k;
    switch (last) {
    case 0: k = 0; break;            /* no call: stamp, enable bits or masks */
    case CB0: case CB1: case 0x103fd330u: k = 1; break;
    case 0x104030c0u: k = 2; break;
    case 0x10402f30u: k = 3; break;  /* small-object cull said no */
    case 0x10402f00u: k = 4; break;  /* flags 0x2000000 */
    case 0x10445780u: k = rd32(LOG + 4u + 64u * (n - 1u) + 4u) == 0x103ffab9u ? 5 : 6; break;
    case 0x10402ec0u: k = 7; break;
    case 0x10403820u: k = 8; break;
    case 0x10403580u: k = 9; break;  /* ResetDynamicQuery after a cull */
    default: k = 10; break;
    }
    paths[k * 2 + ((c->r[0] & 0xffu) != 0)]++;
}

static void test_extract(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        setup_case(&c);
        if (dt_run("ExtractFromEntity", &c, fn_103ff970, g3n_103ff970))
            count_path(&c);
    }
}

/* With the group off the native is the translation. */
/* ---- FUN_104956f0's callees ---------------------------------------------------------------- */

/* SetUnitNumber(image = ECX, stage), RET 4. */
void fn_102f1430(X86 *c) {
    const uint32_t h = fake_enter(c, 0x102f1430u);
    fake_leave(c, h, h >> 9, 4);
}

/* Execute(image = ECX), RET: EAX 1 (bound) mostly. */
void fn_102f1560(X86 *c) {
    const uint32_t h = fake_enter(c, 0x102f1560u);
    fake_leave(c, h, 0, 0);
    if ((h >> 20) % 8u)
        c->r[0] = 1u;
    else if ((h >> 23) & 1u)
        c->r[0] = 0x101u; /* AL = 1, but not 1: CMP EAX,1 fails */
}

/* SetSamplerState(object = ECX, stage, type, value), thiscall RET 0xc: now and then changes a
 * wanted state (read back after the call) or the wrapper's default. */
static void fake_set_sampler_state(X86 *c) {
    const uint32_t h = fake_enter(c, SETSS);
    if ((h & 7u) == 1u)
        wr32(WANT + 4u * ((h >> 3) % 6u), mix(h, 5));
    if (((h >> 6) & 7u) == 2u)
        wr32(WRAPPER + 0x48u, (h >> 9) % 5u);
    fake_leave(c, h, h >> 9, 12);
}

static void setup_samp(X86 *c) {
    case_seed = dt_rand32();
    memset(g_mem + LOG, 0, LOG_SIZE);
    fill_random(DEVSTATE, 0x1000u);
    wr32(RENDERER + 4u, DEVSTATE);
    wr32(RENDERER + 8u, WRAPPER);
    wr32(WRAPPER + 0xcu, SSOBJ);
    wr32(WRAPPER + 0x48u, dt_rand32() % 5u);
    wr32(SSOBJ, SSVTBL);
    wr32(SSVTBL + 0x1f8u, SETSS);
    const uint32_t stage = dt_rand32() % 16u;
    wr32(STAGE + 0x28u, stage);
    const uint32_t img = dt_rand32() % 6u == 0 ? 0u : IMG;
    if (dt_rand32() % 3u == 0)
        wr32(DEVSTATE + 0x7c4u + 4u * stage, img);
    const uint32_t cache = DEVSTATE + 0x804u + 0x18u * stage;
    for (uint32_t k = 0; k < 6u; ++k) {
        uint32_t v;
        switch (dt_rand32() % 4u) {
        case 0: v = 0xababababu; break;
        case 1: v = dt_rand32(); break;
        default: v = rd32(cache + 4u * k); break; /* unchanged */
        }
        wr32(WANT + 4u * k, v);
        if (v == 0xababababu && dt_rand32() % 4u == 0)
            wr32(cache + 4u * k, v);
    }
    dt_context(c, ESP0, dt_rand32() % 8u);
    c->r[0] = img;
    c->r[7] = WANT;
    c->r[4] -= 4u;
    wr32(c->r[4], STAGE);
    c->r[4] -= 4u;
    wr32(c->r[4], RENDERER);
    c->r[4] -= 4u;
    wr32(c->r[4], 0x104a1300u); /* a return site in a caller */
}

static unsigned samp_calls[16];

static void test_samp(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        setup_samp(&c);
        if (dt_run("FUN_104956f0", &c, fn_104956f0, g3n_104956f0)) {
            const uint32_t k = rd32(LOG);
            samp_calls[k < 15u ? k : 15u]++;
        }
    }
}

static void test_off(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        setup_case(&c);
        X86 start = c, o;
        dt_save(0);
        g3_opt = 0;
        fn_103ff970(&c);
        o = c;
        dt_save(1);
        dt_restore();
        c = start;
        g3_opt = ~(G3OPT_SCENE | G3OPT_CULL);
        g3n_103ff970(&c);
        dt_compare("ExtractFromEntity (scene off)", &o, &c);
    }
    g3_opt = 0xffffffffu;
}

/* G3N_BENCH=1: the same 256 cases (the stamp reset each time, the stack and the log restored)
 * through each, ns per call; the fakes and the restore cost the same in both, so the difference
 * is what the native saves per entity. */
static double now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static void null_call(X86 *c) {
    c->eip = rd32(c->r[4]);
    c->r[4] += 12u;
}

static void bench(void) {
    enum { NC = 256, REP = 4000 };
    static X86 start[NC];
    static uint32_t seed[NC];
    static uint8_t mem[NC][0x300 + 0x200 + 0x20];
    for (unsigned i = 0; i < NC; ++i) {
        do
            setup_case(&start[i]);
        while (rd32(ITEM + 0x10u) == rd32(DB + 4u)); /* every case does the work */
        seed[i] = case_seed;
        memcpy(mem[i], g_mem + DB, 0x300);
        memcpy(mem[i] + 0x300, g_mem + ENT, 0x200);
        memcpy(mem[i] + 0x500, g_mem + 0x10874c00u, 0x20);
    }
    void (*const f[3])(X86 *) = {fn_103ff970, g3n_103ff970, null_call};
    double t[3];
    bench_fast = 1;
    for (int k = 0; k < 3; ++k) {
        g3_opt = k ? ~G3OPT_CULL : 0u;
        const double t0 = now_ns();
        for (unsigned rep = 0; rep < REP; ++rep)
            for (unsigned i = 0; i < NC; ++i) {
                memcpy(g_mem + DB, mem[i], 0x300);
                memcpy(g_mem + ENT, mem[i] + 0x300, 0x200);
                memcpy(g_mem + 0x10874c00u, mem[i] + 0x500, 0x20);
                wr32(LOG, 0u);
                case_seed = seed[i];
                X86 c = start[i];
                f[k](&c);
            }
        t[k] = (now_ns() - t0) / (double)(REP * NC);
    }
    g3_opt = 0xffffffffu;
    bench_fast = 0;
    printf("bench ExtractFromEntity, ns per call less the restore (%.1f ns): translation %.1f, native %.1f "
           "(callees are fakes, warm cache)\n",
           t[2], t[0] - t[2], t[1] - t[2]);
}

int main(void) {
    harness_mem();
    dt_load_pe("Engine.dll");
    /* the callees below are fakes; the cull group would run its natives in their place */
    dt_native_opt = ~G3OPT_CULL;
    dt_stack_floor = ESP0 - 0x800u;
    dt_region(dt_stack_floor, 0x900u);
    dt_region(DB, 0x300u);
    dt_region(ENT, 0x200u);
    dt_region(CBS, 0x200u);
    dt_region(LOG, LOG_SIZE);
    dt_region(0x10874c00u, 0x20u);

    dt_region(RENDERER, 0x10u);
    dt_region(DEVSTATE, 0x1000u);
    dt_region(WRAPPER, 0x300u);
    dt_region(STAGE, 0x40u);
    dt_region(WANT, 0x20u);

    test_extract(400000);
    test_off(2000);
    test_samp(300000);
    printf("FUN_104956f0 calls made per case:");
    for (unsigned k = 0; k < 16; ++k)
        if (samp_calls[k])
            printf(" %u:%u", k, samp_calls[k]);
    printf("\n");

    static const char *names[] = {"no call", "filter", "VPT cull", "small object", "flag 0x2000000",
                                  "query below threshold", "render list", "occluder check", "z pass",
                                  "reset query", "other"};
    printf("paths (false/true):");
    for (unsigned k = 0; k < 11; ++k)
        printf(" %s %u/%u;", names[k], paths[2 * k], paths[2 * k + 1]);
    printf("\n");
    if (getenv("G3N_BENCH"))
        bench();
    return dt_report("scene_test");
}
