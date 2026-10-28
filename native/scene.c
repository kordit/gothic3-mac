/* native/scene.c - frustum culling and the render list's gathering.
 *
 * Each g3n_<addr> stands in for the translated fn_<addr> (native/g3_overrides.h) and runs its
 * native version only under G3OPT(SCENE); otherwise it calls the original. Until its native
 * version is written it only calls the original; a native version must end as the translated
 * function does (see its RET in build/recomp/gen/aux-<module>/). */
#include "chunk.h"
#include "g3_opt.h"

#include <string.h>

void fn_103ff970(X86 *c); /* Engine eCFrustumDatabase::ExtractFromEntity */
void fn_1040c9d0(X86 *c); /* Engine eCFrustumFilter::CullFrustum (leaf) */
void fn_10494fc0(X86 *c); /* Engine FUN_10494fc0 (leaf, __fastcall, under eCEntityAdmin::Process) */
void fn_104956f0(X86 *c); /* Engine FUN_104956f0 (under eCEntityAdmin::Process) */

/* ---- ExtractFromEntity's callees, guest functions called from here ----------------------- */

void fn_103fd330(X86 *c); /* eCFrustumDatabase::FilterEntityCullCallback, RET 8 */
void fn_104030c0(X86 *c); /* eCFrustumElementItem::CullVPTHC, RET 4 */
void fn_10403390(X86 *c); /* eCFrustumElementItem::UpdateNodeData, RET 8 */
void fn_10402f30(X86 *c); /* eCFrustumElementItem::CullSmallObject, RET 8, float in ST0 */
void fn_10402f00(X86 *c); /* eCFrustumElementItem::CalculateLoDArea, RET 4 */
void fn_1040caf0(X86 *c); /* eCFrustumFilter::GetNearPlaneDistance, RET 4, float in ST0 */
void fn_10445780(X86 *c); /* array push_back (EAX = array), RET 4 */
void fn_10403580(X86 *c); /* eCFrustumElementItem::ResetDynamicQuery */
void fn_10402ec0(X86 *c); /* eCFrustumElementItem::CheckIfOccluder */
void fn_10403820(X86 *c); /* eCFrustumElementItem::RenderZPass */
#ifndef FN_103fd330
#define FN_103fd330 fn_103fd330
#endif
#ifndef FN_104030c0
#define FN_104030c0 fn_104030c0
#endif
#ifndef FN_10403390
#define FN_10403390 fn_10403390
#endif
#ifndef FN_10402f30
#define FN_10402f30 fn_10402f30
#endif
#ifndef FN_10402f00
#define FN_10402f00 fn_10402f00
#endif
#ifndef FN_1040caf0
#define FN_1040caf0 fn_1040caf0
#endif
#ifndef FN_10445780
#define FN_10445780 fn_10445780
#endif
#ifndef FN_10403580
#define FN_10403580 fn_10403580
#endif
#ifndef FN_10402ec0
#define FN_10402ec0 fn_10402ec0
#endif
#ifndef FN_10403820
#define FN_10403820 fn_10403820
#endif

static inline void sc_push(X86 *c, uint32_t v) {
    c->r[4] -= 4u;
    wr32(c->r[4], v);
}

static inline uint32_t sc_pop(X86 *c) {
    const uint32_t v = rd32(c->r[4]);
    c->r[4] += 4u;
    return v;
}

/* CALL target with the original's continuation `ret` on the guest stack, as the translation's
 * CALL_FN does (the profiler's path included; mod hooks are not consulted, as in vector.c). */
static inline void sc_call(X86 *c, uint32_t ret, uint32_t target, void (*f)(X86 *)) {
    sc_push(c, ret);
    if (recomp_profile_enabled)
        recomp_call(c, target);
    else
        f(c);
}

/* The x87 tail after a call that returned a float in ST0, compared with `b` pushed after it:
 *   FLD b / FXCH / FCOMIP ST0,ST1 / FSTP ST0          (GetNearPlaneDistance, b = FLDZ)
 * or, with `spill`, first FSTP float [spill] / FLD float [spill] (CullSmallObject).
 * Ends as the original does: the result's slot U and the one below it empty, holding b and the
 * value, the top one above U. Returns ST0 > b; a NaN on either side raises IE (FCOMI) and is
 * "not above". `bp` = 0 reads b as a float from memory after the spill (FLD float [EAX]). */
