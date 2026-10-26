/* native/tests/audio_test.c - native/audio.c: the occlusion ray cache in FUN_102b4490.
 *
 * Build and run (from the checkout root):
 *   sh native/tests/build.sh audio aux-engine/chunk_102b4000.c
 *
 * The chunk brings the translated fn_102b4490; eCPhysicsScene::TraceRayNearestHit (103cd980) is a
 * mock here: a wall at x = WALL_X, material from the hit height, the query report at
 * [scene+0xac] filled like the real one ({hit byte, +0x64 material, +0x68 position}) and the
 * filter at [scene+0xa0] cleared on the way out, unless the scene is "locked" (the real trace
 * returns early then and leaves the filter set). The clock is the test's.
 *
 * 1. Cold cache: native against the original, every register and the out/report/scene bytes.
 * 2. Warm cache: the same ray traced once by the native, then the original (a fresh trace)
 *    against the native answering from the cache, same comparison; the mock must not run.
 * 3. Bookkeeping: tolerance (absolute and relative), ends checked separately, reversed ray,
 *    age, scene change, locked scene, group off, NaN, a cached miss, eviction. */
#include "difftest_rp.h"

#include <math.h>

void fn_102b4490(X86 *c);
void g3n_102b4490(X86 *c);

typedef struct {
    uint64_t calls, hits, traced, uncached;
} G3nAudioStats;
extern G3nAudioStats g3n_audio_stats;
extern uint64_t (*g3n_audio_clock_us)(void);
void g3n_audio_flush(void);

#define ESP0 0x0e800000u
#define RET_SITE 0x102b49a4u /* after 102b499f CALL 0x102b4490 (first ray) */
#define SCENE 0x03000000u
#define SCENE2 0x03000400u
#define REPORT 0x03000800u
#define VEC 0x03001000u /* origin, dir, hit out, material out */
#define V_ORIGIN (VEC + 0x00u)
#define V_DIR (VEC + 0x10u)
#define V_HIT (VEC + 0x20u)
#define V_MAT (VEC + 0x30u)
#define SCENE_PTR 0x10862dccu
#define WALL_X 100.0f

#define EPS_ABS 20.0f
#define EPS_REL 0.02f
#define MAX_AGE_US 200000u
#define CACHE_N 256u

static uint64_t now_us = 1000000u;
static uint64_t test_clock(void) {
    return now_us;
}

static unsigned traces;
static int locked;

/* TraceRayNearestHit(this=ECX, origin, dir, ent, flags, ent, pst, group), RET 0x1c. */
void fn_103cd980(X86 *c) {
    const uint32_t esp = c->r[4], scene = c->r[1];
    const uint32_t po = rd32(esp + 4u), pd = rd32(esp + 8u);
    ++traces;
    uint32_t hit = 0;
    if (!locked) {
        const uint32_t rep = rd32(scene + 0xacu);
        const float ox = rdf32(po), oy = rdf32(po + 4u), oz = rdf32(po + 8u);
        const float dx = rdf32(pd), dy = rdf32(pd + 4u), dz = rdf32(pd + 8u);
        wr8(rep, 0);
        if (dx != 0.0f) {
            const float t = (WALL_X - ox) / dx;
            if (t >= 0.0f && t <= 1.0f) {
                hit = 1;
                const float hy = oy + t * dy;
                wr8(rep, 1);
                wr32(rep + 0x64u, 1u + ((uint32_t)fabsf(hy) / 50u) % 7u);
                wrf32(rep + 0x68u, WALL_X);
                wrf32(rep + 0x6cu, hy);
                wrf32(rep + 0x70u, oz + t * dz);
            }
        }
        wr32(scene + 0xa0u, 0);
    }
    c->r[0] = hit;
    /* scratch leftovers, a function of the ray so a cached answer can be compared exactly */
    const uint32_t h = rd32(po) * 0x9e3779b1u ^ rd32(po + 4u) ^ rd32(pd + 8u) * 0x85ebca6bu;
    c->r[1] = 0xc1c1c100u ^ h;
    c->r[2] = 0xd2d2d200u ^ (h >> 3);
    c->eip = rd32(esp);
    c->r[4] = esp + 4u + 0x1cu;
}

static void scene_init(uint32_t scene) {
    for (uint32_t k = 0; k < 0x100u; k += 4)
        wr32(scene + k, 0x5a5a5a5au);
    wr32(scene + 0xa0u, 0);
    wr32(scene + 0xacu, REPORT);
}

static void set_ray(const float *o, const float *d) {
    for (int k = 0; k < 3; ++k) {
        wrf32(V_ORIGIN + 4u * k, o[k]);
        wrf32(V_DIR + 4u * k, d[k]);
        wr32(V_HIT + 4u * k, 0xeeeeeeeeu);
    }
    wr32(V_MAT, 0xeeeeeeeeu);
}

