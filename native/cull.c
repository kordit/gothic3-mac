/* native/cull.c - the per-entity culling chain under eCFrustumDatabase::ExtractFromEntity.
 *
 * Each g3n_<addr> stands in for the translated fn_<addr> (native/g3_overrides.h) and runs its
 * native version only under G3OPT(CULL); otherwise it calls the original. ExtractFromEntity
 * (scene.c) reaches them through FN_<addr>, so with the group on the chain from CullVPTHC down to
 * the frustum planes runs here, without a translated function in between:
 *
 *   CullVPTHC 104030c0      bCBox::GetVertex (SharedBase 1001d070) computed in place, the
 *                           singleton getter FUN_10277ac0 in place when it is initialised, the
 *                           sphere and frustum point tests through vector.c's natives
 *   CullCamera 1040cb50     sphere-sphere through leaf.c's native, then the two below
 *   CullFrustum 1040c940    the sphere against six planes (GetDotProduct in place)
 *   CullFrustum 1040c9d0    the box (scene.c)
 *   CullCamera 1040cba0     the box against the filter's bounds, then 1040c9d0
 *   UpdateNodeData 10403390 InvTranslate through vector.c, GetMagnitude stays a guest call (the
 *                           CRT's sqrt with its control-word checks), CalculateAreaFactor below
 *   CalculateAreaFactor 1040c5a0 (InvTranslate and GetDotProduct in place)
 *   CalculateLoDArea 10402f00
 *
 * Exactness, as in vector.c and scene.c: the original's x87 arithmetic step for step with the
 * kit's helpers (fx87 per FADD/FSUB/FMUL/FDIV, fdivz, fto_float per FSTP to a float, IE from
 * FCOMI on any NaN and from FUCOMI on a signalling one), the x87 stack left as the original leaves
 * it (the slots it pushed into tagged empty, a float result in ST0), the registers as the last
 * instruction that set them leaves them, XMM0/XMM1 as its MOVSS loads leave them. A guest call
 * (or a native that stands in for one) sees the same ESP, arguments, registers and return address
 * as in the original. EFLAGS are not kept (nothing reads them across a call in MSVC code).
 * Stack bytes below the final ESP that nobody reads (spill slots, local copies only native code
 * here reads) are not written: they are dead once the function returns. A local that a callee
 * reads from guest memory is written.
 *
 * The SharedBase calls go through Engine's import table; each native checks at entry that the
 * slots it bypasses hold the export it expects, and otherwise runs the translation (a mod that
 * hooks an import keeps working). Mod hooks on the Engine functions themselves are not consulted,
 * as in vector.c.
 *
 * native/tests/cull_test.c runs each against the translation: registers, x87 state, XMM and
 * memory, bit for bit. */
#include "chunk.h"
#include "g3_opt.h"

#include <string.h>

void fn_104030c0(X86 *c); /* Engine eCFrustumElementItem::CullVPTHC */
void fn_1040cb50(X86 *c); /* Engine eCFrustumFilter::CullCamera(box, sphere) */
void fn_1040cba0(X86 *c); /* Engine eCFrustumFilter::CullCamera(box) */
void fn_1040c940(X86 *c); /* Engine eCFrustumFilter::CullFrustum(sphere) */
void fn_10403390(X86 *c); /* Engine eCFrustumElementItem::UpdateNodeData */
void fn_1040c5a0(X86 *c); /* Engine eCFrustumFilter::CalculateAreaFactor */
void fn_10402f00(X86 *c); /* Engine eCFrustumElementItem::CalculateLoDArea */

/* natives of other groups this file calls in place of the guest calls (each falls back to its
 * translation when its own group is off) */
void g3n_10042f30(X86 *c); /* vector.c: SharedBase bCSphere::Intersects(bCVector) behind its JMP thunk 10043670 */
void g3n_10035fd0(X86 *c); /* vector.c: SharedBase bCFrustum::Contains(bCVector) behind its JMP thunk 10036690 */
void g3n_1001ae70(X86 *c); /* vector.c: SharedBase bCVector::InvTranslate */
void g3n_1001da30(X86 *c); /* leaf.c: SharedBase bCSphere::Intersects(bCSphere) */
void g3n_1040c9d0(X86 *c); /* scene.c: Engine eCFrustumFilter::CullFrustum(box) */

