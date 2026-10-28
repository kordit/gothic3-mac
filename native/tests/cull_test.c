/* native/tests/cull_test.c - native/cull.c against the translation.
 *
 * Build and run (from the checkout root):
 *   NATIVE="cull.c scene.c vector.c leaf.c" sh native/tests/build.sh cull \
 *       aux-engine/chunk_10400000.c aux-engine/chunk_1040c000.c \
 *       aux-sharedbase/chunk_1001c000.c aux-sharedbase/chunk_10040000.c aux-sharedbase/chunk_10034000.c \
 *       aux-sharedbase/chunk_10018000.c
 * G3N_BENCH=1 adds a timing of CullVPTHC and UpdateNodeData.
 *
 * Every callee but two is the real translated function (the SharedBase ones through Engine's
 * import slots, answered below by address), and with the groups on the natives of vector.c,
 * leaf.c and scene.c that cull.c calls in place; GetMagnitude and the singleton's first call are
 * fakes (see them below). Each case runs the original (all
 * groups off) and the native (all on) from the same state: random registers, x87 top with live
 * caller values, stale tags, rounding and precision control words, XMM (difftest_rp.h), and an
 * item, entity, filter and camera filled with geometry that takes every path (corner inside or
 * not, sphere outside, cut or inside each plane, box overlapping the bounds or not, the area
 * factor below 0, above 1, the dot product 0) plus now and then NaN, infinities and denormals.
 * Compared bitwise: registers, x87 state, XMM and memory (stack above the final ESP). Now and
 * then an import slot holds another address (a hooked import): the native must then run the
 * translation. */
#include "difftest_rp.h"

#include <math.h>
#include <time.h>

void fn_104030c0(X86 *c);
void g3n_104030c0(X86 *c);
void fn_1040cb50(X86 *c);
void g3n_1040cb50(X86 *c);
void fn_1040cba0(X86 *c);
void g3n_1040cba0(X86 *c);
void fn_1040c940(X86 *c);
void g3n_1040c940(X86 *c);
void fn_10403390(X86 *c);
void g3n_10403390(X86 *c);
void fn_1040c5a0(X86 *c);
void g3n_1040c5a0(X86 *c);
void fn_10402f00(X86 *c);
void g3n_10402f00(X86 *c);

void fn_1001d070(X86 *c);
void fn_10043670(X86 *c);
void fn_10036690(X86 *c);
void g3n_1001da30(X86 *c);
void fn_1001b580(X86 *c);
void g3n_1001ae70(X86 *c);
void fn_10010c50(X86 *c);

/* bCVector::GetMagnitude (SharedBase 10010c50) is a fake here: its CRT sqrt reaches errno and the
 * thread data on a NaN. Both sides call it (the original through the import slot, the native
 * directly); it answers from what it was handed (ECX's vector, ESP, the return address, the x87
 * top and tags), so a call made from another state gives another result. */
static uint32_t gm_mix(uint32_t h, uint32_t v) {
    h ^= v + 0x7f4a7c15u + (h << 6) + (h >> 2);
    h *= 0x85ebca6bu;
    return h ^ (h >> 15);
}
void fn_10010c50(X86 *c) {
    const uint32_t v = c->r[1];
    uint32_t h = gm_mix(rd32(c->r[4]), c->r[4]);
    h = gm_mix(h, v);
    h = gm_mix(h, c->fpu_top | ((uint32_t)c->fpu_tag << 8));
    const double x = rdf32(v), y = rdf32(v + 4u), z = rdf32(v + 8u);
    const double m = sqrt(x * x + y * y + z * z);
    c->r[0] = gm_mix(h, 1);
    c->r[2] = gm_mix(h, 2);
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u;
    fpush(c, (double)(float)(m == m ? m + (double)(h & 7u) : m));
}

/* FUN_10277ac0 (the application singleton) is a fake too, for its first call, which builds the
 * object through half the engine: with the module enabled and the guard clear it sets the guard,
 * stores the object and logs the return address and ESP it was called with into compared memory;
 * the translation's fast paths are reproduced as they are (EAX 0 when disabled). */