/* Guest state for a call from 102b499f: ECX origin, EAX dir, EDI hit out, [ESP+4] material out. */
static void set_call(X86 *c) {
    c->r[1] = V_ORIGIN;
    c->r[0] = V_DIR;
    c->r[7] = V_HIT;
    c->r[4] = ESP0;
    wr32(ESP0, RET_SITE);
    wr32(ESP0 + 4u, V_MAT);
}

/* One call of the native with fresh registers; returns AL. */
static unsigned ray(const float *o, const float *d) {
    X86 c;
    dt_context(&c, ESP0, 0);
    set_ray(o, d);
    set_call(&c);
    g3n_102b4490(&c);
    if (c.r[4] != ESP0 + 8u || c.eip != RET_SITE) {
        fprintf(stderr, "FAIL bad return esp %08x eip %08x\n", c.r[4], c.eip);
        ++dt_fail;
    }
    return c.r[0] & 0xffu;
}

static float frand(float lo, float hi) {
    return lo + (hi - lo) * (float)(dt_rand32() & 0xffffffu) / 16777216.0f;
}

static void random_ray(float *o, float *d) {
    o[0] = frand(-500.0f, 500.0f);
    o[1] = frand(-300.0f, 300.0f);
    o[2] = frand(-300.0f, 300.0f);
    for (int k = 0; k < 3; ++k)
        d[k] = frand(-1500.0f, 1500.0f);
    switch (dt_rand32() % 8) {
    case 0: d[0] = 0.0f; break;            /* parallel to the wall */
    case 1: d[0] = d[1] = d[2] = 0.0f; break; /* empty segment */
    case 2: o[dt_rand32() % 3] = dt_float(); break; /* NaN, inf, odd values */
    default: break;
    }
}

static void check(int ok, const char *what) {
    ++dt_cases;
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        ++dt_fail;
    }
}

static unsigned unkept;

static void differential(void) {
    for (int i = 0; i < 20000; ++i) {
        float o[3], d[3];
        random_ray(o, d);
        X86 c;
        locked = dt_rand32() % 16 == 0;

        /* cold */
        g3n_audio_flush();
        dt_context(&c, ESP0, dt_rand32() % 4);
        set_ray(o, d);
        set_call(&c);
        dt_run("cold", &c, fn_102b4490, g3n_102b4490);

        /* warm: the native traces it once, then answers from the cache */
        g3n_audio_flush();
        dt_context(&c, ESP0, dt_rand32() % 4);
        set_ray(o, d);
        set_call(&c);
        X86 prime = c;
        g3_opt = 0xffffffffu;
        const uint64_t u0 = g3n_audio_stats.uncached;
        g3n_102b4490(&prime);
        set_ray(o, d); /* garbage in the outputs again */
        const unsigned t1 = traces;
        const uint64_t h0 = g3n_audio_stats.hits;
        const int kept = g3n_audio_stats.uncached == u0;
        /* dt_run: original (one trace), then the native (none if the ray was kept) */
        dt_run("warm", &c, fn_102b4490, g3n_102b4490);
        if (kept) {
            check(traces == t1 + 1u && g3n_audio_stats.hits == h0 + 1u, "warm call answered from the cache");
        } else {
            check(traces == t1 + 2u, "unkept ray traced again");
            ++unkept;
        }
    }
    locked = 0;
}