static inline int sc_fcmp_tail(X86 *c, uint32_t spill, uint32_t bp, double bz) {
    const unsigned u = c->fpu_top & 7u, below = (u - 1u) & 7u;
    double v = c->st[u];
    if (spill) {
        wrf32(spill, fto_float(c, v));
        v = (double)rdf32(spill);
    }
    const double b = bp ? (double)rdf32(bp) : bz;
    c->st[u] = b;
    c->st[below] = v;
    c->st_exact[u] = 0;
    c->st_exact[below] = 0;
    ftag_put(c, u, FTAG_EMPTY);
    ftag_put(c, below, FTAG_EMPTY);
    c->fpu_top = (u + 1u) & 7u;
    if (v != v || b != b) {
        c->fpu_sw |= 0x0001u;
        return 0;
    }
    return v > b;
}

#define EFE_GUARD 0x10874c0cu    /* function-static init bits of the two masks below */
#define EFE_REQUIRE 0x10874c08u  /* entity flags that must all be set: 0x10010c */
#define EFE_EXCLUDE 0x10874c04u  /* entity flags that must be clear: 0x40 */
#define EFE_EPSILON_IAT 0x105f6864u /* &g_fEpsilon (SharedBase import) */

/* bool eCFrustumDatabase::ExtractFromEntity(eCEntity *, eECullStatus), thiscall, RET 8.
 * The entity's frustum item is at entity+0x14c ({area, lod factor, lod area, entity, stamp, ...,
 * +0x18 dynamic query, +0x1c query result, +0x38 cull bits}); an entity whose stamp already
 * holds this database's frame (this+4) is skipped. Otherwise: the enable bits, the database's
 * filter callbacks, the VPT cull when the node was only partly inside, node data, small-object
 * cull, LoD area, then the render list (this+0x1c), and for dynamic-query entities the query
 * list (this+0x10) and the occlusion threshold (this+0x1b4).
 *
 * The same instructions as the original, minus EFLAGS: registers, pushes and the guest stack are
 * built as it builds them, so every callee sees the same ESP, arguments, ECX/EAX and return
 * address, and the values after a call are read back from memory and from ESP as the callee left
 * it. The pushed registers are popped back from the guest stack. */