void fn_10277ac0(X86 *c); /* Engine FUN_10277ac0: the application singleton (statistics counters) */
void fn_10010c50(X86 *c); /* SharedBase bCVector::GetMagnitude */
#ifndef FN_10277ac0
#define FN_10277ac0 fn_10277ac0
#endif
#ifndef FN_10010c50
#define FN_10010c50 fn_10010c50
#endif

/* Engine's import slots and the SharedBase exports they hold in an unmodded game */
#define IAT_GET_VERTEX 0x105f6df8u /* ?GetVertex@bCBox@@QBEXHAAVbCVector@@@Z */
#define SB_GET_VERTEX 0x1001d070u
#define IAT_SPHERE_HAS_POINT 0x105f6a34u /* ?Intersects@bCSphere@@QBE_NABVbCVector@@@Z */
#define SB_SPHERE_HAS_POINT 0x10043670u
#define IAT_FRUSTUM_HAS_POINT 0x105f6e28u /* ?Contains@bCFrustum@@QBE_NABVbCVector@@@Z */
#define SB_FRUSTUM_HAS_POINT 0x10036690u
#define IAT_SPHERES 0x105f6a0cu /* ?Intersects@bCSphere@@QBE_NABV1@@Z */
#define SB_SPHERES 0x1001da30u
#define IAT_DOT 0x105f6854u /* ?GetDotProduct@bCVector@@QBEMABV1@@Z */
#define SB_DOT 0x1001b580u
#define IAT_INV_TRANSLATE 0x105f66ecu /* ?InvTranslate@bCVector@@QAEXABV1@@Z */
#define SB_INV_TRANSLATE 0x1001ae70u
#define IAT_MAGNITUDE 0x105f67c0u /* ?GetMagnitude@bCVector@@QBEMXZ */
#define SB_MAGNITUDE 0x10010c50u
#define IAT_PI 0x105f63fcu /* &g_fPI */

/* FUN_10277ac0's state: enabled byte, init guard, the object */
#define APP_ENABLED 0x10862a8cu
#define APP_GUARD 0x10874bd4u
#define APP_OBJECT 0x10874bd0u

#define AREA_EPS 0x3727c5acu   /* [0x10685f58] 1e-5f, the divisor when the dot product is 0 */
#define AREA_ONE 0x3f800000u   /* [0x1068612c] 1.0f */
#define AREA_MIN 0x106e88c0u   /* double 3e-6 */

static inline void cu_push(X86 *c, uint32_t v) {
    c->r[4] -= 4u;
    wr32(c->r[4], v);
}

static inline uint32_t cu_pop(X86 *c) {
    const uint32_t v = rd32(c->r[4]);
    c->r[4] += 4u;
    return v;
}

static inline void cu_ret(X86 *c, uint32_t args) {
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u + args;
    recomp_return(c);
}

/* CALL with continuation `ret` into a native (or translated) function called directly. */
static inline void cu_call(X86 *c, uint32_t ret, void (*f)(X86 *)) {
    cu_push(c, ret);
    f(c);
}

static inline double cu_ld(uint32_t a) {
    return (double)rdf32(a);
}

