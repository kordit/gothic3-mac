/* native/tests/particles_test.c - native particle functions against the translated Engine code.
 *
 * Build and run (from the checkout root):
 *   NATIVE="particles.c pdo.c" sh native/tests/build.sh particles aux-engine/chunk_103bc000.c \
 *       aux-engine/chunk_103c0000.c aux-engine/chunk_103ac000.c aux-engine/chunk_103b4000.c \
 *       aux-engine/chunk_103b0000.c
 * (pdo.c: CalcExtents and the vector scale wrappers TickParticles calls, group pdo.)
 * Without chunk_103b0000 (UpdateVertices, the particle copy, SpawnParticle) UpdateVertices is not
 * tested, and TickParticles runs against fakes of the copy and SpawnParticle, which then also
 * cover the spawn and removal call sites. Run it both ways. G3N_BENCH=1 adds timings.
 *
 * Engine.dll's sections come from the main repo's stage/ (constants such as 1.0f at 1068612c).
 * The scales get sorted key times most of the time and unsorted, repeated, NaN or empty ones
 * otherwise; t lands on a key, between keys, outside them, or is NaN. */
#include "difftest_rp.h"

/* The particle sort (DepthSortParticles) reaches Engine's qsort, which g3_overrides.h sends to the
 * shadow group's native g3n_10564ec0 (native/shadow.c, tested by shadow_test). Here it is the
 * translated qsort, so this test compares the particle code alone. */
void fn_10564ec0(X86 *c);
__attribute__((weak)) void g3n_10564ec0(X86 *c) {
    fn_10564ec0(c);
}

#include <math.h>
#include <time.h>

void fn_103bf2b0(X86 *c);
void fn_103c0140(X86 *c);
void fn_103af5d0(X86 *c);
void g3n_103bf2b0(X86 *c);
void g3n_103c0140(X86 *c);
void g3n_103af5d0(X86 *c);
void fn_103af790(X86 *c);
void g3n_103af790(X86 *c);

#define ESP0 0x0e800000u
#define RET_SITE 0x103af61bu
#define OBJ 0x03000000u   /* particle system / scale objects */
#define KEYS 0x03001000u  /* key arrays */
#define OUT 0x03002000u

/* A scale at `self`: {keys, count}, keys of `stride` bytes with `vals` floats after the time.
 * Returns a t worth asking about. */
static float make_scale(uint32_t self, uint32_t keys, uint32_t stride, unsigned vals) {
    int32_t count;
    switch (dt_rand32() % 10) {
    case 0: count = 0; break;
    case 1: count = -(int32_t)(dt_rand32() % 3) - 1; break;
    default: count = 1 + (int32_t)(dt_rand32() % 8); break;
    }
    wr32(self, keys);
    wr32(self + 4u, (uint32_t)count);
    const int n = count > 0 ? count : 1;
    const int sorted = dt_rand32() % 5 != 0;
    float tk = 0.0f;
    float times[16];
    for (int i = 0; i < n; ++i) {
        float k;
        if (sorted) {
            if (i == 0 || dt_rand32() % 6)
                tk += (float)(dt_rand32() % 1000) / 1000.0f; /* repeats when the step is 0 */
            k = tk;
        } else
            k = dt_float();
        times[i] = k;
        wrf32(keys + stride * (uint32_t)i, k);
        for (unsigned v = 0; v < vals; ++v)
            wrf32(keys + stride * (uint32_t)i + 4u + 4u * v, dt_rand32() % 6 ? (float)(dt_rand32() % 2000) / 1000.0f : dt_float());
    }
    switch (dt_rand32() % 6) {
    case 0: return times[dt_rand32() % (unsigned)n];
    case 1: return dt_float();
    case 2: return -1.0f;
    case 3: return tk + 1.0f;
    default: return (float)(dt_rand32() % 1000) / 1000.0f * (tk + 0.5f);
    }
}

static void frame2(X86 *c, float t, uint32_t out) {
    uint32_t tb;
    memcpy(&tb, &t, 4);
    c->r[4] -= 4;
    wr32(c->r[4], out);
    c->r[4] -= 4;
    wr32(c->r[4], tb);
    c->r[4] -= 4;
    wr32(c->r[4], RET_SITE);
}

static uint32_t some_out(void) {
    return OUT + 4u * (dt_rand32() % 8);
}

static void test_calc_value(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        const float t = make_scale(OBJ, KEYS, 20u, 4);
        dt_context(&c, ESP0, dt_rand32() % 4);
        c.r[1] = OBJ;
        frame2(&c, t, some_out());
        dt_run("CalcValue", &c, fn_103bf2b0, g3n_103bf2b0);
    }
}

static void test_calc_value_linear(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        const float t = make_scale(OBJ, KEYS, 16u, 3);
        dt_context(&c, ESP0, dt_rand32() % 4);
        c.r[1] = OBJ;
        frame2(&c, t, some_out());
        dt_run("CalcValueLinear", &c, fn_103c0140, g3n_103c0140);
    }
}

static void test_calc_color(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        float t = make_scale(OBJ + 0x3d4u, KEYS, 20u, 4);
        wr8(OBJ + 0x220u, dt_rand32() % 5 != 0);
        wrf32(OBJ + 0x224u, dt_rand32() % 4 ? (float)(dt_rand32() % 4000) / 1000.0f : dt_float());
        if (dt_rand32() % 2)
            t = (float)(dt_rand32() % 100000) / 1000.0f;
        dt_context(&c, ESP0, dt_rand32() % 3);
        c.r[1] = OBJ;
        frame2(&c, t, some_out());
        dt_run("CalcColor", &c, fn_103af5d0, g3n_103af5d0);
    }
}

/* A time near the fade times now and then, so every comparison goes both ways. */
static float near(float x) {
    switch (dt_rand32() % 4) {
    case 0: return x;
    case 1: return x + (float)(dt_rand32() % 1000) / 1000.0f;
    case 2: return x - (float)(dt_rand32() % 1000) / 1000.0f;
    default: return dt_rand32() % 3 ? (float)(dt_rand32() % 4000) / 1000.0f - 1.0f : dt_float();
    }
}