#define APP_LOG 0x03003800u
void fn_10277ac0(X86 *c) {
    c->r[0] = 1u;
    if (rd8(0x10862a8cu) != 1u) {
        c->r[0] = 0u;
    } else {
        if (!(rd8(0x10874bd4u) & 1u)) {
            wr32(0x10874bd4u, rd32(0x10874bd4u) | 1u);
            wr32(APP_LOG, rd32(c->r[4]));
            wr32(APP_LOG + 4u, c->r[4]);
            wr32(APP_LOG + 8u, rd32(APP_LOG + 8u) + 1u);
            c->r[1] = gm_mix(c->r[4], 3);
            c->r[2] = gm_mix(c->r[4], 4);
            wr32(0x10874bd0u, 0x03003000u);
        }
        c->r[0] = rd32(0x10874bd0u);
    }
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u;
}

#define ESP0 0x0e800000u
#define DB 0x03000000u    /* eCFrustumDatabase, 0x300 bytes; its filter at +0x40 */
#define FILTER (DB + 0x40u)
#define ENT 0x03001000u   /* eCEntity, 0x200 bytes */
#define ITEM 0x03001400u  /* eCFrustumElementItem, 0x40 bytes, +0xc = ENT */
#define CAM 0x03002000u   /* the database's camera ([DB+0x134]), position +0x8c */
#define FCAM 0x03002200u  /* the filter's camera ([FILTER+0xc]), position +0x8c */
#define LODOBJ 0x03002400u /* [ENT+0x2c], +0x48 */
#define APPOBJ 0x03003000u /* FUN_10277ac0's object, counters at +0x168, +0x170 */
#define PI_VAR 0x03003400u
#define FLAGS 0x03003500u  /* eSEntityFlags */
#define HOOK 0x0fd00000u   /* a "hooked" import: answered by the same function */
#define STATICS 0x1086dc00u

#define IAT_GET_VERTEX 0x105f6df8u
#define IAT_SPHERE_HAS_POINT 0x105f6a34u
#define IAT_FRUSTUM_HAS_POINT 0x105f6e28u
#define IAT_SPHERES 0x105f6a0cu
#define IAT_DOT 0x105f6854u
#define IAT_INV_TRANSLATE 0x105f66ecu
#define IAT_MAGNITUDE 0x105f67c0u
#define IAT_PI 0x105f63fcu

static const struct {
    uint32_t slot, target;
    void (*f)(X86 *);
} imports[] = {
    {IAT_GET_VERTEX, 0x1001d070u, fn_1001d070},   {IAT_SPHERE_HAS_POINT, 0x10043670u, fn_10043670},
    {IAT_FRUSTUM_HAS_POINT, 0x10036690u, fn_10036690}, {IAT_SPHERES, 0x1001da30u, g3n_1001da30},
    {IAT_DOT, 0x1001b580u, fn_1001b580},          {IAT_INV_TRANSLATE, 0x1001ae70u, g3n_1001ae70},
    {IAT_MAGNITUDE, 0x10010c50u, fn_10010c50},
};
#define NIMPORTS (sizeof imports / sizeof imports[0])
static int hooked = -1; /* the import slot that holds HOOK in this case */

void recomp_call_ic(X86 *c, uint32_t target, uint64_t *ic) {
    (void)ic;
    if (target == HOOK && hooked >= 0) {
        imports[hooked].f(c);
        return;
    }
    for (unsigned i = 0; i < NIMPORTS; ++i)
        if (imports[i].target == target) {
            imports[i].f(c);
            return;
        }
    fprintf(stderr, "unexpected indirect call to %08x\n", target);
    abort();
}
void recomp_call(X86 *c, uint32_t target) {
    recomp_call_ic(c, target, NULL);
}

/* ---- inputs ------------------------------------------------------------------------------ */