void g3n_103ff970(X86 *c) {
    if (!G3OPT(SCENE)) {
        fn_103ff970(c);
        return;
    }
    uint32_t *const r = c->r;
    sc_push(c, r[1]);
    r[2] = rd32(r[4] + 8u); /* the entity */
    /* The entity's cache lines (128 bytes on Apple cores) are cold on most calls: the stamp
     * (+0x15c) is read first, then the flags (+0x3c) and the cull byte (+0x184), and the callees
     * read the box (+0xe8) and spheres. In the host profile the translation's samples pile up on
     * the flag test, waiting for its miss after the stamp's; prefetching the other lines now lets
     * the misses overlap. A prefetch changes nothing the guest can observe. */
    {
        const uint8_t *e = g_mem + r[2];
        __builtin_prefetch(e + 0x3cu);
        __builtin_prefetch(e + 0xe8u);
        __builtin_prefetch(e + 0x184u);
    }
    sc_push(c, r[3]);
    sc_push(c, r[5]);
    sc_push(c, r[6]);
    r[6] = r[1];
    r[0] = rd32(r[6] + 4u);
    const uint32_t stamp = rd32(r[2] + 0x15cu);
    sc_push(c, r[7]);
    r[7] = r[2] + 0x14cu;
    if (stamp == r[0])
        goto out_false;
    wr32(r[7] + 0x10u, r[0]);
    r[0] = rd32(EFE_GUARD);
    r[3] = r[2] + 0x3cu;
    if (!(r[0] & 1u)) {
        r[0] |= 1u;
        r[1] = 0x10010cu;
        wr32(EFE_GUARD, r[0]);
        wr32(EFE_REQUIRE, r[1]);
    } else {
        r[1] = rd32(EFE_REQUIRE);
    }
    if (!(r[0] & 2u)) {
        r[0] |= 2u;
        wr32(EFE_GUARD, r[0]);
        wr32(EFE_EXCLUDE, 0x40u);
    }
    if (!(rd8(r[7] + 0x38u) & 1u) && rd8(r[6] + 0x1bdu) != 1u)
        goto culled;
    r[0] = rd32(r[3]);
    r[5] = r[0] & r[1];
    if (r[5] != r[1])
        goto culled;
    r[1] = rd32(EFE_EXCLUDE);
    if (r[1] & r[0])
        goto culled;
    sc_push(c, r[2]);
    r[5] = r[6] + 0x40u;
    sc_push(c, r[5]);
    r[1] = r[6];
    sc_call(c, 0x103ffa0bu, 0x103fd330u, FN_103fd330);
    if ((uint8_t)r[0] != 1u)
        goto culled;
    if (rd32(r[4] + 0x1cu) != 0u) { /* the cull status argument */
        sc_push(c, r[6]);
        r[1] = r[7];
        sc_call(c, 0x103ffa22u, 0x104030c0u, FN_104030c0);
        if ((uint8_t)r[0] != 1u)
            goto culled;
    }
    sc_push(c, r[3]);
    sc_push(c, r[6]);
    r[1] = r[7];
    sc_call(c, 0x103ffa33u, 0x10403390u, FN_10403390);
    r[2] = rd8(r[6] + 0x1ceu);
    sc_push(c, r[2]);
    sc_push(c, r[6]);
    r[1] = r[7];
    sc_call(c, 0x103ffa43u, 0x10402f30u, FN_10402f30);
    r[0] = rd32(EFE_EPSILON_IAT);
    /* FSTP float [ESP+0x1c] / FLD it / FLD float [EAX] / FXCH / FCOMIP / FSTP ST0 / JBE */
    if (!sc_fcmp_tail(c, r[4] + 0x1cu, r[0], 0.0))
        goto culled;
    sc_push(c, r[6]);
    r[1] = r[7];
    sc_call(c, 0x103ffa66u, 0x10402f00u, FN_10402f00);
    if (rd32(r[3]) & 0x2000000u)
        goto out_true;
    if (rd8(r[6] + 0x1afu) != 1u)
        goto no_query;
    r[1] = (r[1] & 0xffffff00u) | (rd8(r[7] + 0x38u) >> 5);
    if (!(r[1] & 1u))
        goto no_query;
    r[2] = rd32(r[4] + 0x18u) + 0xe8u; /* the entity's box */
    sc_push(c, r[2]);
    r[1] = r[5];
    sc_call(c, 0x103ffa98u, 0x1040caf0u, FN_1040caf0);
    /* FLDZ / FXCH / FCOMIP / FSTP ST0 / JBE */
    if (!sc_fcmp_tail(c, 0u, 0u, 0.0))
        goto no_query;
    if (rd32(r[7] + 0x18u) == 0u) {
        r[0] = r[4] + 0x18u;
        sc_push(c, r[0]);
        r[0] = r[6] + 0x10u;
        wr32(r[4] + 0x1cu, r[7]);
        sc_call(c, 0x103ffab9u, 0x10445780u, FN_10445780);
    }
    r[1] = rd32(r[7] + 0x1cu);
    if (r[1] < rd32(r[6] + 0x1b4u)) {
        r[0] = (r[0] & 0xffffff00u) | 1u;
        goto out;
    }
    goto listed;
no_query:
    r[1] = r[7];
    sc_call(c, 0x103ffad5u, 0x10403580u, FN_10403580);
listed:
    r[2] = r[4] + 0x18u;
    sc_push(c, r[2]);
    r[0] = r[6] + 0x1cu;
    wr32(r[4] + 0x1cu, r[7]);
    sc_call(c, 0x103ffae6u, 0x10445780u, FN_10445780);
    if (rd8(r[6] + 0x192u) == 1u) {
        r[1] = r[7];
        sc_call(c, 0x103ffaf6u, 0x10402ec0u, FN_10402ec0);
        if ((uint8_t)r[0] == 1u) {
            r[1] = r[7];
            sc_call(c, 0x103ffb01u, 0x10403820u, FN_10403820);
        }
    }
out_true:
    r[0] = (r[0] & 0xffffff00u) | 1u;
    goto out;
culled:
    if (rd8(r[6] + 0x1afu) == 1u) {
        r[0] = (r[0] & 0xffffff00u) | (rd8(r[7] + 0x38u) >> 5);
        if (r[0] & 1u) {
            r[1] = r[7];
            sc_call(c, 0x103ffb25u, 0x10403580u, FN_10403580);
        }
    }
out_false:
    r[0] &= 0xffffff00u;
out:
    r[7] = sc_pop(c);
    r[6] = sc_pop(c);
    r[5] = sc_pop(c);
    r[3] = sc_pop(c);
    r[1] = sc_pop(c);
    c->eip = rd32(r[4]);
    r[4] += 12u;
    recomp_return(c);
}