static inline float cu_f(uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static inline uint32_t cu_u(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* FCOMI/FCOMIP: IE on any NaN; returns a > b (JA taken, JBE not taken). */
static inline int cu_gt(X86 *c, double a, double b) {
    if (a != a || b != b)
        c->fpu_sw |= 0x0001u;
    return a > b;
}

/* The original's pushes and pops leave ST(-1)..ST(-n) of the entry stack empty. */
static inline void cu_ffree_below(X86 *c, unsigned n) {
    for (unsigned i = 1; i <= n; ++i) {
        const unsigned p = (c->fpu_top - i) & 7u;
        ftag_put(c, p, FTAG_EMPTY);
        c->st_exact[p] = 0;
    }
}

/* MOVSS XMMn, [mem]: the low lane, the others cleared. */
static inline void cu_xmm(X86 *c, int reg, uint32_t bits) {
    c->xmm[reg][0] = bits;
    c->xmm[reg][1] = c->xmm[reg][2] = c->xmm[reg][3] = 0u;
}

static inline int cu_overlaps(uint32_t a, uint32_t an, uint32_t b, uint32_t bn) {
    return (uint64_t)a < (uint64_t)b + bn && (uint64_t)b < (uint64_t)a + an;
}

/* GetVertex's corners: bit 0 of the index picks min (1, 2 ... see the jump table at 1001d140)
 * per axis; the byte offsets into the box {min x, y, z, max x, y, z}. */
static const uint8_t gv_x[8] = {0x0c, 0x0c, 0x00, 0x00, 0x0c, 0x0c, 0x00, 0x00};
static const uint8_t gv_y[8] = {0x10, 0x04, 0x04, 0x10, 0x10, 0x04, 0x04, 0x10};
static const uint8_t gv_z[8] = {0x14, 0x14, 0x14, 0x14, 0x08, 0x08, 0x08, 0x08};

/* void bCBox::GetVertex(int idx, bCVector &out) const, RET 8, idx 0..7: x through FLD/FSTP, z
 * and y through MOVSS XMM0 and XMM1 (stored in that order), EAX = &out. */
static inline void cu_get_vertex(X86 *c, uint32_t box, uint32_t idx, uint32_t out) {
    const float x = fto_float(c, cu_ld(box + gv_x[idx]));
    const uint32_t z = rd32(box + gv_z[idx]), y = rd32(box + gv_y[idx]);
    cu_xmm(c, 0, z);
    cu_xmm(c, 1, y);
    wrf32(out, x);
    wr32(out + 8u, z);
    wr32(out + 4u, y);
    c->r[0] = out;
    cu_ffree_below(c, 1);
}

/* FUN_10277ac0 at a CALL: EAX = the singleton, or 0 when the module is disabled; the first call
 * (it builds the object) goes to the guest. */
static inline void cu_app(X86 *c, uint32_t ret) {
    if (rd8(APP_ENABLED) != 1u) {
        c->r[0] = 0u;
        return;
    }
    if (rd8(APP_GUARD) & 1u) {
        c->r[0] = rd32(APP_OBJECT);
        return;
    }
    cu_push(c, ret);
    if (recomp_profile_enabled)
        recomp_call(c, 0x10277ac0u);
    else
        FN_10277ac0(c);
}

/* bool eCFrustumElementItem::CullVPTHC(eCFrustumDatabase &db), thiscall on the item, RET 4.
 * The item's +0x38 bits 1-3 name the box corner that last proved the entity visible: that corner
 * inside the database's sphere (db+0x78) and frustum (db+0xa0) is true (counter app+0x168);
 * otherwise the corner index moves on (7 wraps to 0), counter app+0x170, and the full
 * CullCamera(box, sphere) of the filter db+0x40 decides (anything but 15 is true). */
void g3n_104030c0(X86 *c) {
    if (!G3OPT(CULL) || rd32(IAT_GET_VERTEX) != SB_GET_VERTEX || rd32(IAT_SPHERE_HAS_POINT) != SB_SPHERE_HAS_POINT ||
        rd32(IAT_FRUSTUM_HAS_POINT) != SB_FRUSTUM_HAS_POINT) {
        fn_104030c0(c);
        return;
    }
    uint32_t *const r = c->r;
    r[4] -= 0x10u;
    cu_push(c, r[6]);
    r[6] = r[1];
    r[0] = (rd8(r[6] + 0x38u) >> 1) & 7u;
    r[1] = rd32(r[6] + 0xcu) + 0xe8u;
    cu_push(c, r[7]);
    r[2] = r[4] + 0xcu; /* the corner, a local */
    /* PUSH EDX / PUSH EAX / CALL GetVertex (RET 8) */
    cu_get_vertex(c, r[1], r[0], r[2]);
    r[7] = rd32(r[4] + 0x1cu) + 0x40u; /* the filter */
    r[1] = r[4] + 0xcu;
    cu_push(c, r[1]);
    r[1] = r[7] + 0x38u;
    cu_call(c, 0x104030fbu, g3n_10042f30);
    if ((uint8_t)r[0] != 0u) {
        r[2] = r[4] + 0xcu;
        cu_push(c, r[2]);
        r[1] = r[7] + 0x60u;
        cu_call(c, 0x1040310du, g3n_10035fd0);
        if ((uint8_t)r[0] == 1u) {
            cu_app(c, 0x10403116u);
            r[0] += 0x168u;
            wr32(r[0], rd32(r[0]) + 1u);
            r[0] = (r[0] & 0xffffff00u) | 1u;
            r[7] = cu_pop(c);
            r[6] = cu_pop(c);
            r[4] += 0x10u;
            cu_ret(c, 4);
            return;
        }
    }
    r[0] = rd16(r[6] + 0x38u);
    r[1] = (r[1] & 0xffffff00u) | (r[0] & 0xeu);
    if ((r[0] & 0xeu) == 0xeu) {
        r[0] &= 0xfff1u;
        wr16(r[6] + 0x38u, (uint16_t)r[0]);
    } else {
        r[2] = r[0];
        uint32_t dl = ((r[2] & 0xfeu) + 2u) & 0xffu;
        dl ^= r[0] & 0xffu;
        r[2] = (r[2] & 0xffffff00u) | dl;
        r[2] &= 0xeu;
        r[2] ^= r[0];
        wr16(r[6] + 0x38u, (uint16_t)r[2]);
    }
    cu_app(c, 0x10403159u);
    wr32(r[0] + 0x170u, rd32(r[0] + 0x170u) + 1u);
    r[6] = rd32(r[6] + 0xcu);
    r[0] = r[6] + 0x100u;
    r[1] = r[6] + 0xe8u;
    cu_push(c, r[0]);
    cu_push(c, r[1]);
    r[1] = r[7];
    cu_call(c, 0x1040317du, g3n_1040cb50);
    r[1] = r[0] != 0xfu ? 1u : 0u;
    r[7] = cu_pop(c);
    r[0] = (r[0] & 0xffffff00u) | r[1];
    r[6] = cu_pop(c);
    r[4] += 0x10u;
    cu_ret(c, 4);
}

/* eECullStatus eCFrustumFilter::CullCamera(bCBox const &, bCSphere const &) const, RET 8:
 * 15 (outside) unless the sphere meets the filter's sphere (this+0x38); then the sphere against
 * the planes, and on 1 (cut) the box against the planes. ECX ends as it came (PUSH/POP ECX). */
void g3n_1040cb50(X86 *c) {
    if (!G3OPT(CULL) || rd32(IAT_SPHERES) != SB_SPHERES) {
        fn_1040cb50(c);
        return;
    }
    uint32_t *const r = c->r;
    cu_push(c, r[1]);
    cu_push(c, r[6]);
    cu_push(c, r[7]);
    r[7] = rd32(r[4] + 0x14u);
    r[6] = r[1];
    cu_push(c, r[7]);
    r[1] = r[6] + 0x38u;
    cu_call(c, 0x1040cb63u, g3n_1001da30);
    if ((uint8_t)r[0] == 0u) {
        r[0] = 0xfu;
    } else {
        cu_push(c, r[7]);
        r[1] = r[6];
        cu_call(c, 0x1040cb7au, g3n_1040c940);
        if (r[0] == 1u) {
            r[0] = rd32(r[4] + 0x10u);
            cu_push(c, r[0]);
            r[1] = r[6];
            cu_call(c, 0x1040cb8bu, g3n_1040c9d0);
        }
    }
    r[7] = cu_pop(c);
    r[6] = cu_pop(c);
    r[1] = cu_pop(c);
    cu_ret(c, 8);
}

/* eECullStatus eCFrustumFilter::CullFrustum(bCSphere const &s) const, RET 4. Sphere {r, c[3]},
 * planes {n[3], d} at this+0x60, from the first: dist = (n . c through GetDotProduct's float
 * slot) + d, through a float slot. -r > dist: 15 (outside); r > |dist|: 1 (cut); all six
 * passed: 0 (inside). ECX ends as the last plane tested, EDX as it came. */
void g3n_1040c940(X86 *c) {
    if (!G3OPT(CULL) || rd32(IAT_DOT) != SB_DOT) {
        fn_1040c940(c);
        return;
    }
    const uint32_t self = c->r[1], esp0 = c->r[4];
    const uint32_t s = rd32(esp0 + 4u);
    /* the frame is ESP0-0x30..ESP0+8 at most; inputs inside it would see the original's spills */
    if (cu_overlaps(s, 16u, esp0 - 0x30u, 0x38u) || cu_overlaps(self + 0x60u, 0x60u, esp0 - 0x30u, 0x38u)) {
        fn_1040c940(c);
        return;
    }
    const double cx = cu_ld(s + 4u), cy = cu_ld(s + 8u), cz = cu_ld(s + 0xcu);
    const double rad = (double)fto_float(c, cu_ld(s));
    uint32_t pl = self + 0x60u, eax = 0u;
    for (int k = 0;; ++k, pl += 0x10u) {
        const double d = (double)fto_float(c, cu_ld(pl + 0xcu));
        const double a = fx87(c, cy * cu_ld(pl + 4u));
        const double b = fx87(c, cx * cu_ld(pl));
        const double ab = fx87(c, a + b);
        const double z = fx87(c, cz * cu_ld(pl + 8u));
        const double dot = (double)fto_float(c, fx87(c, ab + z));
        const double dist = (double)fto_float(c, fx87(c, dot + d));
        if (cu_gt(c, -rad, dist)) {
            eax = 0xfu;
            break;
        }
        if (cu_gt(c, rad, fabs(dist))) {
            eax = 1u;
            break;
        }
        if (k == 5)
            break;
    }
    c->r[0] = eax;
    c->r[1] = pl;
    cu_ffree_below(c, 3);
    c->eip = rd32(esp0);
    c->r[4] = esp0 + 8u;
    recomp_return(c);
}

/* eECullStatus eCFrustumFilter::CullCamera(bCBox const &) const, RET 4: 15 unless the box
 * overlaps the filter's bounds (this+0x48 min, this+0x54 max; a NaN on either side passes and
 * raises IE), then a tail jump to CullFrustum(box) with the box in the argument slot. EDX = box. */
void g3n_1040cba0(X86 *c) {
    if (!G3OPT(CULL)) {
        fn_1040cba0(c);
        return;
    }
    const uint32_t self = c->r[1];
    const uint32_t box = rd32(c->r[4] + 4u);
    c->r[2] = box;
    int out = cu_gt(c, cu_ld(box), cu_ld(self + 0x54u)) || cu_gt(c, cu_ld(self + 0x48u), cu_ld(box + 0xcu)) ||
              cu_gt(c, cu_ld(box + 4u), cu_ld(self + 0x58u)) || cu_gt(c, cu_ld(self + 0x4cu), cu_ld(box + 0x10u)) ||
              cu_gt(c, cu_ld(box + 8u), cu_ld(self + 0x5cu)) || cu_gt(c, cu_ld(self + 0x50u), cu_ld(box + 0x14u));
    cu_ffree_below(c, 2);
    if (out) {
        c->r[0] = 0xfu;
        cu_ret(c, 4);
        return;
    }
    wr32(c->r[4] + 4u, box);
    g3n_1040c9d0(c);
}

/* float eCFrustumFilter::CalculateAreaFactor(bCSphere const &s), RET 4: the sphere's centre
 * relative to the camera (camera = [this+0xc], position +0x8c) dotted with the view direction
 * (this+0x28); 0 becomes 1e-5. f = (r / dot)^2 * pi through float slots, clamped to [0, 1]
 * (a NaN stays). The result in ST0; XMM0 the clamped value's source (0 when below 0), XMM1 1.0,
 * EAX = the local's address with LAHF's AH, ECX = this+0x28, EDX = &g_fPI. */
void g3n_1040c5a0(X86 *c) {
    if (!G3OPT(CULL) || rd32(IAT_INV_TRANSLATE) != SB_INV_TRANSLATE || rd32(IAT_DOT) != SB_DOT) {
        fn_1040c5a0(c);
        return;
    }
    const uint32_t self = c->r[1], esp0 = c->r[4];
    const uint32_t s = rd32(esp0 + 4u);
    const uint32_t cam = rd32(self + 0xcu) + 0x8cu;
    if (cu_overlaps(s, 16u, esp0 - 0x20u, 0x28u) || cu_overlaps(cam, 12u, esp0 - 0x20u, 0x28u) ||
        cu_overlaps(self + 0x28u, 12u, esp0 - 0x20u, 0x28u)) {
        fn_1040c5a0(c);
        return;
    }
    /* the local copy of the centre, InvTranslate on it (FLD v, FSUB cam, FSTP v) */
    const float vx = fto_float(c, fx87(c, (double)cu_f(rd32(s + 4u)) - cu_ld(cam)));
    const float vy = fto_float(c, fx87(c, (double)cu_f(rd32(s + 8u)) - cu_ld(cam + 4u)));
    const float vz = fto_float(c, fx87(c, (double)cu_f(rd32(s + 0xcu)) - cu_ld(cam + 8u)));
    const uint32_t dir = self + 0x28u;
    const double a = fx87(c, (double)vy * cu_ld(dir + 4u));
    const double b = fx87(c, (double)vx * cu_ld(dir));
    const double ab = fx87(c, a + b);
    const double z = fx87(c, (double)vz * cu_ld(dir + 8u));
    const float dot = fto_float(c, fx87(c, ab + z));
    /* FLDZ / FLD ST0 / FUCOMIP ST0,ST2 / FSTP ST1 / LAHF / TEST AH,0x44 / JP */
    fucomi(c, 0.0, (double)dot);
    const uint32_t ah = (x86_get_eflags(c) & 0xd5u) | 0x02u;
    c->r[0] = ((esp0 - 0xcu) & 0xffff00ffu) | (ah << 8);
    uint32_t div = cu_u(dot);
    if ((ah & 0x44u) == 0x40u) {
        div = AREA_EPS;
        cu_xmm(c, 0, AREA_EPS);
    }
    const double q0 = fx87(c, fdivz(c, cu_ld(s), (double)cu_f(div)));
    c->r[2] = rd32(IAT_PI);
    cu_xmm(c, 1, AREA_ONE);
    const double q = (double)fto_float(c, q0);
    const double pq = fx87(c, cu_ld(c->r[2]) * q);
    const float f = fto_float(c, fx87(c, pq * q));
    cu_xmm(c, 0, cu_u(f));
    const int gt1 = cu_gt(c, (double)f, 1.0);
    const float g = gt1 ? 1.0f : f;
    uint32_t res;
    if (cu_gt(c, 0.0, (double)g)) {
        c->xmm[0][0] = c->xmm[0][1] = c->xmm[0][2] = c->xmm[0][3] = 0u;
        res = 0u;
    } else {
        res = cu_gt(c, (double)f, 1.0) ? AREA_ONE : cu_u(f);
    }
    c->r[1] = dir;
    cu_ffree_below(c, 4);
    fpush(c, (double)cu_f(res));
    c->eip = rd32(esp0);
    c->r[4] = esp0 + 8u;
    recomp_return(c);
}

/* void eCFrustumElementItem::UpdateNodeData(eCFrustumDatabase &db, eSEntityFlags const &f),
 * RET 8. Under flag 0x8000000 or 0x10000000: the distance to the camera ([db+0x134]+0x8c, the
 * entity's position +0x70) into item+0x3c. Then the area factor of the database's filter
 * (db+0x40) into item+0: from the entity's sphere (+0x100) under 0x80000 and 0x100000, and when
 * that is below 3e-6 and the entity's +0x12 is set, again from its second sphere (+0xd8); from
 * the second sphere alone under 0x200000; otherwise 0. The masks are function statics with init
 * guards, set on first use as the original does. */
void g3n_10403390(X86 *c) {
    if (!G3OPT(CULL) || rd32(IAT_INV_TRANSLATE) != SB_INV_TRANSLATE || rd32(IAT_MAGNITUDE) != SB_MAGNITUDE) {
        fn_10403390(c);
        return;
    }
    uint32_t *const r = c->r;
    r[4] -= 0xcu;
    const uint32_t g0 = rd32(0x1086de6cu);
    cu_push(c, r[3]);
    cu_push(c, r[5]);
    cu_push(c, r[6]);
    cu_push(c, r[7]);
    r[7] = r[1];
    r[5] = 1u;
    if (!(g0 & 1u)) {
        wr32(0x1086de6cu, rd32(0x1086de6cu) | 1u);
        wr32(0x1086de70u, 0x8000000u);
    }
    r[6] = rd32(r[4] + 0x24u);
    r[0] = rd32(r[6]);
    r[1] = rd32(0x1086de70u);
    r[3] = rd32(r[4] + 0x20u);
    int dist = (r[1] & r[0]) != 0u;
    if (!dist) {
        if (!(rd8(0x1086de74u) & 1u)) {
            wr32(0x1086de74u, rd32(0x1086de74u) | 1u);
            wr32(0x1086de78u, 0x10000000u);
        }
        r[2] = rd32(0x1086de78u);
        dist = (r[2] & r[0]) != 0u;
    }
    if (dist) {
        r[0] = rd32(r[3] + 0x134u) + 0x8cu;
        const uint32_t px = rd32(r[0]), py = rd32(r[0] + 4u), pz = rd32(r[0] + 8u);
        wr32(r[4] + 0x10u, px);
        wr32(r[4] + 0x14u, py);
        cu_xmm(c, 0, pz);
        r[0] = rd32(r[7] + 0xcu) + 0x70u;
        cu_push(c, r[0]);
        r[1] = r[4] + 0x14u;
        wr32(r[4] + 0x1cu, pz);
        cu_call(c, 0x1040342eu, g3n_1001ae70);
        r[1] = r[4] + 0x10u;
        cu_push(c, 0x10403438u);
        if (recomp_profile_enabled)
            recomp_call(c, SB_MAGNITUDE);
        else
            FN_10010c50(c);
        wrf32(r[7] + 0x3cu, fto_float(c, ST(c, 0)));
        fdrop(c);
    }
    if (!(rd8(0x1086de64u) & 1u)) {
        wr32(0x1086de64u, rd32(0x1086de64u) | r[5]);
        wr32(0x1086de68u, 0x80000u);
    }
    r[0] = rd32(r[6]);
    r[1] = rd32(0x1086de68u);
    uint32_t sphere;
    if (r[1] & r[0]) {
        if (!(rd8(0x1086dc14u) & 1u)) {
            wr32(0x1086dc14u, rd32(0x1086dc14u) | r[5]);
            wr32(0x1086dc18u, 0x100000u);
        }
        r[2] = rd32(0x1086dc18u);
        if (r[2] & r[0]) {
            r[0] = rd32(r[7] + 0xcu) + 0x100u;
            r[6] = r[3] + 0x40u;
            cu_push(c, r[0]);
            r[1] = r[6];
            cu_call(c, 0x10403496u, g3n_1040c5a0);
            /* FSTP float [ESP+0x24] / FLD it / FST float [EDI] / FLD double 3e-6 / FCOMIP / FSTP ST0 */
            wrf32(r[4] + 0x24u, fto_float(c, ST(c, 0)));
            fdrop(c);
            fpush(c, cu_ld(r[4] + 0x24u));
            wrf32(r[7], fto_float(c, ST(c, 0)));
            fpush(c, rdf64(AREA_MIN));
            const int small = cu_gt(c, ST(c, 0), ST(c, 1));
            fdrop(c);
            fdrop(c);
            if (!small)
                goto done;
            r[0] = rd32(r[7] + 0xcu);
            if (rd16(r[0] + 0x12u) == 0u)
                goto done;
            r[1] = r[6];
            goto second;
        }
    }
    if (!(rd8(0x1086dc1cu) & 1u)) {
        wr32(0x1086dc1cu, rd32(0x1086dc1cu) | r[5]);
        wr32(0x1086dc20u, 0x200000u);
    }
    r[1] = rd32(0x1086dc20u);
    if (!(r[1] & r[0])) {
        c->xmm[0][0] = c->xmm[0][1] = c->xmm[0][2] = c->xmm[0][3] = 0u;
        wr32(r[7], 0u);
        goto done;
    }
    r[0] = rd32(r[7] + 0xcu);
    r[1] = r[3] + 0x40u;
second:
    r[0] += 0xd8u;
    sphere = r[0];
    cu_push(c, sphere);
    cu_call(c, 0x104034ffu, g3n_1040c5a0);
    wrf32(r[7], fto_float(c, ST(c, 0)));
    fdrop(c);
done:
    r[7] = cu_pop(c);
    r[6] = cu_pop(c);
    r[5] = cu_pop(c);
    r[3] = cu_pop(c);
    r[4] += 0xcu;
    cu_ret(c, 8);
}

/* void eCFrustumElementItem::CalculateLoDArea(eCFrustumDatabase &db), RET 4:
 * item+8 = db+0x1a4 * item+0 * entity+0x128 * [entity+0x2c]+0x48. EAX = [entity+0x2c],
 * EDX = &db. */
void g3n_10402f00(X86 *c) {
    if (!G3OPT(CULL)) {
        fn_10402f00(c);
        return;
    }
    const uint32_t self = c->r[1];
    c->r[2] = rd32(c->r[4] + 4u);
    double v = cu_ld(c->r[2] + 0x1a4u);
    c->r[0] = rd32(self + 0xcu);
    v = fx87(c, v * cu_ld(self));
    v = fx87(c, v * cu_ld(c->r[0] + 0x128u));
    c->r[0] = rd32(c->r[0] + 0x2cu);
    v = fx87(c, v * cu_ld(c->r[0] + 0x48u));
    wrf32(self + 8u, fto_float(c, v));
    cu_ffree_below(c, 1);
    cu_ret(c, 4);
}