static float odd_float(void) {
    return dt_float();
}
/* a coordinate: mostly in [-span, span], 1 in 32 an awkward value */
static float coord(float span) {
    if (dt_rand32() % 32u == 0)
        return odd_float();
    return ((float)(int32_t)dt_rand32() / 2147483648.0f) * span;
}
static float positive(float span) {
    if (dt_rand32() % 32u == 0)
        return odd_float();
    return ((float)(dt_rand32() >> 8) / 16777216.0f) * span;
}
static void wrf(uint32_t a, float f) {
    wrf32(a, f);
}
static void fill_random(uint32_t a, uint32_t n) {
    for (uint32_t k = 0; k < n; k += 4)
        wr32(a + k, dt_rand32());
}

/* a box {min, max} around a centre */
static void put_box(uint32_t a, float cx, float cy, float cz, float h) {
    const float hx = positive(h), hy = positive(h), hz = positive(h);
    wrf(a, cx - hx);
    wrf(a + 4u, cy - hy);
    wrf(a + 8u, cz - hz);
    wrf(a + 0xcu, cx + hx);
    wrf(a + 0x10u, cy + hy);
    wrf(a + 0x14u, cz + hz);
    if (dt_rand32() % 16u == 0)
        wrf(a + 4u * (dt_rand32() % 6u), odd_float());
}

static void put_sphere(uint32_t a, float cx, float cy, float cz, float r) {
    wrf(a, r);
    wrf(a + 4u, cx);
    wrf(a + 8u, cy);
    wrf(a + 0xcu, cz);
}

/* the filter: its sphere (+0x38), bounds (+0x48 min, +0x54 max), planes (+0x60), the box
 * cull's corner index bytes (+0xc0, +0xd2), the view direction (+0x28), its camera (+0xc) */
static void put_filter(void) {
    const unsigned mode = dt_rand32() % 4u;
    put_sphere(FILTER + 0x38u, coord(50), coord(50), coord(50), mode == 0 ? 1e4f : positive(120));
    put_box(FILTER + 0x48u, coord(40), coord(40), coord(40), mode == 1 ? 1e4f : 80.0f);
    for (uint32_t k = 0; k < 6u; ++k) {
        const uint32_t pl = FILTER + 0x60u + 0x10u * k;
        float nx = coord(1), ny = coord(1), nz = coord(1);
        wrf(pl, nx);
        wrf(pl + 4u, ny);
        wrf(pl + 8u, nz);
        wrf(pl + 0xcu, mode == 0 ? 1e4f : mode == 3 ? coord(10) : coord(80));
    }
    for (uint32_t k = 0; k < 18u; ++k) {
        wr8(FILTER + 0xc0u + k, (uint8_t)(dt_rand32() % 6u));
        wr8(FILTER + 0xd2u + k, (uint8_t)(dt_rand32() % 6u));
    }
    wrf(FILTER + 0x28u, coord(1));
    wrf(FILTER + 0x2cu, coord(1));
    wrf(FILTER + 0x30u, coord(1));
    if (dt_rand32() % 8u == 0) { /* the dot product exactly 0 */
        wrf(FILTER + 0x28u, 0.0f);
        wrf(FILTER + 0x2cu, dt_rand32() & 1 ? 0.0f : -0.0f);
        wrf(FILTER + 0x30u, 0.0f);
    }
    wr32(FILTER + 0xcu, FCAM);
    wrf(FCAM + 0x8cu, coord(50));
    wrf(FCAM + 0x90u, coord(50));
    wrf(FCAM + 0x94u, coord(50));
}

static void put_entity(void) {
    const float cx = coord(60), cy = coord(60), cz = coord(60);
    put_box(ENT + 0xe8u, cx, cy, cz, 30.0f);
    put_sphere(ENT + 0x100u, cx + coord(2), cy + coord(2), cz + coord(2), positive(40));
    put_sphere(ENT + 0xd8u, cx, cy, cz, dt_rand32() % 4u ? positive(0.01f) : positive(40));
    wrf(ENT + 0x70u, cx);
    wrf(ENT + 0x74u, cy);
    wrf(ENT + 0x78u, cz);
    wr16(ENT + 0x12u, dt_rand32() % 2u ? 0u : (uint16_t)dt_rand32());
    wrf(ENT + 0x128u, positive(4));
    wr32(ENT + 0x2cu, LODOBJ);
    wrf(LODOBJ + 0x48u, positive(4));
    wr32(ITEM + 0xcu, ENT);
    wr16(ITEM + 0x38u, (uint16_t)dt_rand32());
    wrf(ITEM, positive(1));
}