static void bookkeeping(void) {
    g3_opt = 0xffffffffu;
    g3n_audio_flush();
    unsigned t;

    /* the same ray: one trace, then answers */
    const float o[3] = {0.0f, 10.0f, 20.0f}, d[3] = {300.0f, 0.0f, 0.0f}; /* hits the wall, len 300 */
    t = traces;
    check(ray(o, d) == 1u, "hit");
    check(ray(o, d) == 1u, "cached hit");
    check(rd32(V_MAT) != 0xeeeeeeeeu && rdf32(V_HIT) == WALL_X, "cached hit writes the outputs");
    check(traces == t + 1u, "same ray traced once");

    /* len 300: eps = max(20, 6) = 20 */
    float o2[3] = {o[0], o[1] + 0.9f * EPS_ABS, o[2]};
    t = traces;
    ray(o2, d); /* origin and end both moved by 18 */
    check(traces == t, "inside the absolute tolerance");
    o2[1] = o[1] + 1.1f * EPS_ABS;
    ray(o2, d);
    check(traces == t + 1u, "outside the absolute tolerance");

    /* end alone moved */
    float d2[3] = {d[0], d[1], d[2] + 25.0f};
    t = traces;
    ray(o, d2);
    check(traces == t + 1u, "end outside the tolerance");

    /* long ray: len 5000, eps = 100 */
    g3n_audio_flush();
    const float lo[3] = {-2000.0f, 0.0f, 0.0f}, ld[3] = {5000.0f, 0.0f, 0.0f};
    ray(lo, ld);
    t = traces;
    float lo2[3] = {lo[0] + 60.0f, lo[1] + 60.0f, lo[2]};
    ray(lo2, ld);
    check(traces == t, "inside the relative tolerance (|d| = 85 < 100)");
    lo2[1] = 85.0f;
    ray(lo2, ld);
    check(traces == t + 1u, "outside the relative tolerance (|d| = 104 > 100)");

    /* reversed ray (listener -> source) is a different ray */
    g3n_audio_flush();
    ray(o, d);
    const float ro[3] = {o[0] + d[0], o[1], o[2]}, rd[3] = {-d[0], 0.0f, 0.0f};
    t = traces;
    ray(ro, rd);
    check(traces == t + 1u, "reversed ray traced");

    /* age */
    g3n_audio_flush();
    ray(o, d);
    t = traces;
    now_us += MAX_AGE_US;
    ray(o, d);
    check(traces == t, "at the age limit: cached");
    now_us += 1u;
    ray(o, d);
    check(traces == t + 1u, "past the age limit: traced");
    ray(o, d);
    check(traces == t + 1u, "re-traced ray cached again");

    /* scene change */
    wr32(SCENE_PTR, SCENE2);
    t = traces;
    ray(o, d);
    check(traces == t + 1u, "other scene traced");
    wr32(SCENE_PTR, SCENE);
    ray(o, d);
    check(traces == t + 1u, "first scene's entry still there");

    /* locked scene: the filter stays set, nothing kept */
    g3n_audio_flush();
    locked = 1;
    t = traces;
    ray(o, d);
    wr32(SCENE + 0xa0u, 0); /* someone else's trace clears it */
    ray(o, d);
    check(traces == t + 2u, "locked scene not cached");
    locked = 0;
    wr32(SCENE + 0xa0u, 0);

    /* group off: always the original, the cache untouched */
    g3n_audio_flush();
    ray(o, d);
    g3_opt = 0;
    const uint64_t calls = g3n_audio_stats.calls;
    t = traces;
    ray(o, d);
    ray(o, d);
    check(traces == t + 2u && g3n_audio_stats.calls == calls, "group off traces");
    g3_opt = 0xffffffffu;
    ray(o, d);
    check(traces == t + 2u, "group back on: entry still used");

    /* NaN never matches and is never kept */
    const float no[3] = {dt_bits(0x7fc00000u), 0.0f, 0.0f};
    t = traces;
    ray(no, d);
    ray(no, d);
    check(traces == t + 2u, "NaN traced every time");

    /* a cached miss: AL 0, outputs untouched */
    const float mo[3] = {0.0f, 0.0f, 0.0f}, md[3] = {50.0f, 0.0f, 0.0f}; /* stops short of the wall */
    t = traces;
    check(ray(mo, md) == 0u, "miss");
    check(ray(mo, md) == 0u && traces == t + 1u, "cached miss");
    check(rd32(V_MAT) == 0xeeeeeeeeu && rd32(V_HIT) == 0xeeeeeeeeu, "cached miss leaves the outputs");

    /* eviction: CACHE_N + 44 distinct rays, one per microsecond; the oldest go first */
    g3n_audio_flush();
    for (unsigned i = 0; i < CACHE_N + 44u; ++i) {
        const float eo[3] = {0.0f, 1000.0f * (float)i, 0.0f};
        ++now_us;
        ray(eo, d);
    }
    t = traces;
    for (unsigned i = 44; i < CACHE_N + 44u; ++i) {
        const float eo[3] = {0.0f, 1000.0f * (float)i, 0.0f};
        ray(eo, d);
    }
    check(traces == t, "the newest CACHE_N rays still cached");
    const float first[3] = {0.0f, 0.0f, 0.0f};
    ray(first, d);
    check(traces == t + 1u, "the oldest ray evicted");
}

int main(void) {
    harness_mem();
    g3n_audio_clock_us = test_clock;
    scene_init(SCENE);
    scene_init(SCENE2);
    wr32(SCENE_PTR, SCENE);
    dt_stack_floor = ESP0 - 0x400u;
    dt_region(dt_stack_floor, 0x400u);
    dt_region(SCENE, 0x100u);
    dt_region(REPORT, 0x80u);
    dt_region(VEC, 0x40u);

    differential();
    const unsigned diff_cases = dt_cases;
    bookkeeping();
    printf("differential %u (warm rays not kept: %u), bookkeeping %u; stats: calls %llu hits %llu traced %llu uncached %llu\n",
           diff_cases, unkept, dt_cases - diff_cases, (unsigned long long)g3n_audio_stats.calls,
           (unsigned long long)g3n_audio_stats.hits, (unsigned long long)g3n_audio_stats.traced,
           (unsigned long long)g3n_audio_stats.uncached);
    return dt_report("audio");
}