/* Helpers for CullFrustum (prefixed so they stay apart from the rest of this file). */
static inline double cf_ld(uint32_t a) {
    return (double)rdf32(a);
}
/* FCOMI: IE on any NaN; returns a > b (JA taken). */
static inline int cf_gt(X86 *c, double a, double b) {
    if (a != a || b != b)
        c->fpu_sw |= 0x0001u;
    return a > b;
}
static inline int cf_overlaps(uint32_t a, uint32_t an, uint32_t b, uint32_t bn) {
    return (uint64_t)a < (uint64_t)b + bn && (uint64_t)b < (uint64_t)a + an;
}

/* int eCFrustumFilter::CullFrustum(float const *box), thiscall, RET 4. The six planes {n[3], d}
 * at this+0x60, tested from the last; plane k picks its box corners by the index bytes at
 * this+0xc0+3k (the far corner) and this+0xd2+3k (the near one), box[i] the float at box+4*i.
 * Each plane's four floats go through stack slots (the normal's x through the argument slot).
 * The far corner's distance below zero: EAX = 15 (outside). Otherwise, until a plane has cut
 * the box, the near corner's distance below zero marks it cut. At the end EAX = cut (0 or 1).
 *
 * As the original keeps it on the x87 stack: a 0.0 under everything (FLDZ, the comparisons'
 * left side), the normal and d below the sums, seven deep at most (ffree_below 7). EDX ends as
 * the last plane's index (or its last near-corner index byte); EBX, ESI, EDI as they came; the
 * flags as ADD ESP,0x10 leaves them. The frame (ESP0-0x1c to ESP0+8) is locals here, so a frame
 * that overlaps the planes or the box runs the original. */
void g3n_1040c9d0(X86 *c) {
    if (!G3OPT(SCENE)) {
        fn_1040c9d0(c);
        return;
    }
    const uint32_t self = c->r[1], esp0 = c->r[4];
    const uint32_t box = rd32(esp0 + 4u);
    if (cf_overlaps(esp0 - 0x1cu, 0x24u, self + 0x60u, 0x84u) || cf_overlaps(esp0 - 0x1cu, 0x24u, box, 0x400u)) {
        fn_1040c9d0(c);
        return;
    }
    uint32_t edx = 0, eax = 0;
    uint8_t cut = 0;
    float sny = 0, snx = 0, snz = 0, sd = 0;
    int out = 0;
    for (int k = 5; k >= 0; --k) {
        const uint32_t pl = self + 0x10u * (uint32_t)(k + 6);
        const uint32_t ix = self + 3u * (uint32_t)k; /* EDI */
        edx = (uint32_t)k;
        sny = fto_float(c, cf_ld(pl + 4u));
        snx = fto_float(c, cf_ld(pl));
        snz = fto_float(c, cf_ld(pl + 8u));
        sd = fto_float(c, cf_ld(pl + 0xcu));
        const double nx = snx, ny = sny, nz = snz, d = sd;
        const double b0 = cf_ld(box + 4u * rd8(ix + 0xc0u));
        const double b1 = cf_ld(box + 4u * rd8(ix + 0xc1u));
        const double b2 = cf_ld(box + 4u * rd8(ix + 0xc2u));
        const double p0 = fx87(c, b0 * nx);
        const double p1 = fx87(c, b1 * ny);
        const double s1 = fx87(c, p1 + p0);
        const double p2 = fx87(c, b2 * nz);
        const double s2 = fx87(c, p2 + s1);
        const double dist = fx87(c, s2 + d);
        if (cf_gt(c, 0.0, dist)) {
            out = 1;
            break;
        }
        if (!cut) {
            const uint32_t j0 = rd8(ix + 0xd2u), j1 = rd8(ix + 0xd3u), j2 = rd8(ix + 0xd4u);
            const double q0 = fx87(c, nx * cf_ld(box + 4u * j0));
            const double q1 = fx87(c, ny * cf_ld(box + 4u * j1));
            const double t1 = fx87(c, q1 + q0);
            const double q2 = fx87(c, nz * cf_ld(box + 4u * j2));
            const double t2 = fx87(c, t1 + q2);
            const double dist2 = fx87(c, d + t2);
            edx = j2;
            if (cf_gt(c, 0.0, dist2))
                cut = 1;
        }
    }
    eax = out ? 0xfu : (uint32_t)(cut != 0);
    /* the frame as the original leaves it: EDI ESI EBX pushed, the cut byte, the slots */
    wr32(esp0 - 0x1cu, c->r[7]);
    wr32(esp0 - 0x18u, c->r[6]);
    wr32(esp0 - 0x14u, c->r[3]);
    wr8(esp0 - 0xdu, cut);
    wrf32(esp0 - 0xcu, sny);
    wrf32(esp0 - 8u, snz);
    wrf32(esp0 - 4u, sd);
    wrf32(esp0 + 4u, snx);
    c->r[0] = eax;
    c->r[2] = edx;
    {   /* ADD ESP,0x10 */
        const uint32_t a = esp0 - 0x10u, r = esp0;
        c->eflags_cf = r < a;
        c->eflags_of = ((~(a ^ 0x10u) & (a ^ r)) >> 31) & 1u;
        c->eflags_af = ((a ^ 0x10u ^ r) >> 4) & 1u;
        c->eflags_zf = r == 0;
        c->eflags_sf = r >> 31;
        c->eflags_pf = parity8(r);
    }
    for (unsigned i = 1; i <= 7u; ++i) {
        const unsigned p = (c->fpu_top - i) & 7u;
        ftag_put(c, p, FTAG_EMPTY);
        c->st_exact[p] = 0;
    }
    c->eip = rd32(esp0);
    c->r[4] = esp0 + 8u;
    recomp_return(c);
}