static void setup_world(void) {
    fill_random(DB, 0x300u);
    fill_random(ENT, 0x200u);
    fill_random(ITEM, 0x40u);
    fill_random(CAM, 0x100u);
    fill_random(FCAM, 0x100u);
    put_filter();
    put_entity();
    wr32(DB + 0x134u, CAM);
    wrf(CAM + 0x8cu, coord(80));
    wrf(CAM + 0x90u, coord(80));
    wrf(CAM + 0x94u, coord(80));
    wrf(DB + 0x1a4u, positive(2));
    /* FUN_10277ac0: enabled and built (or disabled now and then: EAX 0, the counter at 0+0x168) */
    wr8(0x10862a8cu, dt_rand32() % 32u ? 1u : 0u);
    wr32(0x10874bd4u, dt_rand32() % 8u ? rd32(0x10874bd4u) | 1u : rd32(0x10874bd4u) & ~1u);
    wr32(0x10874bd0u, APPOBJ);
    wr32(APP_LOG + 8u, 0u);
    /* a plane the sphere touches exactly: zero normal, d = +-r (ties in both comparisons) */
    if (dt_rand32() % 4u == 0) {
        const uint32_t pl = FILTER + 0x60u + 0x10u * (dt_rand32() % 6u);
        const float r = rdf32(ENT + 0x100u);
        wrf(pl, 0.0f);
        wrf(pl + 4u, 0.0f);
        wrf(pl + 8u, 0.0f);
        wrf(pl + 0xcu, dt_rand32() & 1 ? r : -r);
    }
    wr32(APPOBJ + 0x168u, dt_rand32());
    wr32(APPOBJ + 0x170u, dt_rand32());
    wr32(0x168u, 0u);
    wr32(0x170u, 0u);
    wrf(PI_VAR, dt_rand32() % 16u ? 3.14159274f : coord(4));
    wr32(IAT_PI, PI_VAR);
    for (unsigned i = 0; i < NIMPORTS; ++i)
        wr32(imports[i].slot, imports[i].target);
    hooked = -1;
    if (dt_rand32() % 64u == 0) {
        hooked = (int)(dt_rand32() % NIMPORTS);
        wr32(imports[hooked].slot, HOOK);
    }
    /* UpdateNodeData's function statics: initialised or not, the real masks or others */
    static const uint32_t guards[] = {0x1086de6cu, 0x1086de74u, 0x1086de64u, 0x1086dc14u, 0x1086dc1cu};
    static const uint32_t masks[] = {0x8000000u, 0x10000000u, 0x80000u, 0x100000u, 0x200000u};
    for (unsigned i = 0; i < 5; ++i) {
        const uint32_t g = dt_rand32() & ~1u;
        wr32(guards[i], dt_rand32() % 4u ? g | 1u : g);
        wr32(guards[i] + 4u, dt_rand32() % 8u ? masks[i] : dt_rand32());
    }
    uint32_t f = 0;
    for (unsigned i = 0; i < 5; ++i)
        if (dt_rand32() % 2u)
            f |= masks[i];
    wr32(FLAGS, f | (dt_rand32() & 0xe000000fu));
}

/* a thiscall: ECX = this, the arguments pushed right to left, a return address */
static void call_ctx(X86 *c, uint32_t self, const uint32_t *args, unsigned n) {
    dt_context(c, ESP0, dt_rand32() % 6u);
    c->r[1] = self;
    for (unsigned i = n; i-- > 0;) {
        c->r[4] -= 4u;
        wr32(c->r[4], args[i]);
    }
    c->r[4] -= 4u;
    wr32(c->r[4], 0x103ffa22u);
}

