/* native/vector.c - SharedBase vector and matrix primitives.
 *
 * Each g3n_<addr> stands in for the translated fn_<addr> (native/g3_overrides.h) and runs its
 * native version only under G3OPT(VEC); otherwise it calls the original.
 *
 * These are leaves called per particle, per culled item, per bone: the work is a handful of float
 * multiplies, and the translation's cost is the x87 register stack around them (a tag update,
 * st_bits and st_exact on every push, pop and FXCH) and the flags of the integer instructions in
 * between. The native versions repeat the original's x87 arithmetic step for step with the kit's
 * own helpers: fx87 for each FADD/FSUB/FMUL/FDIV result (rounded to float under PC = single, the
 * game's setting; a NaN result becomes the x87 indefinite and raises IE), fto_float for every FSTP
 * to a float (rounding control), fdivz for FDIV, and IE from FCOMI on any NaN and from FUCOMI on a
 * signalling one. A value the original keeps in an x87 register is a local double here. Memory is
 * read and written in the original's order, so an output that overlaps an input comes out the
 * same. The x87 stack ends as the original leaves it: the top where it was (or one lower for a
 * float result), the slots the original pushed into tagged empty.
 *
 * Stack bytes the original writes below its final ESP (spill slots, the argument slots RET n
 * pops) are dead once it returns. The short functions still write the last value there; the
 * cylinder test writes only what the guest call it makes reads.
 *
 * native/tests/vector_test.c runs each against the translation: registers, x87 state and memory,
 * bit for bit. */
#include "chunk.h"
#include "g3_opt.h"

void fn_10011c00(X86 *c); /* SharedBase operator* (vector by scalar) */
void fn_10010e20(X86 *c); /* SharedBase bCVector::GetSquareMagnitude */
void fn_100103d0(X86 *c); /* SharedBase bCVector::Scale */
void fn_100103f0(X86 *c); /* SharedBase Translate */
void fn_1001ae70(X86 *c); /* SharedBase InvTranslate */
void fn_100138d0(X86 *c); /* SharedBase bCMatrix3::ModifyXAxis */
void fn_10016b10(X86 *c); /* SharedBase bCMatrix::SetToProduct */
void fn_1001b9e0(X86 *c); /* SharedBase bCBox::Merge(bCVector) */
void fn_1003a9b0(X86 *c); /* SharedBase bCInfDoubleCylinder::Cull */
void fn_10042f30(X86 *c); /* SharedBase bCSphere::Cull */
void fn_10035fd0(X86 *c); /* SharedBase bCFrustum::Cull */
void fn_1000f810(X86 *c); /* SharedBase bCVector2::Normalize (the cylinder test calls it) */

#ifndef FN_1000f810
#define FN_1000f810 fn_1000f810
#endif

#define SB_CYL_SCALE 0x10090ba0u      /* double 0.93f the cylinder test scales the radii by */
#define SB_PRODUCT_GUARD 0x1011dcc8u  /* SetToProduct's static temp: init guard bit 0 */
#define SB_PRODUCT_TEMP 0x1011dcd0u   /* the 4x4 float temp itself */
#define SB_CYL_NORMALIZE_RET 0x1003ab7au /* the cylinder test's return site after CALL Normalize */

/* The original's pushes and pops leave ST(-1)..ST(-n) of the entry stack empty. */
static inline void ffree_below(X86 *c, unsigned n) {
    for (unsigned i = 1; i <= n; ++i) {
        const unsigned p = (c->fpu_top - i) & 7u;
        ftag_put(c, p, FTAG_EMPTY);
        c->st_exact[p] = 0;
    }
}

/* FLD float ptr [a]. */
static inline double ld(uint32_t a) {
    return (double)rdf32(a);
}

/* FSTP float ptr [slot] then FLD float ptr [slot]. */
static inline double spill(const X86 *c, double v) {
    return (double)fto_float(c, v);
}

/* FCOMI/FCOMIP: IE on any NaN. Returns a > b, the one outcome every caller here branches on
 * (JA taken, JBE not taken). */
static inline int fgt(X86 *c, double a, double b) {
    if (a != a || b != b)
        c->fpu_sw |= 0x0001u;
    return a > b;
}