/* FUN_10494fc0 is no leaf. It resets the shader constant ranges at ESI+0x20, makes one virtual
 * call per component (vtable +8) and one per entry (vtable +4), then flushes up to three ranges
 * through the device (vtable +0x18c..0x194 or +0x1b4..0x1bc). Under the scene group it stays the
 * translation; the exec group (exec.c, g3x_10494fc0) has a native version that keeps the loads,
 * stores and guest calls but not the EFLAGS the translation carries into those calls (MSVC code
 * never reads flags on entry, as everywhere else in native/). */
void g3x_10494fc0(X86 *c);
void g3n_10494fc0(X86 *c) {
    if (G3OPT(EXEC)) {
        g3x_10494fc0(c);
        return;
    }
    fn_10494fc0(c);
}

/* ---- FUN_104956f0: a texture stage's image and sampler states, set only where they changed -- */

void fn_102f1430(X86 *c); /* eCGfxImageData::SetUnitNumber(stage), RET 4 */
void fn_102f1560(X86 *c); /* eCGfxImageData::Execute, RET; EAX = bEResult */
#ifndef FN_102f1430
#define FN_102f1430 fn_102f1430
#endif
#ifndef FN_102f1560
#define FN_102f1560 fn_102f1560
#endif

/* CALL EAX with the continuation `ret`, through a call site's inline cache as the translation's. */
static inline void sc_icall(X86 *c, uint32_t ret, uint64_t *ic) {
    const uint32_t t = c->r[0];
    sc_push(c, ret);
    recomp_call_cached(c, t, ic);
}

#define SAMP_DEFAULT 0xababababu /* "the device's default" in the wanted states */

/* One of the sampler states 2, 5, 6, 7 (the shape the original repeats for each): wanted
 * [EDI+off] against the stage's cached [ESI+off]; when they differ, SetSamplerState(stage, type,
 * value) through the device wrapper's vtable +0x1f8 (thiscall, RET 0xc), the default 0xabababab
 * replaced by [wrapper+0x48] (clamped to 2 for type 7), then the cache updated. */
static inline void samp_state(X86 *c, uint32_t off, uint32_t type, uint32_t ret, int clamp2, uint64_t *ic) {
    uint32_t *const r = c->r;
    r[0] = rd32(r[7] + off);
    if (rd32(r[6] + off) == r[0])
        return;
    r[1] = rd32(r[5] + 8u);
    if (r[0] == SAMP_DEFAULT) {
        r[0] = rd32(r[1] + 0x48u);
        if (clamp2 && r[0] > 2u)
            r[0] = 2u;
    }
    r[1] = rd32(r[1] + 0xcu);
    r[2] = rd32(r[1]);
    sc_push(c, r[0]);
    r[0] = rd32(r[3] + 0x28u);
    sc_push(c, type);
    sc_push(c, r[0]);
    r[0] = rd32(r[2] + 0x1f8u);
    sc_icall(c, ret, ic);
    r[1] = rd32(r[7] + off);
    wr32(r[6] + off, r[1]);
}