static unsigned vpt_paths[2], cam_res[3], fr_res[3], box_res[2], area_res[3], node_paths[4];

static unsigned res_index(uint32_t eax) {
    return eax == 0u ? 0u : eax == 1u ? 1u : 2u;
}

static void test_vpt(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        setup_world();
        const uint32_t a[] = {DB};
        call_ctx(&c, ITEM, a, 1);
        if (dt_run("CullVPTHC", &c, fn_104030c0, g3n_104030c0))
            vpt_paths[(c.r[0] & 0xffu) != 0]++;
    }
}

static void test_camera(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        setup_world();
        const uint32_t a[] = {ENT + 0xe8u, ENT + 0x100u};
        call_ctx(&c, FILTER, a, 2);
        if (dt_run("CullCamera", &c, fn_1040cb50, g3n_1040cb50))
            cam_res[res_index(c.r[0])]++;
    }
}

static void test_frustum(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        setup_world();
        const uint32_t a[] = {ENT + 0x100u};
        call_ctx(&c, FILTER, a, 1);
        if (dt_run("CullFrustum(sphere)", &c, fn_1040c940, g3n_1040c940))
            fr_res[res_index(c.r[0])]++;
    }
}

static void test_box(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        setup_world();
        const uint32_t a[] = {ENT + 0xe8u};
        call_ctx(&c, FILTER, a, 1);
        if (dt_run("CullCamera(box)", &c, fn_1040cba0, g3n_1040cba0))
            box_res[c.r[0] != 0xfu]++;
    }
}

static void test_area(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        setup_world();
        const uint32_t a[] = {ENT + 0x100u};
        call_ctx(&c, FILTER, a, 1);
        if (dt_run("CalculateAreaFactor", &c, fn_1040c5a0, g3n_1040c5a0)) {
            const double v = ST(&c, 0);
            area_res[v == 0.0 ? 0 : v == 1.0 ? 2 : 1]++;
        }
    }
}

static void test_node(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        setup_world();
        const uint32_t a[] = {DB, FLAGS};
        call_ctx(&c, ITEM, a, 2);
        if (dt_run("UpdateNodeData", &c, fn_10403390, g3n_10403390)) {
            const uint32_t f = rd32(FLAGS);
            node_paths[(f & 0x18000000u) ? 1 : 0]++;
            node_paths[2 + ((f & 0x180000u) == 0x180000u)]++;
        }
    }
}

static void test_lod(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        setup_world();
        const uint32_t a[] = {DB};
        call_ctx(&c, ITEM, a, 1);
        dt_run("CalculateLoDArea", &c, fn_10402f00, g3n_10402f00);
    }
}

/* With the group off the natives are the translation. */
static void test_off(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        setup_world();
        const uint32_t a[] = {DB};
        call_ctx(&c, ITEM, a, 1);
        X86 start = c, o;
        dt_save(0);
        g3_opt = 0;
        fn_104030c0(&c);
        o = c;
        dt_save(1);
        dt_restore();
        c = start;
        g3_opt = ~G3OPT_CULL;
        g3n_104030c0(&c);
        dt_compare("CullVPTHC (cull off)", &o, &c);
    }
    g3_opt = 0xffffffffu;
}

static double now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