static inline void ret_pop(X86 *c, uint32_t args) {
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u + args;
    recomp_return(c);
}

static inline void xmm_load_bits(X86 *c, int reg, uint32_t bits) {
    c->xmm[reg][0] = bits;
    c->xmm[reg][1] = c->xmm[reg][2] = c->xmm[reg][3] = 0u;
}

static inline uint32_t f2u(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* bCVector operator*(bCVector const &, float), thiscall on the vector, RET 8:
 * [ESP+4] = result, [ESP+8] = scalar. EAX = result. */
void g3n_10011c00(X86 *c) {
    if (!G3OPT(VEC)) {
        fn_10011c00(c);
        return;
    }
    const uint32_t self = c->r[1];
    const uint32_t out = rd32(c->r[4] + 4u);
    const double x = ld(self);
    const double s = ld(c->r[4] + 8u);
    wrf32(out, fto_float(c, fx87(c, x * s)));
    const double y = ld(self + 4u);
    wrf32(out + 4u, fto_float(c, fx87(c, y * s)));
    const double z = ld(self + 8u);
    wrf32(out + 8u, fto_float(c, fx87(c, s * z)));
    c->r[0] = out;
    ffree_below(c, 3);
    ret_pop(c, 8);
}

/* float bCVector::GetSquareMagnitude() const: x*x + y*y + z*z through a float stack slot, left
 * in ST0. The slot is PUSH ECX / POP ECX, so ECX ends as the float's bits. */
void g3n_10010e20(X86 *c) {
    if (!G3OPT(VEC)) {
        fn_10010e20(c);
        return;
    }
    const uint32_t self = c->r[1];
    const double y = ld(self + 4u);
    const double x = ld(self);
    const double z = ld(self + 8u);
    const double xx = fx87(c, x * x);
    const double yy = fx87(c, y * y);
    const double s = fx87(c, fx87(c, yy + xx) + fx87(c, z * z));
    const float f = fto_float(c, s);
    wrf32(c->r[4] - 4u, f);
    c->r[1] = f2u(f);
    ffree_below(c, 4);
    fpush(c, (double)f);
    ret_pop(c, 0);
}

/* void bCVector::Scale(bCVector const &), RET 4: this[i] = v[i] * this[i]. EAX = &v. */
void g3n_100103d0(X86 *c) {
    if (!G3OPT(VEC)) {
        fn_100103d0(c);
        return;
    }
    const uint32_t self = c->r[1], v = rd32(c->r[4] + 4u);
    for (uint32_t i = 0; i < 12u; i += 4u) {
        const double a = ld(v + i);
        wrf32(self + i, fto_float(c, fx87(c, a * ld(self + i))));
    }
    c->r[0] = v;
    ffree_below(c, 1);
    ret_pop(c, 4);
}

/* void Translate(bCVector const &), RET 4: this[i] = v[i] + this[i]. EAX = &v. */
void g3n_100103f0(X86 *c) {
    if (!G3OPT(VEC)) {
        fn_100103f0(c);
        return;
    }
    const uint32_t self = c->r[1], v = rd32(c->r[4] + 4u);
    for (uint32_t i = 0; i < 12u; i += 4u) {
        const double a = ld(v + i);
        wrf32(self + i, fto_float(c, fx87(c, a + ld(self + i))));
    }
    c->r[0] = v;
    ffree_below(c, 1);
    ret_pop(c, 4);
}

/* void InvTranslate(bCVector const &), RET 4: this[i] = this[i] - v[i]. EAX = &v. */
void g3n_1001ae70(X86 *c) {
    if (!G3OPT(VEC)) {
        fn_1001ae70(c);
        return;
    }
    const uint32_t self = c->r[1], v = rd32(c->r[4] + 4u);
    for (uint32_t i = 0; i < 12u; i += 4u) {
        const double a = ld(self + i);
        wrf32(self + i, fto_float(c, fx87(c, a - ld(v + i))));
    }
    c->r[0] = v;
    ffree_below(c, 1);
    ret_pop(c, 4);
}

/* "ModifyXAxis" (a COMDAT-folded three-float copy through FLD/FSTP), RET 4: this[i] = v[i]. */
void g3n_100138d0(X86 *c) {
    if (!G3OPT(VEC)) {
        fn_100138d0(c);
        return;
    }
    const uint32_t self = c->r[1], v = rd32(c->r[4] + 4u);
    for (uint32_t i = 0; i < 12u; i += 4u)
        wrf32(self + i, fto_float(c, ld(v + i)));
    c->r[0] = v;
    ffree_below(c, 1);
    ret_pop(c, 4);
}

/* SetToProduct's loop: dst[i][j] = a[0][j]*b[i][0] + a[1][j]*b[i][1] + a[2][j]*b[i][2] +
 * a[3][j]*b[i][3], from a zeroed dst, each partial sum through the float scratch slot and stored
 * to dst before the next product is read, as the original does. Returns the last scratch value. */
static float mat_product(X86 *c, uint32_t dst, uint32_t a, uint32_t b) {
    float s = 0.0f;
    for (uint32_t i = 0; i < 4u; ++i) {
        const uint32_t bi = b + 16u * i;
        for (uint32_t j = 0; j < 4u; ++j) {
            const uint32_t e = dst + 16u * i + 4u * j, aj = a + 4u * j;
            double t = fx87(c, fx87(c, ld(aj) * ld(bi)) + ld(e));
            s = fto_float(c, t);
            wrf32(e, s);
            t = fx87(c, (double)s + fx87(c, ld(aj + 0x10u) * ld(bi + 4u)));
            s = fto_float(c, t);
            wrf32(e, s);
            t = fx87(c, (double)s + fx87(c, ld(bi + 8u) * ld(aj + 0x20u)));
            s = fto_float(c, t);
            wrf32(e, s);
            t = fx87(c, (double)s + fx87(c, ld(aj + 0x30u) * ld(bi + 12u)));
            wrf32(e, fto_float(c, t));
        }
    }
    return s;
}

/* bCMatrix &bCMatrix::SetToProduct(bCMatrix const &a, bCMatrix const &b), RET 8. With this
 * distinct from both, the product goes straight into this; otherwise into a function-static temp
 * that is then copied over this. The temp's first use registers an atexit, left to the original. */
void g3n_10016b10(X86 *c) {
    if (!G3OPT(VEC)) {
        fn_10016b10(c);
        return;
    }
    const uint32_t self = c->r[1], esp = c->r[4];
    const uint32_t a = rd32(esp + 4u), b = rd32(esp + 8u);
    const int alias = self == a || self == b;
    if (alias && !(rd8(SB_PRODUCT_GUARD) & 1u)) {
        fn_10016b10(c);
        return;
    }
    const uint32_t dst = alias ? SB_PRODUCT_TEMP : self;
    /* XORPS XMM0,XMM0 / MOVSS [ESP+0x14],XMM0 / MOV EAX,[ESP+0x14] and sixteen MOV [dst],EAX. */
    c->xmm[0][0] = c->xmm[0][1] = c->xmm[0][2] = c->xmm[0][3] = 0u;
    wr32(esp + 4u, 0u);
    for (uint32_t k = 0; k < 64u; k += 4u)
        wr32(dst + k, 0u);
    wrf32(esp + 4u, mat_product(c, dst, a, b));
    /* the pushes of EBX EBP ESI EDI */
    wr32(esp - 4u, c->r[3]);
    wr32(esp - 8u, c->r[5]);
    wr32(esp - 12u, c->r[6]);
    wr32(esp - 16u, c->r[7]);
    if (!alias) {
        c->r[0] = self + 0x40u;
        c->r[1] = a + 0x30u;
        c->r[2] = b + 0x48u;
    } else {
        uint32_t w = 0;
        for (uint32_t k = 0; k < 64u; k += 4u) {
            w = rd32(SB_PRODUCT_TEMP + k);
            wr32(self + k, w);
        }
        c->r[0] = SB_PRODUCT_TEMP + 0x40u;
        c->r[1] = 0u;
        c->r[2] = w;
    }
    ffree_below(c, 2);
    ret_pop(c, 8);
}

/* void bCBox::Merge(bCVector const &p), RET 4. Box = {min[3], max[3]}. Each p[i] goes through
 * the argument slot as a float; max[i] = p[i] if p[i] > max[i], then min[i] = p[i] if
 * min[i] > p[i] (FCOMIP: a NaN on either side stores nothing and raises IE). The stores are
 * MOVSS through XMM0. p is re-read for the min pass, after the max stores. */
void g3n_1001b9e0(X86 *c) {
    if (!G3OPT(VEC)) {
        fn_1001b9e0(c);
        return;
    }
    const uint32_t self = c->r[1], slot = c->r[4] + 4u;
    const uint32_t p = rd32(slot);
    float pf = 0.0f;
    for (uint32_t i = 0; i < 12u; i += 4u) {
        pf = fto_float(c, ld(p + i));
        const double mx = ld(self + 0xcu + i);
        if (fgt(c, (double)pf, mx)) {
            xmm_load_bits(c, 0, f2u(pf));
            wrf32(self + 0xcu + i, pf);
        }
    }
    for (uint32_t i = 0; i < 12u; i += 4u) {
        pf = fto_float(c, ld(p + i));
        const double mn = ld(self + i);
        if (fgt(c, mn, (double)pf)) {
            xmm_load_bits(c, 0, f2u(pf));
            wrf32(self + i, pf);
        }
    }
    wrf32(slot, pf);
    c->r[2] = p;
    ffree_below(c, 2);
    ret_pop(c, 4);
}

/* bool bCSphere::Cull(bCVector const &p) const, RET 4. Sphere = {r, center[3]}.
 * d = p - center through float slots, true iff r*r > |d|^2 (both through a float slot). */
void g3n_10042f30(X86 *c) {
    if (!G3OPT(VEC)) {
        fn_10042f30(c);
        return;
    }
    const uint32_t self = c->r[1], esp = c->r[4];
    const uint32_t p = rd32(esp + 4u);
    /* FLD/FSTP copies of p into [ESP0-12..-4], then the differences in place */
    const float px = fto_float(c, ld(p)), py = fto_float(c, ld(p + 4u)), pz = fto_float(c, ld(p + 8u));
    const float dx = fto_float(c, fx87(c, (double)px - ld(self + 4u)));
    const float dy = fto_float(c, fx87(c, (double)py - ld(self + 8u)));
    const float dz = fto_float(c, fx87(c, (double)pz - ld(self + 0xcu)));
    wrf32(esp - 12u, dx);
    wrf32(esp - 8u, dy);
    wrf32(esp - 4u, dz);
    const double r = ld(self);
    const double yy = fx87(c, (double)dy * (double)dy);
    const double xx = fx87(c, (double)dx * (double)dx);
    const double xy = fx87(c, xx + yy);
    const double d2 = spill(c, fx87(c, xy + fx87(c, (double)dz * (double)dz)));
    const float rr = fto_float(c, fx87(c, r * r));
    wrf32(esp + 4u, rr);
    const int in = fgt(c, (double)rr, d2);
    c->r[0] = (p & 0xffffff00u) | (in ? 1u : 0u);
    ffree_below(c, 5);
    ret_pop(c, 4);
}

/* bool bCFrustum::Cull(bCVector const &p) const, RET 4. Six planes {n[3], d} at this, tested
 * from the last: dist = (n.x*x + n.y*y + n.z*z) through a float slot, + d through a float slot;
 * false at the first plane with 0 > dist (FCOMI: a NaN distance passes and raises IE). ECX and
 * EDX end as the loop leaves them. */
void g3n_10035fd0(X86 *c) {
    if (!G3OPT(VEC)) {
        fn_10035fd0(c);
        return;
    }
    const uint32_t self = c->r[1], esp = c->r[4];
    const uint32_t v = rd32(esp + 4u);
    const uint32_t xb = rd32(v), zb = rd32(v + 8u);
    const double y = ld(v + 4u), x = ld(v), z = ld(v + 8u);
    float dist = 0.0f;
    int k = 5, out = 0;
    for (; k >= 0; --k) {
        const uint32_t pl = self + 0x10u * (uint32_t)k;
        const double s = fx87(c, fx87(c, ld(pl) * x) + fx87(c, ld(pl + 4u) * y));
        const double f = spill(c, fx87(c, s + fx87(c, z * ld(pl + 8u))));
        dist = fto_float(c, fx87(c, f + ld(pl + 0xcu)));
        if (fgt(c, 0.0, (double)dist)) {
            out = 1;
            break;
        }
    }
    xmm_load_bits(c, 0, zb);
    wr32(esp - 8u, xb);
    wr32(esp - 4u, zb);
    wrf32(esp + 4u, dist);
    c->r[0] = (v & 0xffffff00u) | (out ? 0u : 1u);
    c->r[1] = self + 8u + 0x10u * (uint32_t)k;
    c->r[2] = (uint32_t)k;
    ffree_below(c, 6);
    ret_pop(c, 4);
}

/* bool bCInfDoubleCylinder::Cull(bCVector const &p, bool scaled) const, RET 8.
 * this = {r0, c0[3], r1, c1[3]}; only x and z count. True if p is inside either end circle
 * (radii scaled by K when scaled == 1); otherwise p is projected on the axis through
 * bCVector2::Normalize (a guest call: it ends in the CRT's sqrt), and the result is whether the
 * projection falls strictly between the ends within the interpolated radius. A `scaled` byte
 * other than 0 or 1 skips the end circles and fails the radius test, as the original does.
 *
 * The x87 stack is modelled by depth only: `md` is the deepest the original's path went, and its
 * slots are tagged empty at the end (and before the guest call). The depths come from the
 * translation in build/recomp/gen/aux-sharedbase/chunk_10038000.c, instruction by instruction. */
void g3n_1003a9b0(X86 *c) {
    if (!G3OPT(VEC)) {
        fn_1003a9b0(c);
        return;
    }
    const uint32_t self = c->r[1], esp0 = c->r[4];
    const uint32_t p = rd32(esp0 + 4u);
    const uint32_t bl = rd8(esp0 + 8u);
    const uint32_t pxb = rd32(p), pzb = rd32(p + 8u);
    float pxf, pzf;
    c->r[0] = p;
    memcpy(&pxf, &pxb, 4);
    memcpy(&pzf, &pzb, 4);
    xmm_load_bits(c, 0, pxb);
    xmm_load_bits(c, 1, pzb);
    const double px = (double)pxf, pz = (double)pzf;
    const double dx0 = spill(c, fx87(c, px - ld(self + 4u)));
    const double dz0 = spill(c, fx87(c, pz - ld(self + 0xcu)));
    const double dx1 = spill(c, fx87(c, px - ld(self + 0x14u)));
    const double dz1 = spill(c, fx87(c, pz - ld(self + 0x1cu)));
    const double k = rdf64(SB_CYL_SCALE);
    unsigned md = 2; /* FLDZ, FLD K */
    uint32_t al;

    if (bl == 1u) {
        const double z0 = fx87(c, 0.0 * 0.0);
        double a = fx87(c, fx87(c, dx0 * dx0) + z0);
        const double r0 = ld(self);
        double f = spill(c, fx87(c, fx87(c, dz0 * dz0) + a));
        double g = spill(c, fx87(c, fx87(c, fx87(c, r0 * r0) * k) * k));
        md = 6;
        if (fgt(c, g, f)) {
            al = 1;
            goto done;
        }
        a = fx87(c, z0 + fx87(c, dx1 * dx1));
        const double r1 = ld(self + 0x10u);
        f = spill(c, fx87(c, a + fx87(c, dz1 * dz1)));
        g = spill(c, fx87(c, k * fx87(c, fx87(c, r1 * r1) * k)));
        if (fgt(c, g, f)) {
            al = 1;
            goto done;
        }
    } else if (bl == 0u) {
        const double z0 = fx87(c, 0.0 * 0.0);
        const double a = fx87(c, fx87(c, dx0 * dx0) + z0);
        const double r0 = ld(self);
        double f = spill(c, fx87(c, fx87(c, dz0 * dz0) + a));
        double g = spill(c, fx87(c, r0 * r0));
        md = 5;
        if (fgt(c, g, f)) {
            al = 1;
            goto done;
        }
        const double a1 = fx87(c, z0 + fx87(c, dx1 * dx1));
        const double r1 = ld(self + 0x10u);
        f = spill(c, fx87(c, a1 + fx87(c, dz1 * dz1)));
        g = spill(c, fx87(c, r1 * r1));
        if (fgt(c, g, f)) {
            al = 1;
            goto done;
        }
    }

    /* 1003ab3b: the axis. Frame as the original builds it: EBP = ESP0-4, S = the ESP after
     * PUSH EBX / PUSH ESI below the aligned 0x30 bytes. */
    {
        const uint32_t ebp = esp0 - 4u;
        const uint32_t s = ((ebp & 0xfffffff8u) - 0x30u) - 8u;
        const uint32_t save_ebx = c->r[3], save_esi = c->r[6], save_ebp = c->r[5];
        const float c0xf = fto_float(c, ld(self + 4u));
        const float exf = fto_float(c, fx87(c, ld(self + 0x14u) - (double)c0xf));
        const float c0zf = fto_float(c, ld(self + 0xcu));
        const float ezf = fto_float(c, fx87(c, ld(self + 0x1cu) - (double)c0zf));
        wr32(ebp, save_ebp);
        wr32(s + 4u, save_ebx);
        wr32(s, save_esi);
        wrf32(s + 0x2cu, fto_float(c, -(double)ezf));
        wrf32(s + 0x30u, fto_float(c, (double)exf));
        ffree_below(c, md);
        md = 0;
        c->r[0] = p;
        c->r[1] = s + 0x2cu;
        c->r[3] = (save_ebx & 0xffffff00u) | bl;
        c->r[5] = ebp;
        c->r[6] = self;
        c->r[4] = s - 4u;
        wr32(c->r[4], SB_CYL_NORMALIZE_RET);
        FN_1000f810(c);
        c->r[4] = esp0;
        c->r[3] = save_ebx;
        c->r[6] = save_esi;
        c->r[5] = save_ebp;

        const double n0 = ld(s + 0x2cu), n1 = ld(s + 0x30u);
        const double pxs = spill(c, px), pzs = spill(c, pz);
        const double un = fx87(c, fx87(c, pxs - ld(self + 4u)) * n0);
        const double vn = fx87(c, fx87(c, pzs - ld(self + 0xcu)) * n1);
        const double dd = spill(c, fx87(c, vn + un));
        /* FUCOMIP 0 against exf, then LAHF: EAX's AH is the comparison's flags. */
        fucomi(c, 0.0, (double)exf);
        c->r[0] = (c->r[0] & 0xffff00ffu) | (((x86_get_eflags(c) & 0xd5u) | 0x02u) << 8);
        md = 8;
        double t;
        if (exf == 0.0f) {
            const double w = fx87(c, pzs - fx87(c, n1 * dd));
            t = spill(c, fx87(c, fdivz(c, fx87(c, w - (double)c0zf), (double)ezf)));
        } else {
            const double w = fx87(c, pxs - fx87(c, n0 * dd));
            t = spill(c, fx87(c, fdivz(c, fx87(c, w - (double)c0xf), (double)exf)));
        }
        al = 0;
        if (fgt(c, t, 0.0) && fgt(c, 1.0, t) && bl <= 1u) {
            const double r0f = spill(c, ld(self));
            const double adf = spill(c, fabs(dd));
            const double r1 = ld(self + 0x10u);
            const double tdr = fx87(c, t * fx87(c, r1 - r0f));
            double lim;
            if (bl == 1u)
                lim = fx87(c, fx87(c, tdr * k) + fx87(c, r0f * k));
            else
                lim = fx87(c, tdr + r0f);
            const double limf = spill(c, lim);
            /* JNC / JC after FCOMIP limf, adf: true iff limf >= adf, ordered. */
            if (limf != limf || adf != adf)
                c->fpu_sw |= 0x0001u;
            al = limf >= adf;
        }
    }
done:
    c->r[0] = (c->r[0] & 0xffffff00u) | al;
    ffree_below(c, md);
    ret_pop(c, 8);
}