/* bool FUN_104956f0(renderer, stage), EAX = eCGfxImageData *, EDI = the wanted sampler states
 * (6 dwords), RET 8. [renderer+4] = the device state ({+0x7c4 image per stage, +0x804 sampler
 * states per stage, 0x18 bytes}), [renderer+8] = the device wrapper, [stage+0x28] = the stage
 * number. A null image is false; an image that is not the stage's current one is bound
 * (SetUnitNumber, Execute; false if Execute fails); then states 1, 2, 5, 6, 7, 8 as in
 * samp_state. Called per draw and texture stage.
 *
 * The original's instructions minus EFLAGS, with its pushes, its argument-slot spill of state 1
 * ([ESP+0x14]) and its register use (EBX and EBP reloaded for state 8), so every guest call sees
 * the same stack and registers. */
void g3n_104956f0(X86 *c) {
    if (!G3OPT(SCENE)) {
        fn_104956f0(c);
        return;
    }
    static uint64_t ic1, ic2, ic5, ic6, ic7, ic8;
    uint32_t *const r = c->r;
    sc_push(c, r[3]);
    r[3] = rd32(r[4] + 0xcu);
    sc_push(c, r[5]);
    r[5] = rd32(r[4] + 0xcu);
    sc_push(c, r[6]);
    r[6] = r[0];
    r[0] &= 0xffffff00u;
    if (r[6] == 0u)
        goto out;
    r[0] = rd32(r[3] + 0x28u);
    r[1] = rd32(r[5] + 4u);
    if (rd32(r[1] + r[0] * 4u + 0x7c4u) != r[6]) {
        sc_push(c, r[0]);
        r[1] = r[6];
        sc_call(c, 0x1049571eu, 0x102f1430u, FN_102f1430);
        r[1] = r[6];
        sc_call(c, 0x10495725u, 0x102f1560u, FN_102f1560);
        if (r[0] != 1u) {
            r[0] &= 0xffffff00u;
            goto out;
        }
        r[2] = rd32(r[3] + 0x28u);
        r[0] = rd32(r[5] + 4u);
        wr32(r[0] + r[2] * 4u + 0x7c4u, r[6]);
    }
    r[0] = rd32(r[3] + 0x28u);
    r[2] = rd32(r[5] + 4u);
    r[1] = r[0] * 3u;
    r[6] = r[2] + r[1] * 8u + 0x804u;
    /* state 1: the value goes through the stage argument's slot */
    r[1] = rd32(r[7]);
    if (rd32(r[6]) != r[1]) {
        r[2] = rd32(r[5] + 8u);
        wr32(r[4] + 0x14u, r[1]);
        if (r[1] == SAMP_DEFAULT) {
            r[1] = rd32(r[2] + 0x48u);
            wr32(r[4] + 0x14u, r[1]);
        }
        sc_push(c, rd32(r[4] + 0x14u));
        r[1] = rd32(r[2] + 0xcu);
        r[2] = rd32(r[1]);
        sc_push(c, 1u);
        sc_push(c, r[0]);
        r[0] = rd32(r[2] + 0x1f8u);
        sc_icall(c, 0x1049577bu, &ic1);
        r[1] = rd32(r[7]);
        wr32(r[6], r[1]);
    }
    samp_state(c, 4u, 2u, 0x104957a8u, 0, &ic2);
    samp_state(c, 0xcu, 5u, 0x104957d7u, 0, &ic5);
    samp_state(c, 8u, 6u, 0x10495806u, 0, &ic6);
    samp_state(c, 0x10u, 7u, 0x1049583fu, 1, &ic7);
    /* state 8: EBX and EBP become the stage number and the wrapper */
    r[0] = rd32(r[7] + 0x14u);
    if (rd32(r[6] + 0x14u) != r[0]) {
        r[3] = rd32(r[3] + 0x28u);
        r[5] = rd32(r[5] + 8u);
        if (r[0] == SAMP_DEFAULT)
            r[0] = rd32(r[5] + 0x48u);
        r[1] = rd32(r[5] + 0xcu);
        r[2] = rd32(r[1]);
        sc_push(c, r[0]);
        r[0] = rd32(r[2] + 0x1f8u);
        sc_push(c, 8u);
        sc_push(c, r[3]);
        sc_icall(c, 0x1049586eu, &ic8);
        r[1] = rd32(r[7] + 0x14u);
        wr32(r[6] + 0x14u, r[1]);
    }
    r[0] = (r[0] & 0xffffff00u) | 1u;
out:
    r[6] = sc_pop(c);
    r[5] = sc_pop(c);
    r[3] = sc_pop(c);
    c->eip = rd32(r[4]);
    r[4] += 12u;
    recomp_return(c);
}