/* G3N_BENCH=1: the same cases through each, ns per call (warm cache, -O1 as built here). */
static void bench_one(const char *name, void (*orig)(X86 *), void (*native)(X86 *), uint32_t self,
                      const uint32_t *args, unsigned na) {
    enum { NC = 128, REP = 2000 };
    static X86 start[NC];
    static uint8_t mem[NC][0x300 + 0x200 + 0x40 + 0x300];
    for (unsigned i = 0; i < NC; ++i) {
        do {
            setup_world();
        } while (hooked >= 0);
        call_ctx(&start[i], self, args, na);
        memcpy(mem[i], g_mem + DB, 0x300);
        memcpy(mem[i] + 0x300, g_mem + ENT, 0x200);
        memcpy(mem[i] + 0x500, g_mem + ITEM, 0x40);
        memcpy(mem[i] + 0x540, g_mem + STATICS, 0x300);
    }
    static volatile uint32_t sink;
    double t[3];
    for (int k = 0; k < 3; ++k) {
        g3_opt = k == 1 ? 0xffffffffu : 0u;
        const double t0 = now_ns();
        for (unsigned rep = 0; rep < REP; ++rep)
            for (unsigned i = 0; i < NC; ++i) {
                memcpy(g_mem + DB, mem[i], 0x300);
                memcpy(g_mem + ENT, mem[i] + 0x300, 0x200);
                memcpy(g_mem + ITEM, mem[i] + 0x500, 0x40);
                memcpy(g_mem + STATICS, mem[i] + 0x540, 0x300);
                X86 c = start[i];
                if (k < 2)
                    (k ? native : orig)(&c);
                sink += c.r[0];
            }
        t[k] = (now_ns() - t0) / (double)(REP * NC);
    }
    g3_opt = 0xffffffffu;
    printf("bench %s, ns per call less the restore (%.1f ns): translation %.1f, native %.1f\n", name, t[2],
           t[0] - t[2], t[1] - t[2]);
}

int main(void) {
    harness_mem();
    dt_load_pe("SharedBase.dll");
    dt_load_pe("Engine.dll");
    dt_stack_floor = ESP0 - 0x800u;
    dt_region(dt_stack_floor, 0x900u);
    dt_region(DB, 0x300u);
    dt_region(ENT, 0x200u);
    dt_region(ITEM, 0x40u);
    dt_region(CAM, 0x100u);
    dt_region(FCAM, 0x100u);
    dt_region(APPOBJ, 0x200u);
    dt_region(STATICS, 0x300u);
    dt_region(0x10874bc0u, 0x40u);
    dt_region(0x100u, 0x100u);
    dt_region(FLAGS, 0x10u);
    dt_region(APP_LOG, 0x10u);
    dt_region(0x100d0000u, 0x5000u); /* the CRT's data (sqrt) */

    test_frustum(200000);
    test_box(100000);
    test_camera(200000);
    test_vpt(300000);
    test_area(200000);
    test_node(200000);
    test_lod(50000);
    test_off(2000);

    printf("CullFrustum(sphere): 0 %u, 1 %u, 15 %u\n", fr_res[0], fr_res[1], fr_res[2]);
    printf("CullCamera(box): out %u, on to the planes %u\n", box_res[0], box_res[1]);
    printf("CullCamera: 0 %u, 1 %u, 15 %u\n", cam_res[0], cam_res[1], cam_res[2]);
    printf("CullVPTHC: false %u, true %u\n", vpt_paths[0], vpt_paths[1]);
    printf("CalculateAreaFactor: 0 %u, between %u, 1 %u\n", area_res[0], area_res[1], area_res[2]);
    printf("UpdateNodeData: no distance %u, distance %u; one sphere %u, entity sphere first %u\n", node_paths[0],
           node_paths[1], node_paths[2], node_paths[3]);
    if (!fr_res[0] || !fr_res[1] || !fr_res[2] || !box_res[0] || !box_res[1] || !cam_res[0] || !cam_res[1] ||
        !cam_res[2] || !vpt_paths[0] || !vpt_paths[1] || !area_res[0] || !area_res[1] || !area_res[2] ||
        !node_paths[0] || !node_paths[1] || !node_paths[2] || !node_paths[3]) {
        fprintf(stderr, "FAIL: a path never taken\n");
        ++dt_fail;
    }
    if (getenv("G3N_BENCH")) {
        const uint32_t a1[] = {DB}, a2[] = {DB, FLAGS};
        bench_one("CullVPTHC", fn_104030c0, g3n_104030c0, ITEM, a1, 1);
        bench_one("UpdateNodeData", fn_10403390, g3n_10403390, ITEM, a2, 2);
    }
    return dt_report("cull_test");
}