static void test_calc_color_factor(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        wr8(OBJ + 0x1ecu, dt_rand32() % 4 != 0);
        wr8(OBJ + 0x1edu, dt_rand32() % 4 != 0);
        const float in = dt_rand32() % 5 ? (float)(dt_rand32() % 2000) / 1000.0f : dt_float();
        const float fade = dt_rand32() % 5 ? (float)(dt_rand32() % 2000) / 1000.0f : dt_float();
        wrf32(OBJ + 0x1f0u, in);
        wrf32(OBJ + 0x1f4u, fade);
        for (uint32_t k = 0; k < 4; ++k) {
            wrf32(OBJ + 0x1fcu + 4u * k, dt_rand32() % 5 ? (float)(dt_rand32() % 1000) / 1000.0f : dt_float());
            wrf32(OBJ + 0x210u + 4u * k, dt_rand32() % 5 ? (float)(dt_rand32() % 1000) / 1000.0f : dt_float());
        }
        const float t = near(dt_rand32() & 1 ? in : fade);
        const float life = near(fade);
        uint32_t tb, lb;
        memcpy(&tb, &t, 4);
        memcpy(&lb, &life, 4);
        dt_context(&c, ESP0, dt_rand32() % 3);
        c.r[1] = OBJ;
        c.r[4] -= 4;
        wr32(c.r[4], some_out());
        c.r[4] -= 4;
        wr32(c.r[4], lb);
        c.r[4] -= 4;
        wr32(c.r[4], tb);
        c.r[4] -= 4;
        wr32(c.r[4], RET_SITE);
        dt_run("CalcColorFactor", &c, fn_103af790, g3n_103af790);
    }
}

/* ---------------------------------------------------------------- TickParticles
 *
 * Everything TickParticles calls that is not one of the particle functions (the SharedBase
 * imports, the virtual calls, TraceRayNearestHit, SetEntity, the particle copy, SetCount and
 * SpawnParticle) is a fake here that records how it was called: every register, the return
 * address and arguments, the x87 control, status and tag words and live registers, the XMM
 * registers, and a hash of the stack from ESP up and of the particle memory. The original's calls
 * and the native ones must come out the same, one for one. The fakes do roughly what the real
 * functions do (copy, scale, translate, normalise, reflect; a hit or not; a spawned particle or
 * not), decided by a hash of what they were given, and clobber EAX, ECX, EDX, XMM0-1, the x87
 * tags below the stack and IE the way a real callee may. The particle functions themselves
 * (CalcVelocityFactor, CalcRevolutionFactor, the vector scale's CalcValue, CalcFrame, CalcColor,
 * CalcColorFactor, PlayCollisionSound) run as they are, translated or native. */

void fn_103b5800(X86 *c);
void g3n_103b5800(X86 *c);

#define PS 0x03100000u       /* eCParticle_PS */
#define PARTS 0x03101000u    /* its particle array */
#define TKEYS 0x03102000u    /* key arrays of its scales, 0x200 each */
#define SUBPS 0x03104000u    /* the particle system a hit spawns into */
#define SUBPARTS 0x03104800u
#define ENTITY 0x03105000u
#define VTABLES 0x03106000u
#define VT_PS (VTABLES + 0x000u)
#define VT_ENTITY (VTABLES + 0x200u)
#define VT_PROXY (VTABLES + 0x400u)
#define SCENE 0x03107000u
#define REPORT 0x03107100u
#define DEGTORAD 0x03107200u
#define ENTITY_PTR 0x03107300u /* what [PS+0x2dc] points at */
#define TICK_RET 0x103b568au   /* DoProcessV2's return site after CALL TickParticles */
#define FAKE 0x0f000000u       /* fake call targets, 16 bytes apart */
/* UpdateVertices' world */
#define UV_WORLD 0x03108000u   /* [10862a88]: +0x5e8 the frame number, +0x34 the camera object */
#define UV_CAMOBJ 0x03108800u
#define UV_RENDER 0x03109000u
#define UV_CAMERA 0x03109800u  /* its matrix at +0x14 */
#define UV_VBUF 0x0310a000u    /* the locked vertex data, 0xc0 bytes a particle */
#define UV_ZERO 0x0310b000u
#define UV_UP 0x0310b010u
#define UV_OBJ398 0x0310b100u  /* [PS+0x398]: +0xc the primitive holder */
#define UV_OBJC 0x0310b200u
#define UV_PRIM 0x0310b300u
#define UV_VDATA 0x0310b400u   /* [PS+0x39c] */
#define VT_OBJC (VTABLES + 0x600u)

enum {
    F_SETVECTOR, F_SCALE_V, F_GETSCALED, F_TRANSLATE, F_INVTRANSLATE, F_R3RANDOM, F_R3RANDOM_TO, F_SETTOROT,
    F_MULM3, F_GETMAG, F_GETINVTR, F_NORMALIZE, F_ISVALID, F_RANDINT, F_REFLECT, F_SCALE_F, F_GETSQMAG,
    F_PS_ENTITY, F_PROXY_GET, F_PROPSET,
    /* UpdateVertices' */
    F_MATASSIGN, F_MATINV, F_GETZERO, F_FCOLOR_CTOR, F_FCOLOR_SET, F_GETINVERTED, F_TRANSFORMDIR, F_GETUP,
    F_ISEQUAL, F_SETCOORDS, F_SETCROSS, F_MATINV2, F_MULM4, F_GETTRANSLATED, F_AAROT, F_GETNORMALIZED, F_GETDOT,
    F_BCOLOR_CTOR, F_BCOLOR_ARGB, F_FCOLOR_CLAMP, F_GETPRIM, F_NIMPORTS
};
static const uint32_t fake_iat[F_NIMPORTS] = {
    [F_SETVECTOR] = 0x105f7158u,   [F_SCALE_V] = 0x105f6c28u,      [F_GETSCALED] = 0x105f6748u,
    [F_TRANSLATE] = 0x105f67b4u,   [F_INVTRANSLATE] = 0x105f66ecu, [F_R3RANDOM] = 0x105f6d54u,
    [F_R3RANDOM_TO] = 0x105f6d4cu, [F_SETTOROT] = 0x105f6d20u,     [F_MULM3] = 0x105f6d30u,
    [F_GETMAG] = 0x105f67c0u,      [F_GETINVTR] = 0x105f6754u,     [F_NORMALIZE] = 0x105f6904u,
    [F_ISVALID] = 0x105f6f70u,     [F_RANDINT] = 0x105f68c4u,      [F_REFLECT] = 0x105f6d28u,
    [F_SCALE_F] = 0x105f6684u,     [F_GETSQMAG] = 0x105f677cu,     [F_MATASSIGN] = 0x105f7180u,
    [F_MATINV] = 0x105f68a8u,      [F_GETZERO] = 0x105f6d34u,      [F_FCOLOR_CTOR] = 0x105f6a7cu,
    [F_FCOLOR_SET] = 0x105f6a70u,  [F_GETINVERTED] = 0x105f68b0u,  [F_TRANSFORMDIR] = 0x105f684cu,
    [F_GETUP] = 0x105f6d38u,       [F_ISEQUAL] = 0x105f686cu,      [F_SETCOORDS] = 0x105f649cu,
    [F_SETCROSS] = 0x105f67f4u,    [F_MATINV2] = 0x105f6810u,      [F_MULM4] = 0x105f6728u,
    [F_GETTRANSLATED] = 0x105f6774u, [F_AAROT] = 0x105f6d3cu,      [F_GETNORMALIZED] = 0x105f6870u,
    [F_GETDOT] = 0x105f6854u,      [F_BCOLOR_CTOR] = 0x105f6bc4u,  [F_BCOLOR_ARGB] = 0x105f6adcu,
    [F_FCOLOR_CLAMP] = 0x105f6c4cu,
};
enum {
    F_TRACE = 64, F_SETENTITY, F_COPY, F_SETCOUNT, F_SPAWN,
    F_RENDER, F_CAMERA, F_LOCK, F_UNLOCK, F_DEAD, F_HIDDEN, F_SCALED, F_FLOOR, F_CRT_CEIL, F_QSORT
};

#define LOG_MAX 4096
typedef struct {
    uint32_t what, r[8], ret, args[8];
    uint32_t top, tag, sw, cw;
    uint32_t xmm[8][4];
    double st[8];
    uint64_t stack_hash, mem_hash;
} CallRec;
static CallRec log_o[LOG_MAX], log_n[LOG_MAX];
static unsigned nlog_o, nlog_n;

static uint64_t fnv(uint64_t h, uint32_t a, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) {
        h ^= g_mem[a + i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static uint64_t case_salt; /* drawn per case, the same for both runs */

/* Records the call in the log of the run in progress (g3_opt is 0 for the original) and returns
 * the fake's decisions: a hash of the case, the call's place in the run and what it was given. */
static int fakes_quiet; /* the timing: no record, a cheap decision */
static uint32_t log_call(X86 *c, uint32_t what) {
    if (fakes_quiet) /* everything there, alive and visible; no hit */
        return 1u;
    const unsigned idx = g3_opt ? nlog_n : nlog_o;
    CallRec *L = g3_opt ? &log_n[nlog_n < LOG_MAX - 1 ? nlog_n++ : LOG_MAX - 1]
                        : &log_o[nlog_o < LOG_MAX - 1 ? nlog_o++ : LOG_MAX - 1];
    memset(L, 0, sizeof *L);
    L->what = what;
    memcpy(L->r, c->r, sizeof L->r);
    L->ret = rd32(c->r[4]);
    for (int i = 0; i < 8; ++i)
        L->args[i] = rd32(c->r[4] + 4u + 4u * (uint32_t)i);
    L->top = c->fpu_top;
    L->tag = c->fpu_tag;
    L->sw = c->fpu_sw;
    L->cw = c->fpu_cw;
    memcpy(L->xmm, c->xmm, sizeof L->xmm);
    for (unsigned p = 0; p < 8; ++p)
        if (((c->fpu_tag >> (2 * p)) & 3u) != FTAG_EMPTY)
            L->st[p] = c->st[p];
    L->stack_hash = c->r[4] < ESP0 + 0x10u ? fnv(0xcbf29ce484222325ull, c->r[4], ESP0 + 0x10u - c->r[4]) : 0;
    uint64_t h = fnv(0xcbf29ce484222325ull, PS, 0x400u);
    h = fnv(h, PARTS, 0xb4u * 12u);
    h = fnv(h, SUBPARTS, 0xb4u * 4u);
    h = fnv(h, UV_VBUF, 0xc0u * 12u);
    L->mem_hash = h;
    uint64_t d = case_salt ^ what ^ ((uint64_t)idx << 40);
    d = fnv(d, c->r[4] + 4u, 12u);
    d ^= c->r[1] * 0x2545f4914f6cdd1dull;
    if (c->r[1] >= 0x03000000u && c->r[1] < 0x03200000u)
        d = fnv(d, c->r[1], 12u);
    d ^= d >> 29;
    d *= 0xbf58476d1ce4e5b9ull;
    d ^= d >> 32;
    return (uint32_t)d;
}

/* A callee's leftovers: EAX ECX EDX, XMM0-1 now and then, x87 slots below the stack it used, IE. */
static void clobber(X86 *c, uint32_t h, uint32_t eax) {
    c->r[0] = eax;
    c->r[1] = h ^ 0x51ed2701u;
    c->r[2] = h * 0x9e3779b1u;
    if (h & 0x100u) {
        c->xmm[0][0] = h;
        c->xmm[0][1] = ~h;
        c->xmm[1][0] = h >> 3;
    }
    for (unsigned i = 1; i <= ((h >> 9) & 3u); ++i)
        ftag_put(c, (c->fpu_top - i) & 7u, FTAG_EMPTY);
    if ((h & 0x1800u) == 0x1800u)
        c->fpu_sw |= 0x0001u;
}
static void ret_n(X86 *c, uint32_t n) {
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u + n;
}
static float hf(uint32_t h, int k) {
    return (float)((int32_t)((h >> (k * 5)) & 0x3ffu) - 0x200) / 64.0f;
}
static void vset(uint32_t a, float x, float y, float z) {
    wrf32(a, x);
    wrf32(a + 4u, y);
    wrf32(a + 8u, z);
}

static void fake_import(X86 *c, unsigned f) {
    const uint32_t h = log_call(c, f);
    const uint32_t self = c->r[1], a1 = rd32(c->r[4] + 4u), a2 = rd32(c->r[4] + 8u);
    uint32_t eax = h, n = 4;
    switch (f) {
    case F_SETVECTOR:
        vset(self, rdf32(a1), rdf32(a1 + 4u), rdf32(a1 + 8u));
        eax = self;
        break;
    case F_SCALE_V:
        vset(self, rdf32(self) * rdf32(a1), rdf32(self + 4u) * rdf32(a1 + 4u), rdf32(self + 8u) * rdf32(a1 + 8u));
        break;
    case F_GETSCALED: {
        const float s = rdf32(c->r[4] + 8u);
        vset(a1, rdf32(self) * s, rdf32(self + 4u) * s, rdf32(self + 8u) * s);
        eax = a1;
        n = 8;
        break;
    }
    case F_TRANSLATE:
        vset(self, rdf32(self) + rdf32(a1), rdf32(self + 4u) + rdf32(a1 + 4u), rdf32(self + 8u) + rdf32(a1 + 8u));
        break;
    case F_INVTRANSLATE:
        vset(self, rdf32(self) - rdf32(a1), rdf32(self + 4u) - rdf32(a1 + 4u), rdf32(self + 8u) - rdf32(a1 + 8u));
        break;
    case F_R3RANDOM:
    case F_R3RANDOM_TO:
        vset(a1, hf(h, 0) / 8.0f + 1.0f, hf(h, 1) / 8.0f + 1.0f, hf(h, 2) / 8.0f + 1.0f);
        if (f == F_R3RANDOM)
            eax = a1;
        break;
    case F_SETTOROT:
        for (uint32_t k = 0; k < 9u; ++k)
            wrf32(self + 4u * k, (k % 4u == 0u ? 1.0f : 0.0f) + rdf32(a1 + 4u * (k % 3u)) / 16.0f);
        break;
    case F_MULM3: {
        const float x = rdf32(self), y = rdf32(self + 4u), z = rdf32(self + 8u);
        vset(self, x * rdf32(a1) + y * rdf32(a1 + 12u) + z * rdf32(a1 + 24u),
             x * rdf32(a1 + 4u) + y * rdf32(a1 + 16u) + z * rdf32(a1 + 28u),
             x * rdf32(a1 + 8u) + y * rdf32(a1 + 20u) + z * rdf32(a1 + 32u));
        break;
    }
    case F_GETMAG:
    case F_GETSQMAG: {
        const float x = rdf32(self), y = rdf32(self + 4u), z = rdf32(self + 8u);
        const float q = x * x + y * y + z * z;
        clobber(c, h, h);
        fpush(c, (double)(f == F_GETMAG ? sqrtf(q) : q));
        ret_n(c, 0);
        return;
    }
    case F_GETINVTR:
        vset(a1, rdf32(self) - rdf32(a2), rdf32(self + 4u) - rdf32(a2 + 4u), rdf32(self + 8u) - rdf32(a2 + 8u));
        eax = a1;
        n = 8;
        break;
    case F_NORMALIZE: {
        const float x = rdf32(self), y = rdf32(self + 4u), z = rdf32(self + 8u);
        const float l = sqrtf(x * x + y * y + z * z);
        if (l > 0.0f)
            vset(self, x / l, y / l, z / l);
        eax = self;
        n = 0;
        break;
    }
    case F_ISVALID:
        eax = (h & 0xffffff00u) | ((h >> 13) & 1u);
        n = 0;
        break;
    case F_RANDINT:
        eax = (uint32_t)((int32_t)(h % 5u) - 1);
        n = 0;
        break;
    case F_REFLECT: {
        const float x = rdf32(self), y = rdf32(self + 4u), z = rdf32(self + 8u);
        const float nx = rdf32(a2), ny = rdf32(a2 + 4u), nz = rdf32(a2 + 8u);
        const float d = 2.0f * (x * nx + y * ny + z * nz);
        vset(a1, x - d * nx, y - d * ny, z - d * nz);
        eax = a1;
        n = 8;
        break;
    }
    case F_SCALE_F: {
        const float s = rdf32(c->r[4] + 4u);
        vset(self, rdf32(self) * s, rdf32(self + 4u) * s, rdf32(self + 8u) * s);
        break;
    }
    case F_PS_ENTITY:
        eax = ENTITY;
        n = 0;
        break;
    case F_PROXY_GET:
        eax = (h & 3u) ? ENTITY : 0u;
        n = 0;
        break;
    case F_PROPSET:
        eax = (h % 3u) ? SUBPS : 0u;
        break;
    /* UpdateVertices' */
    case F_MATASSIGN:
        memmove(g_mem + self, g_mem + a1, 64);
        eax = self;
        break;
    case F_MATINV:
    case F_MATINV2: /* the transpose, the translation negated */
        for (uint32_t i = 0; i < 4u; ++i)
            for (uint32_t j = 0; j < 4u; ++j)
                wrf32(a1 + 16u * i + 4u * j, i < 3u && j < 3u ? rdf32(self + 16u * j + 4u * i)
                                                             : i == 3u && j < 3u ? -rdf32(self + 48u + 4u * j)
                                                                                 : rdf32(self + 16u * i + 4u * j));
        eax = a1;
        break;
    case F_GETZERO:
        eax = UV_ZERO;
        n = 0;
        break;
    case F_GETUP:
        eax = UV_UP;
        n = 0;
        break;
    case F_FCOLOR_CTOR:
        wr32(self, 0x0f800000u);
        eax = self;
        n = 0;
        break;
    case F_FCOLOR_SET:
        for (uint32_t k = 0; k < 16u; k += 4u)
            wr32(self + 4u + k, rd32(c->r[4] + 4u + k));
        n = 16;
        break;
    case F_GETINVERTED:
        vset(a1, -rdf32(self), -rdf32(self + 4u), -rdf32(self + 8u));
        eax = a1;
        break;
    case F_TRANSFORMDIR:
    case F_MULM4: {
        const float x = rdf32(self), y = rdf32(self + 4u), z = rdf32(self + 8u);
        const float t = f == F_MULM4 ? 1.0f : 0.0f;
        vset(self, x * rdf32(a1) + y * rdf32(a1 + 16u) + z * rdf32(a1 + 32u) + t * rdf32(a1 + 48u),
             x * rdf32(a1 + 4u) + y * rdf32(a1 + 20u) + z * rdf32(a1 + 36u) + t * rdf32(a1 + 52u),
             x * rdf32(a1 + 8u) + y * rdf32(a1 + 24u) + z * rdf32(a1 + 40u) + t * rdf32(a1 + 56u));
        break;
    }
    case F_ISEQUAL:
        eax = (h & 0xffffff00u) | (memcmp(g_mem + self, g_mem + a1, 12) == 0 || (h & 7u) == 0);
        break;
    case F_SETCOORDS:
        for (uint32_t k = 0; k < 12u; k += 4u)
            wr32(self + k, rd32(c->r[4] + 4u + k));
        eax = self;
        n = 12;
        break;
    case F_SETCROSS: {
        const float ax = rdf32(a1), ay = rdf32(a1 + 4u), az = rdf32(a1 + 8u);
        const float bx = rdf32(a2), by = rdf32(a2 + 4u), bz = rdf32(a2 + 8u);
        vset(self, ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx);
        eax = self;
        n = 8;
        break;
    }
    case F_GETTRANSLATED:
        vset(a1, rdf32(self) + rdf32(a2), rdf32(self + 4u) + rdf32(a2 + 4u), rdf32(self + 8u) + rdf32(a2 + 8u));
        eax = a1;
        n = 8;
        break;
    case F_AAROT: { /* a rotation-ish matrix from the axis and the angle */
        const float s = rdf32(a1 + 12u) / 8.0f;
        for (uint32_t k = 0; k < 9u; ++k)
            wrf32(self + 4u * k, (k % 4u == 0u ? 1.0f : 0.0f) + s * rdf32(a1 + 4u * (k % 3u)));
        break;
    }
    case F_GETNORMALIZED: {
        const float x = rdf32(self), y = rdf32(self + 4u), z = rdf32(self + 8u);
        const float l = sqrtf(x * x + y * y + z * z);
        if (l > 0.0f)
            vset(a1, x / l, y / l, z / l);
        else
            vset(a1, x, y, z);
        eax = a1;
        break;
    }
    case F_GETDOT: {
        const float d = rdf32(self) * rdf32(a1) + rdf32(self + 4u) * rdf32(a1 + 4u) + rdf32(self + 8u) * rdf32(a1 + 8u);
        clobber(c, h, h);
        fpush(c, (double)d);
        ret_n(c, 4);
        return;
    }
    case F_BCOLOR_CTOR:
        wr32(self, 0x0f800100u);
        for (uint32_t k = 0; k < 4u; ++k) {
            const float v = rdf32(a1 + 4u + 4u * k);
            wr8(self + 4u + k, v != v ? 0 : v <= 0.0f ? 0 : v >= 1.0f ? 255 : (uint8_t)(v * 255.0f));
        }
        eax = self;
        break;
    case F_BCOLOR_ARGB:
        eax = rd32(self + 4u);
        n = 0;
        break;
    case F_FCOLOR_CLAMP:
        for (uint32_t k = 4u; k <= 16u; k += 4u) {
            const float v = rdf32(self + k);
            wrf32(self + k, v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v);
        }
        n = 0;
        break;
    case F_GETPRIM:
        eax = UV_PRIM;
        n = 0;
        break;
    }
    clobber(c, h, eax);
    ret_n(c, n);
}

/* UpdateVertices' Engine callees that are not particle code */
void fn_10277b80(X86 *c) { /* the render module, or 0 */
    const uint32_t h = log_call(c, F_RENDER);
    clobber(c, h, h % 16u ? UV_RENDER : 0u);
    ret_n(c, 0);
}
void fn_102ec7c0(X86 *c) { /* GetActiveCamera(&camera), RET 4 */
    const uint32_t h = log_call(c, F_CAMERA);
    wr32(rd32(c->r[4] + 4u), h % 16u ? UV_CAMERA : 0u);
    clobber(c, h, h);
    ret_n(c, 4);
}
void fn_102f6cb0(X86 *c) { /* LockBuffer(0, 0, flags, &data), RET 0x10 */
    const uint32_t h = log_call(c, F_LOCK);
    wr32(rd32(c->r[4] + 16u), h % 16u ? UV_VBUF : 0u);
    clobber(c, h, 1u);
    ret_n(c, 16);
}
void fn_102f6cf0(X86 *c) { /* UnlockBuffer */
    const uint32_t h = log_call(c, F_UNLOCK);
    clobber(c, h, h);
    ret_n(c, 0);
}
void fn_103abc20(X86 *c) { /* the particle at ECX is dead */
    const uint32_t h = log_call(c, F_DEAD);
    clobber(c, h, (h & 0xffffff00u) | (h % 6u == 0u));
    ret_n(c, 0);
}
void fn_103abc50(X86 *c) { /* the particle at ESI is invisible */
    const uint32_t h = log_call(c, F_HIDDEN);
    clobber(c, h, (h & 0xffffff00u) | (h % 7u == 0u));
    ret_n(c, 0);
}
/* FUN_102995d0 (the override header's name for it): [ESI] = [EAX] * f, EAX = ESI, RET 4 */
void g3n_102995d0(X86 *c) {
    const uint32_t h = log_call(c, F_SCALED);
    const uint32_t src = c->r[0], out = c->r[6];
    const float f = rdf32(c->r[4] + 4u);
    vset(out, rdf32(src) * f, rdf32(src + 4u) * f, rdf32(src + 8u) * f);
    clobber(c, h, out);
    ret_n(c, 4);
}
void fn_10379d20(X86 *c) { /* floor(f) in ST0, RET 4 */
    const uint32_t h = log_call(c, F_FLOOR);
    const float f = rdf32(c->r[4] + 4u);
    clobber(c, h, h);
    fpush(c, (double)floorf(f));
    ret_n(c, 4);
}
void fn_10565640(X86 *c) { /* the CRT's ceil(double) in ST0, cdecl */
    const uint32_t h = log_call(c, F_CRT_CEIL);
    const double d = rdf64(c->r[4] + 4u);
    clobber(c, h, h);
    fpush(c, ceil(d));
    ret_n(c, 0);
}
void fn_10564ec0(X86 *c) { /* qsort, cdecl: the order is left as it is */
    const uint32_t h = log_call(c, F_QSORT);
    clobber(c, h, h);
    ret_n(c, 0);
}

/* TraceRayNearestHit(origin, direction, entity, flags, 0, 0, 0), RET 0x1c: a hit half the time. */
void fn_103cd980(X86 *c) {
    const uint32_t h = log_call(c, F_TRACE);
    wr32(c->r[1] + 0xacu, REPORT);
    wr8(REPORT, (h >> 3) & 1u);
    for (uint32_t k = 0; k < 6u; ++k)
        wrf32(REPORT + 0x68u + 4u * k, hf(h, (int)k));
    clobber(c, h, (h >> 3) & 1u);
    ret_n(c, 0x1cu);
}
/* eCEntityProxy::SetEntity(entity), RET 4 */
void fn_1048e500(X86 *c) {
    const uint32_t h = log_call(c, F_SETENTITY);
    wr32(ENTITY_PTR + 0x10u, rd32(c->r[4] + 4u) ^ c->r[1]);
    clobber(c, h, h);
    ret_n(c, 4);
}
/* The particle copy and SpawnParticle are in chunk_103b0000, with UpdateVertices: when that chunk
 * is linked, the real ones run (SpawnParticle then finds the spawning system full and returns -1). */
static int fakes_103b0000;
/* the particle copy: [EAX] = [ECX] */
__attribute__((weak)) void fn_103b0330(X86 *c) {
    fakes_103b0000 = 1;
    const uint32_t h = log_call(c, F_COPY);
    if (c->r[0] != c->r[1])
        memmove(g_mem + c->r[0], g_mem + c->r[1], 0xb4u);
    clobber(c, h, h);
    ret_n(c, 0);
}
/* SetCount(EDX) of the array at EDI, which is large enough */
void fn_103ba810(X86 *c) {
    const uint32_t h = log_call(c, F_SETCOUNT);
    clobber(c, h, h);
    ret_n(c, 0);
}
/* SpawnParticle(&pos), RET 4: an index into SUBPARTS, or -1 */
__attribute__((weak)) void fn_103b0480(X86 *c) {
    const uint32_t h = log_call(c, F_SPAWN);
    const int32_t i = (int32_t)(h % 4u) - 1;
    if (i >= 0)
        for (uint32_t k = 0; k < 12u; k += 4u)
            wr32(SUBPARTS + 0xb4u * (uint32_t)i + 0xcu + k, rd32(rd32(c->r[4] + 4u) + k));
    clobber(c, h, (uint32_t)i);
    ret_n(c, 4);
}

void recomp_call_ic(X86 *c, uint32_t target, uint64_t *ic) {
    (void)ic;
    if (target >= FAKE && target < FAKE + 16u * F_NIMPORTS && (target & 15u) == 0) {
        fake_import(c, (target - FAKE) / 16u);
        return;
    }
    fprintf(stderr, "unexpected indirect call to %08x\n", target);
    abort();
}

/* A game-range float, now and then (1 in `odd`) an awkward one. */
static float rf(unsigned odd) {
    return dt_rand32() % odd ? (float)((int32_t)(dt_rand32() % 4001u) - 2000) / 1000.0f : dt_float();
}
static void rvec(uint32_t a, unsigned odd) {
    for (uint32_t k = 0; k < 12u; k += 4u)
        wrf32(a + k, rf(odd));
}

static void tick_setup(void) {
    for (unsigned f = 0; f < F_NIMPORTS; ++f)
        if (fake_iat[f])
            wr32(fake_iat[f], FAKE + 16u * f);
    wr32(0x105f7178u, DEGTORAD);
    wrf32(DEGTORAD, 0.017453292f);
    wr32(VT_PS + 0x68u, FAKE + 16u * F_PS_ENTITY);
    wr32(VT_PROXY, FAKE + 16u * F_PROXY_GET);
    wr32(VT_ENTITY + 0xecu, FAKE + 16u * F_PROPSET);
    wr32(ENTITY, VT_ENTITY);
    wr32(0x10862dccu, SCENE);
    wr32(SUBPS + 0x318u, SUBPARTS);
    wr32(SUBPS + 0x31cu, 4u); /* full: the real SpawnParticle returns -1 */
    wr32(SUBPS + 0x320u, 4u);
    /* UpdateVertices' world */
    wr32(0x105f6a74u, 0x0f800000u); /* the colour vftables the imports point at */
    wr32(0x105f63ecu, 0x0f800100u);
    wr32(0x10862a88u, UV_WORLD);
    wr32(UV_WORLD + 0x34u, UV_CAMOBJ);
    wr32(UV_OBJ398 + 0xcu, UV_OBJC);
    wr32(UV_OBJC, VT_OBJC);
    wr32(VT_OBJC + 0x30u, FAKE + 16u * F_GETPRIM);
    vset(UV_UP, 0.0f, 1.0f, 0.0f);
}

static void tick_case(X86 *c) {
    for (uint32_t k = 0; k < 0x400u; k += 4u)
        wr32(PS + k, dt_rand32() % 3 ? dt_rand32() : 0u);
    wr32(PS, VT_PS);
    /* the flags, mostly 0 or 1, sometimes another byte */
    static const uint32_t flags[] = {0x1cu,  0xd0u,  0x100u, 0x160u, 0x194u, 0x1b8u, 0x1e4u,
                                     0x1e6u, 0x220u, 0x1ecu, 0x1edu, 0x284u, 0x285u, 0x2a8u,
                                     0x2d4u, 0x2d5u, 0x2d6u, 0x2d7u, 0x2fcu};
    for (unsigned i = 0; i < sizeof flags / sizeof flags[0]; ++i)
        wr8(PS + flags[i], dt_rand32() % 8 ? (uint8_t)(dt_rand32() & 1u) : (uint8_t)dt_rand32());
    wr32(PS + 0x84u, dt_rand32() % 3);
    rvec(PS + 0xb8u, 6);
    for (uint32_t k = 0xc4u; k <= 0xccu; k += 4u)
        wrf32(PS + k, dt_rand32() % 3 ? 0.0f : rf(6));
    wrf32(PS + 0xd4u, rf(6));
    wrf32(PS + 0x198u, rf(6));
    wrf32(PS + 0x1bcu, rf(6));
    wrf32(PS + 0x1e8u, rf(6));
    for (uint32_t k = 0x1d4u; k <= 0x1e0u; k += 4u)
        wr32(PS + k, dt_rand32() % 6);
    wrf32(PS + 0x224u, rf(6));
    wrf32(PS + 0x1f0u, rf(6));
    wrf32(PS + 0x1f4u, rf(6));
    for (uint32_t k = 0; k < 4u; ++k) {
        wrf32(PS + 0x1fcu + 4u * k, rf(6));
        wrf32(PS + 0x210u + 4u * k, rf(6));
    }
    wr32(PS + 0x254u, 0u);
    wrf32(PS + 0x330u, dt_rand32() & 1 ? 1.0f : 0.0f); /* PlayCollisionSound returns at once */
    rvec(PS + 0x2c4u, 6);
    wrf32(PS + 0x2d0u, dt_rand32() % 3 ? (float)(dt_rand32() % 3000u) / 1000.0f : rf(6));
    wr32(PS + 0x2d8u, VT_PROXY);
    switch (dt_rand32() % 3) {
    case 0:
        wr32(PS + 0x2dcu, 0u);
        break;
    case 1:
        wr32(PS + 0x2dcu, ENTITY_PTR);
        wr32(ENTITY_PTR, 0u);
        break;
    default:
        wr32(PS + 0x2dcu, ENTITY_PTR);
        wr32(ENTITY_PTR, ENTITY);
        break;
    }
    (void)make_scale(PS + 0x3c8u, TKEYS, 16u, 3);
    (void)make_scale(PS + 0x3bcu, TKEYS + 0x200u, 16u, 3);
    (void)make_scale(PS + 0x3e0u, TKEYS + 0x400u, 16u, 3);
    (void)make_scale(PS + 0x3b0u, TKEYS + 0x600u, 8u, 1);
    (void)make_scale(PS + 0x3d4u, TKEYS + 0x800u, 20u, 4);
    int32_t n;
    switch (dt_rand32() % 12) {
    case 0: n = 0; break;
    case 1: n = -1; break;
    default: n = 1 + (int32_t)(dt_rand32() % 8); break;
    }
    wr32(PS + 0x318u, PARTS);
    wr32(PS + 0x31cu, (uint32_t)n);
    wr32(PS + 0x320u, 16u);
    for (uint32_t i = 0; i < 12u; ++i) {
        const uint32_t p = PARTS + 0xb4u * i;
        for (uint32_t k = 0; k < 0xa0u; k += 4u)
            wrf32(p + k, rf(8));
        wrf32(p + 0xa0u, (float)(dt_rand32() % 2000u) / 1000.0f);
        wrf32(p + 0xa4u, dt_rand32() % 6 ? (float)(dt_rand32() % 3000u) / 1000.0f : dt_rand32() & 1 ? 0.0f : rf(4));
        wrf32(p + 0xa8u, rf(6));
        wr32(p + 0xacu, dt_rand32() % 4);
        wr32(p + 0xb0u, dt_rand32() % 8 ? 0u : 1u);
    }
    for (uint32_t k = 0; k < 0xb4u * 4u; k += 4u)
        wrf32(SUBPARTS + k, rf(8));
    const float dt = dt_rand32() % 8 ? (float)(dt_rand32() % 100u) / 1000.0f : rf(4);
    uint32_t dtb;
    memcpy(&dtb, &dt, 4);
    dt_context(c, ESP0 - 4u * (dt_rand32() % 4), dt_rand32() % 3);
    c->r[1] = PS;
    c->r[4] -= 4;
    wr32(c->r[4], dtb);
    c->r[4] -= 4;
    wr32(c->r[4], TICK_RET);
}

/* The calls of one run of each, compared one for one; what the fakes and call sites saw is
 * counted in *cov. */
typedef struct {
    unsigned long calls, seen[128];
    uint32_t sites[256]; /* return addresses seen */
    unsigned nsites;
} Coverage;

static void run_logged(const char *name, X86 *c, void (*orig)(X86 *), void (*native)(X86 *), Coverage *cov) {
    case_salt = ((uint64_t)dt_rand32() << 32) ^ dt_rand32();
    nlog_o = nlog_n = 0;
    const int ok = dt_run(name, c, orig, native);
    cov->calls += nlog_o;
    for (unsigned k = 0; k < nlog_o; ++k) {
        ++cov->seen[log_o[k].what & 127u];
        unsigned s = 0;
        while (s < cov->nsites && cov->sites[s] != log_o[k].ret)
            ++s;
        if (s == cov->nsites && cov->nsites < 256)
            cov->sites[cov->nsites++] = log_o[k].ret;
    }
    int bad = nlog_o != nlog_n;
    unsigned k = 0;
    for (; k < nlog_o && k < nlog_n; ++k)
        if (memcmp(&log_o[k], &log_n[k], sizeof log_o[k])) {
            bad = 1;
            break;
        }
    if (!bad)
        return;
    if (ok)
        ++dt_fail;
    if (dt_fail >= 20)
        return;
    fprintf(stderr, "FAIL %s calls %u/%u", name, nlog_o, nlog_n);
    if (k < nlog_o && k < nlog_n) {
        const CallRec *x = &log_o[k], *y = &log_n[k];
        fprintf(stderr, " at #%u what %u/%u ret %08x/%08x", k, x->what, y->what, x->ret, y->ret);
        for (int r = 0; r < 8; ++r)
            if (x->r[r] != y->r[r])
                fprintf(stderr, " r%d %08x/%08x", r, x->r[r], y->r[r]);
        for (int r = 0; r < 8; ++r)
            if (x->args[r] != y->args[r])
                fprintf(stderr, " arg%d %08x/%08x", r, x->args[r], y->args[r]);
        if (x->top != y->top || x->tag != y->tag || x->sw != y->sw || x->cw != y->cw)
            fprintf(stderr, " fpu %u/%u %04x/%04x %04x/%04x", x->top, y->top, x->tag, y->tag, x->sw, y->sw);
        for (int r = 0; r < 8; ++r)
            if (memcmp(x->xmm[r], y->xmm[r], 16))
                fprintf(stderr, " xmm%d", r);
        if (memcmp(x->st, y->st, sizeof x->st))
            fprintf(stderr, " st");
        if (x->stack_hash != y->stack_hash)
            fprintf(stderr, " stack");
        if (x->mem_hash != y->mem_hash)
            fprintf(stderr, " mem");
    } else if (k > 0)
        fprintf(stderr, " after #%u ret %08x", k - 1, log_o[k - 1].ret);
    fprintf(stderr, "\n");
}

static void test_tick(unsigned n) {
    X86 c;
    static Coverage cov;
    for (unsigned i = 0; i < n; ++i) {
        tick_case(&c);
        run_logged("TickParticles", &c, fn_103b5800, g3n_103b5800, &cov);
    }
    printf("TickParticles: %u cases, %lu guest calls from %u call sites compared (%lu rays, %lu bounces, %lu spawns, "
           "%lu rotations, %lu removals)\n",
           n, cov.calls, cov.nsites, cov.seen[F_TRACE], cov.seen[F_REFLECT], cov.seen[F_SPAWN], cov.seen[F_SETTOROT],
           cov.seen[F_COPY]);
}

/* ---------------------------------------------------------------- UpdateVertices
 *
 * Needs chunk_103b0000 (its own and CreateVerticesFX's). The particle system of tick_case, with
 * the orientation, normal, atlas and lock fields drawn here; the vertex buffer, camera and render
 * module are fakes, DepthSortParticles and CreateVerticesFX run as they are (the sort's qsort is a
 * fake that leaves the order; the vertex buffer exists or, now and then, cannot be made). */
void fn_103b3630(X86 *c);
void g3n_103b3630(X86 *c);
#define UV_RET 0x103b4cd4u /* RenderVertices' return site after CALL UpdateVertices */

static void uv_case(X86 *c) {
    tick_case(c);
    wr32(UV_WORLD + 0x5e8u, dt_rand32() % 16 ? 7u : 6u);
    wr32(PS + 0x348u, 6u); /* the frame it last ran in */
    for (uint32_t k = 0x8cu; k <= 0x94u; k += 4u)
        wrf32(UV_CAMOBJ + k, rf(6));
    for (uint32_t k = 0; k < 64u; k += 4u) {
        wrf32(UV_CAMERA + 0x14u + k, rf(6));
        wrf32(ENTITY + 0x40u + k, rf(6));
    }
    wr32(PS + 0x13cu, dt_rand32() % 6);
    rvec(PS + 0x140u, 6);
    wrf32(PS + 0x14cu, rf(6));
    wrf32(PS + 0x150u, rf(6));
    wr32(PS + 0x15cu, dt_rand32() % 3);
    wr8(PS + 0x1c0u, dt_rand32() % 3 == 0);
    wr32(PS + 0x1d4u, dt_rand32() % 3 ? 0u : dt_rand32() % 4);
    wr32(PS + 0x1d8u, dt_rand32() % 3 ? 0u : dt_rand32() % 4);
    wr8(PS + 0x1e5u, dt_rand32() & 1);
    wr8(PS + 0x1e7u, dt_rand32() & 1);
    wr32(PS + 0x234u, dt_rand32() % 5);
    wr32(PS + 0x398u, UV_OBJ398);
    if (dt_rand32() % 16) {
        wr32(PS + 0x39cu, UV_VDATA);
    } else {
        wr32(PS + 0x39cu, 0u);
        wr32(PS + 0x320u, 0u);
    }
    if ((int32_t)rd32(PS + 0x31cu) > 12)
        wr32(PS + 0x31cu, 12u);
    for (uint32_t i = 0; i < 12u; ++i)
        wrf32(PARTS + 0xb4u * i + 0xa8u, (float)(dt_rand32() % 4000u) / 1000.0f);
    for (uint32_t k = 0; k < 0xc0u * 12u; k += 4u)
        wr32(UV_VBUF + k, 0xdeadbeefu);
    c->r[4] += 8; /* tick_case pushed dt and its return address */
    c->r[4] -= 4;
    wr32(c->r[4], UV_RET);
}

static void test_uv(unsigned n) {
    X86 c;
    static Coverage cov;
    for (unsigned i = 0; i < n; ++i) {
        uv_case(&c);
        run_logged("UpdateVertices", &c, fn_103b3630, g3n_103b3630, &cov);
    }
    printf("UpdateVertices: %u cases, %lu guest calls from %u call sites compared (%lu quads, %lu spins, %lu atlas "
           "frames)\n",
           n, cov.calls, cov.nsites, cov.seen[F_BCOLOR_ARGB] / 2, cov.seen[F_AAROT], cov.seen[F_FLOOR]);}

/* G3N_BENCH: TickParticles over 8 live particles that stay alive (dt 1 ms, a long life), without
 * and with the collision ray, the fakes quiet. Each call goes on from the state the last one left. */
static void bench_tick(int collide, unsigned n) {
    double t[2];
    X86 c0;
    tick_case(&c0);
    wr32(PS + 0x31cu, 8u);
    wr8(PS + 0x1cu, 0);
    wr8(PS + 0x160u, 0);
    wr8(PS + 0x284u, (uint8_t)collide);
    wr8(PS + 0x285u, 0);
    wr8(PS + 0x1e4u, 0);
    wrf32(PS + 0x2d0u, -1.0f); /* no minimum speed: a bounce does not kill */
    for (uint32_t i = 0; i < 8u; ++i) {
        wrf32(PARTS + 0xb4u * i + 0xa0u, 0.0f);
        wrf32(PARTS + 0xb4u * i + 0xa4u, 1.0e6f);
        wr32(PARTS + 0xb4u * i + 0xb0u, 0u);
    }
    wrf32(c0.r[4] + 4u, 0.001f);
    dt_save(0);
    for (int w = 0; w < 2; ++w) {
        X86 c;
        dt_restore();
        g3_opt = 0xffffffffu; /* the leaves it calls are native in both */
        void (*const f)(X86 *) = w ? g3n_103b5800 : fn_103b5800;
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (unsigned i = 0; i < n; ++i) {
            c = c0;
            f(&c);
        }
        clock_gettime(CLOCK_MONOTONIC, &b);
        t[w] = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / n;
    }
    printf("  TickParticles, 8 particles%-10s translated %7.0f ns  native %7.0f ns  x%.2f\n",
           collide ? ", ray" : "", t[0], t[1], t[0] / t[1]);
}

/* G3N_BENCH: UpdateVertices over 8 particles facing the camera (orientation 0), with or without the
 * three spins and the frame atlas. */
static void bench_uv(int spin, unsigned n) {
    double t[2];
    X86 c0;
    uv_case(&c0);
    wr32(UV_WORLD + 0x5e8u, 7u);
    wr32(PS + 0x31cu, 8u);
    wr32(PS + 0x39cu, UV_VDATA);
    wr32(PS + 0x13cu, 0u);
    wr32(PS + 0x84u, 2u);
    wr32(PS + 0x15cu, 0u);
    wr32(PS + 0x234u, 0u);
    wr8(PS + 0x1c0u, 0);
    wr32(PS + 0x1d4u, spin ? 3u : 0u);
    wr32(PS + 0x1d8u, spin ? 3u : 0u);
    for (uint32_t i = 0; i < 8u; ++i)
        for (uint32_t k = 0x48u; k <= 0x50u; k += 4u)
            wrf32(PARTS + 0xb4u * i + k, spin ? 0.5f : 0.0f);
    dt_save(0);
    for (int w = 0; w < 2; ++w) {
        X86 c;
        dt_restore();
        g3_opt = 0xffffffffu;
        void (*const f)(X86 *) = w ? g3n_103b3630 : fn_103b3630;
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (unsigned i = 0; i < n; ++i) {
            wr32(PS + 0x348u, 6u);
            c = c0;
            f(&c);
        }
        clock_gettime(CLOCK_MONOTONIC, &b);
        t[w] = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / n;
    }
    printf("  UpdateVertices, 8 quads%-13s translated %7.0f ns  native %7.0f ns  x%.2f\n",
           spin ? ", spins, atlas" : "", t[0], t[1], t[0] / t[1]);
}

/* Whether chunk_103b0000 is linked: the weak particle copy says so when it is the one that runs. */
static int have_103b0000(void) {
    X86 c;
    dt_context(&c, ESP0, 0);
    c.r[0] = PARTS + 0xb4u * 11u;
    c.r[1] = PARTS + 0xb4u * 10u;
    c.r[4] -= 4;
    wr32(c.r[4], 0x103b58bdu);
    fakes_103b0000 = 0;
    fn_103b0330(&c);
    nlog_o = nlog_n = 0;
    return !fakes_103b0000;
}

int main(void) {
    harness_mem();
    dt_load_pe("Engine.dll");
    dt_stack_floor = ESP0 - 0x1000u;
    dt_region(dt_stack_floor, 0x1010u);
    dt_region(OBJ, 0x3000u);
    dt_region(PS, 0x10000u);
    tick_setup();
    const int uv = have_103b0000();
    test_calc_value(30000);
    test_calc_value_linear(30000);
    test_calc_color(30000);
    test_calc_color_factor(100000);
    test_tick(20000);
    if (uv)
        test_uv(20000);
    else
        printf("UpdateVertices: not tested (link aux-engine/chunk_103b0000.c)\n");
    if (getenv("G3N_BENCH")) {
        fakes_quiet = 1;
        printf("bench (ns per call; the fakes are cheaper than the SharedBase functions):\n");
        bench_tick(0, 200000);
        bench_tick(1, 200000);
        if (uv) {
            bench_uv(0, 50000);
            bench_uv(1, 50000);
        }
        fakes_quiet = 0;
    }
    return dt_report("particles_test");
}
