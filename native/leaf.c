/* native/leaf.c - small math leaves called per entity and per light.
 *
 * Each g3n_<addr> stands in for the translated fn_<addr> (native/g3_overrides.h) and runs its
 * native version only under G3OPT(LEAF); otherwise it calls the original.
 *
 * The rules are vector.c's: the original's x87 arithmetic step for step with the kit's helpers
 * (fx87 for each FADD/FSUB/FMUL/FDIV/FSQRT result, fto_float for every FSTP to a float, fdivz,
 * IE from FCOMI on any NaN), a value the original keeps in an x87 register is a local double, a
 * value it spills to a float stack slot is a local float, guest memory is read and written in
 * the original's order, and the x87 stack ends as the original leaves it: the top where it was,
 * the slots the original pushed into tagged empty (ffree_below). Registers end as the
 * translation leaves them, ECX and EDX included, and so do the EFLAGS fields the translation
 * writes last (most of these end on ADD ESP, which the translator keeps).
 *
 * Spill slots: the original's stack slots are locals here, which is only right while the frame
 * does not overlap the data the function reads or writes. Each function that keeps them so checks
 * that once and calls the original when it does (never in practice: the frame is below the
 * caller's ESP). The others write their slots in the original's order.
 * The final value of every stack slot the original writes is stored at the end, as the
 * original leaves it, though it is below the final ESP and dead.
 *
 * SSE: the one SSE leaf (10299740) repeats the translation's MULPS/ADDPS lane by lane in host
 * float arithmetic, operands in the source order, without FP contraction (the pragma below), so
 * no multiply and add fuse. For inputs without a NaN that is the translation bit for bit (an
 * infinity's invalid operation gives the host's one default NaN either way). With a NaN in an
 * input it is a NaN in the same lanes, but its payload is not the translation's: there the
 * translation's own answer depends on the operand order clang picks for the host fmul/fadd, and
 * it changes with the optimisation level (the translated function at -O0, -O1 and -O2 disagrees
 * with itself on most matrices that hold a NaN).
 *
 * native/tests/leaf_test.c runs each against the translation: registers, EFLAGS, x87 state,
 * XMM and memory, bit for bit (but for those SSE NaN payloads). */
#include "chunk.h"
#include "g3_opt.h"

#include <stdlib.h>

#pragma STDC FP_CONTRACT OFF

void fn_10010060(X86 *c); /* SharedBase bCVector::Normalize */
void fn_10011bd0(X86 *c); /* SharedBase bCEulerAngles::operator- */
void fn_10015be0(X86 *c); /* SharedBase bCMatrix::bCMatrix(bCMatrix const &) */
void fn_1001da30(X86 *c); /* SharedBase bCSphere::Intersects(bCSphere const &) */
void fn_10295f40(X86 *c); /* Engine FUN_10295f40 (leaf, __fastcall float *) */
void fn_10299740(X86 *c); /* Engine FUN_10299740 (leaf, __fastcall float *) */
void fn_102995d0(X86 *c); /* Engine FUN_102995d0 (float) */

#define SB_SQRT_FASTFLAG 0x100d4118u  /* the CRT sqrt goes to its other exit when this is set */
#define SB_SQRT_NAME 0x100d3c50u      /* "sqrt", ECX as the CRT's __math_exit leaves it */
#define SB_NORMALIZE_SQRT_RET 0x100100a9u
#define SB_SQRT_RET1 0x1008558bu      /* after CALL 0x100868c8 */
#define SB_SQRT_RET2 0x10085590u      /* after CALL 0x1008559d */
#define SB_SQRT_RET3 0x100855b5u      /* after CALL 0x10086855 (control word other than 0x27f) */
#define ENGINE_ONE_F 0x1068612cu      /* float 1.0f, the quaternion matrix's [3][3] */
#define ENGINE_SCALE_IAT 0x105f6684u  /* SharedBase bCVector::Scale(float) */
#define ENGINE_SCALE_RET 0x102995f0u

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

/* FCOMI/FCOMIP: IE on any NaN. Returns a > b (JA taken, JBE not taken). */
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

/* [a, a+an) and [b, b+bn) share a byte (guest addresses, no wrap). */
static inline int overlaps(uint32_t a, uint32_t an, uint32_t b, uint32_t bn) {
    return (uint64_t)a < (uint64_t)b + bn && (uint64_t)b < (uint64_t)a + an;
}

/* EFLAGS of ADD r32, imm and SUB r32, imm, as the translation computes them. */
static inline void flags_add32(X86 *c, uint32_t a, uint32_t b) {
    const uint64_t rf = (uint64_t)a + b;
    const uint32_t r = (uint32_t)rf;
    c->eflags_cf = (uint32_t)(rf >> 32) & 1u;
    c->eflags_of = ((~(a ^ b) & (a ^ r)) >> 31) & 1u;
    c->eflags_af = ((a ^ b ^ r) >> 4) & 1u;
    c->eflags_zf = r == 0;
    c->eflags_sf = r >> 31;
    c->eflags_pf = parity8(r);
}
static inline void flags_sub32(X86 *c, uint32_t a, uint32_t b) {
    const uint64_t rf = (uint64_t)a - b;
    const uint32_t r = (uint32_t)rf;
    c->eflags_cf = (uint32_t)(rf >> 32) & 1u;
    c->eflags_of = (((a ^ b) & (a ^ r)) >> 31) & 1u;
    c->eflags_af = ((a ^ b ^ r) >> 4) & 1u;
    c->eflags_zf = r == 0;
    c->eflags_sf = r >> 31;
    c->eflags_pf = parity8(r);
}

static inline uint32_t f2u(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* void bCVector::Normalize(), thiscall, RET. Frame: PUSH EBP, EBP = ESP0-4, ESP aligned to 8,
 * 0x14 bytes, PUSH ESI; sp is the ESP after that. x, y, z go through float slots, their squares
 * sum through a float slot into the CRT's sqrt (CALL 0x10085580), and a length > 0 scales the
 * vector by the float 1/length.
 *
 * The sqrt is done here for its common path: a non-negative argument that is not a NaN, the
 * CRT's fast flag clear, and no pending precision exception the CRT's exit would report. That
 * path, as the
 * translation runs it (chunk_10084000.c: 10085580, 100868c8, 1008559d, 10086855, 100868eb), is
 * FSQRT under the control word (cw & 0x300) | 0x7f (the same precision control, so the same
 * rounding) with cw restored after, ECX = the name "sqrt", and EDX the slot PUSH EDX / FSTCW /
 * the new control word made of it. A NaN (the CRT's error path, errno) and the other cases run
 * the whole original instead, decided before anything is written. */
void g3n_10010060(X86 *c) {
    if (!G3OPT(LEAF)) {
        fn_10010060(c);
        return;
    }
    const uint32_t self = c->r[1]; /* MOV ESI,ECX */
    const uint32_t esp0 = c->r[4];
    const uint32_t ebp = esp0 - 4u;
    const uint32_t sp = ((ebp & 0xfffffff8u) - 0x14u) - 4u;
    /* the frame down to the sqrt's deepest slot: [sp-0x1c, esp0) */
    if (overlaps(sp - 0x1cu, esp0 - (sp - 0x1cu), self, 12u)) {
        fn_10010060(c);
        return;
    }
    const uint16_t sw0 = c->fpu_sw;
    const float fy = fto_float(c, ld(self + 4u));
    const float fx = fto_float(c, ld(self));
    const float fz = fto_float(c, ld(self + 8u));
    const double xx = fx87(c, (double)fx * (double)fx);
    const double yy = fx87(c, (double)fy * (double)fy);
    const double xy = fx87(c, yy + xx);
    const double zz = fx87(c, (double)fz * (double)fz);
    const float s = fto_float(c, fx87(c, xy + zz));
    const uint32_t sb = f2u(s);
    const uint16_t cw = c->fpu_cw;
    /* Sign clear and not a NaN (the float's bits say what the double's would). +inf takes the
     * CRT's common exit without its FSQRT, which leaves the same infinity. */
    if ((sb & 0x80000000u) || ((sb & 0x7f800000u) == 0x7f800000u && (sb & 0x007fffffu)) ||
        rd32(SB_SQRT_FASTFLAG) != 0 ||
        (cw != 0x027fu && (cw & 0x20u) && (c->fpu_sw & 0x20u))) {
        c->fpu_sw = sw0;
        fn_10010060(c);
        return;
    }
    const uint32_t edx0 = c->r[2];
    const double r = sb == 0x7f800000u ? (double)s : fx87(c, sqrt((double)s));
    const float len = fto_float(c, r);
    /* the CRT's frame: ESP_b = sp - 0x10 holds the double argument */
    wr32(sp - 4u, SB_NORMALIZE_SQRT_RET);
    wrf64(sp - 0x10u, (double)s);
    wr32(sp - 0x14u, SB_SQRT_RET2);
    uint32_t edx;
    if (cw != 0x027fu && sb != 0x7f800000u) { /* +inf skips the control word change */
        edx = ((uint32_t)((cw & 0x300u) | 0x7fu) << 16) | cw;
        wr32(sp - 0x1cu, SB_SQRT_RET3);
    } else {
        edx = (edx0 & 0xffff0000u) | cw;
    }
    wr32(sp - 0x18u, edx);
    c->r[1] = SB_SQRT_NAME;
    c->r[2] = edx;
    c->r[0] = self;
    /* FLDZ, FLD len, FCOMI ST0,ST1, FSTP ST1, JBE */
    fcomi(c, (double)len, 0.0);
    float last = len;
    if (!c->eflags_cf && !c->eflags_zf) {
        const float inv = fto_float(c, fx87(c, fdivz(c, 1.0, (double)len)));
        wrf32(self, fto_float(c, fx87(c, (double)fx * (double)inv)));
        wrf32(self + 4u, fto_float(c, fx87(c, (double)fy * (double)inv)));
        wrf32(self + 8u, fto_float(c, fx87(c, (double)inv * (double)fz)));
        last = inv;
    }
    wr32(ebp, c->r[5]);
    wr32(sp, c->r[6]);
    wrf32(sp + 8u, fx);
    wrf32(sp + 0xcu, fy);
    wrf32(sp + 0x10u, fz);
    wrf32(sp + 0x14u, last);
    ffree_below(c, 4);
    ret_pop(c, 0);
}

/* bCEulerAngles operator-(bCEulerAngles const &) const, thiscall, RET 8: [ESP+4] = result,
 * [ESP+8] = the other; result[i] = this[i] - other[i]. EAX = result, EDX = other. */
void g3n_10011bd0(X86 *c) {
    if (!G3OPT(LEAF)) {
        fn_10011bd0(c);
        return;
    }
    const uint32_t a = c->r[1];
    const uint32_t b = rd32(c->r[4] + 8u);
    const uint32_t out = rd32(c->r[4] + 4u);
    for (uint32_t i = 0; i < 12u; i += 4u) {
        const double x = ld(a + i);
        wrf32(out + i, fto_float(c, fx87(c, x - ld(b + i))));
    }
    c->r[2] = b;
    c->r[0] = out;
    ffree_below(c, 1);
    ret_pop(c, 8);
}

/* bCMatrix::bCMatrix(bCMatrix const &), thiscall, RET 4: sixteen dwords, first to last, through
 * ECX (so a destination just above the source repeats what was already copied, as it does in
 * the original). EAX = this, ECX = the last dword, EDX = source + 64; the flags are TEST ESI,ESI
 * on the count's 0. */
void g3n_10015be0(X86 *c) {
    if (!G3OPT(LEAF)) {
        fn_10015be0(c);
        return;
    }
    const uint32_t esp0 = c->r[4];
    const uint32_t dst = c->r[1];
    uint32_t src = rd32(esp0 + 4u);
    wr32(esp0 - 4u, c->r[6]);
    wr32(esp0 - 8u, c->r[7]);
    uint32_t v = 0;
    for (int i = 0; i < 16; ++i, src += 4u) {
        v = rd32(src);
        wr32(dst + (uint32_t)(i * 4), v);
    }
    c->r[0] = dst;
    c->r[1] = v;
    c->r[2] = src;
    c->eflags_cf = 0;
    c->eflags_of = 0;
    c->eflags_zf = 1;
    c->eflags_sf = 0;
    c->eflags_pf = 1;
    ret_pop(c, 4);
}

/* bool bCSphere::Intersects(bCSphere const &o) const, thiscall, RET 4. Sphere = {r, center[3]}.
 * d = o.center - center through three float slots, rs = o.r + r through the argument slot; true
 * iff rs*rs (through the slot) > |d|^2 (through the slot). EAX = &o with AL the answer. */
void g3n_1001da30(X86 *c) {
    if (!G3OPT(LEAF)) {
        fn_1001da30(c);
        return;
    }
    const uint32_t self = c->r[1], esp0 = c->r[4];
    const uint32_t o = rd32(esp0 + 4u);
    if (overlaps(esp0 - 0xcu, 0x14u, o, 16u) || overlaps(esp0 - 0xcu, 0x14u, self, 16u)) {
        fn_1001da30(c);
        return;
    }
    const float ox = fto_float(c, ld(o + 4u));
    const float oy = fto_float(c, ld(o + 8u));
    const float oz = fto_float(c, ld(o + 0xcu));
    const float dx = fto_float(c, fx87(c, (double)ox - ld(self + 4u)));
    const float dy = fto_float(c, fx87(c, (double)oy - ld(self + 8u)));
    const float dz = fto_float(c, fx87(c, (double)oz - ld(self + 0xcu)));
    const double orad = ld(o);
    const float rs = fto_float(c, fx87(c, orad + ld(self)));
    const float rr = fto_float(c, fx87(c, (double)rs * (double)rs));
    const double yy = fx87(c, (double)dy * (double)dy);
    const double xx = fx87(c, (double)dx * (double)dx);
    const double xy = fx87(c, xx + yy);
    const double zz = fx87(c, (double)dz * (double)dz);
    const float d2 = fto_float(c, fx87(c, zz + xy));
    const int in = fgt(c, (double)rr, (double)d2);
    wrf32(esp0 - 0xcu, dx);
    wrf32(esp0 - 8u, dy);
    wrf32(esp0 - 4u, dz);
    wrf32(esp0 + 4u, d2);
    c->r[0] = (o & 0xffffff00u) | (in ? 1u : 0u);
    flags_add32(c, esp0 - 0xcu, 0xcu);
    ffree_below(c, 5);
    ret_pop(c, 4);
}

/* FUN_10295f40: ECX = a quaternion {x, y, z, w}, EAX = a 4x4 float matrix it fills: the 3x3
 * rotation in rows 0-2, zeros in column 3 and row 3, 1.0f at [3][3] (the zeros and the one
 * through XMM0, before the quaternion's y is read). Each product goes through a float slot; the
 * slots hold, at the end, zz, ww, xx, yy, zw, xy, zx, yw, xw, zy from ESP up. RET. */
void g3n_10295f40(X86 *c) {
    if (!G3OPT(LEAF)) {
        fn_10295f40(c);
        return;
    }
    const uint32_t q = c->r[1], m = c->r[0], esp0 = c->r[4];
    if (overlaps(esp0 - 0x28u, 0x28u, q, 16u) || overlaps(esp0 - 0x28u, 0x28u, m, 0x40u)) {
        fn_10295f40(c);
        return;
    }
    const float x = fto_float(c, ld(q));
    c->xmm[0][0] = c->xmm[0][1] = c->xmm[0][2] = c->xmm[0][3] = 0u;
    const float zero = 0.0f;
    wrf32(m + 0xcu, zero);
    wrf32(m + 0x1cu, zero);
    wrf32(m + 0x2cu, zero);
    const float xx = fto_float(c, fx87(c, (double)x * (double)x));
    wrf32(m + 0x30u, zero);
    wrf32(m + 0x34u, zero);
    wrf32(m + 0x38u, zero);
    const float one = rdf32(ENGINE_ONE_F);
    xmm_set_f32(c, 0, one);
    const double qy = ld(q + 4u);
    wrf32(m + 0x3cu, one);
    const float y = fto_float(c, qy);
    const float xy = fto_float(c, fx87(c, (double)y * (double)x));
    const float yy = fto_float(c, fx87(c, (double)y * (double)y));
    const float z = fto_float(c, ld(q + 8u));
    const float zx = fto_float(c, fx87(c, (double)z * (double)x));
    const float zy = fto_float(c, fx87(c, (double)z * (double)y));
    const float zz = fto_float(c, fx87(c, (double)z * (double)z));
    const float w = fto_float(c, ld(q + 0xcu));
    const float xw = fto_float(c, fx87(c, (double)x * (double)w));
    const float yw = fto_float(c, fx87(c, (double)y * (double)w));
    const float zw = fto_float(c, fx87(c, (double)z * (double)w));
    const float ww = fto_float(c, fx87(c, (double)w * (double)w));
    double t;
    t = fx87(c, (double)xx - (double)yy);
    t = fx87(c, t - (double)zz);
    wrf32(m, fto_float(c, fx87(c, t + (double)ww)));
    t = fx87(c, (double)zw + (double)xy);
    t = fx87(c, t + (double)xy);
    wrf32(m + 4u, fto_float(c, fx87(c, t + (double)zw)));
    t = fx87(c, (double)zx - (double)yw);
    t = fx87(c, t + (double)zx);
    wrf32(m + 8u, fto_float(c, fx87(c, t - (double)yw)));
    t = fx87(c, (double)xy - (double)zw);
    t = fx87(c, (double)xy + t);
    wrf32(m + 0x10u, fto_float(c, fx87(c, t - (double)zw)));
    t = fx87(c, (double)yy - (double)xx);
    t = fx87(c, t - (double)zz);
    wrf32(m + 0x14u, fto_float(c, fx87(c, t + (double)ww)));
    t = fx87(c, (double)xw + (double)zy);
    t = fx87(c, t + (double)zy);
    wrf32(m + 0x18u, fto_float(c, fx87(c, t + (double)xw)));
    t = fx87(c, (double)yw + (double)zx);
    t = fx87(c, (double)zx + t);
    wrf32(m + 0x20u, fto_float(c, fx87(c, (double)yw + t)));
    t = fx87(c, (double)zy - (double)xw);
    t = fx87(c, (double)zy + t);
    wrf32(m + 0x24u, fto_float(c, fx87(c, t - (double)xw)));
    t = fx87(c, (double)xx + (double)yy);
    t = fx87(c, (double)zz - t);
    wrf32(m + 0x28u, fto_float(c, fx87(c, t + (double)ww)));
    const uint32_t sp = esp0 - 0x28u;
    wrf32(sp, zz);
    wrf32(sp + 0x4u, ww);
    wrf32(sp + 0x8u, xx);
    wrf32(sp + 0xcu, yy);
    wrf32(sp + 0x10u, zw);
    wrf32(sp + 0x14u, xy);
    wrf32(sp + 0x18u, zx);
    wrf32(sp + 0x1cu, yw);
    wrf32(sp + 0x20u, xw);
    wrf32(sp + 0x24u, zy);
    flags_add32(c, sp, 0x28u);
    ffree_below(c, 8);
    ret_pop(c, 0);
}

typedef struct {
    float v[4];
} V4;

static inline V4 v4_load(uint32_t a) {
    uint32_t w[4] = {rd32(a), rd32(a + 4u), rd32(a + 8u), rd32(a + 12u)};
    V4 r;
    memcpy(r.v, w, 16);
    return r;
}
static inline void v4_store(uint32_t a, V4 x) {
    uint32_t w[4];
    memcpy(w, x.v, 16);
    wr32(a, w[0]);
    wr32(a + 4u, w[1]);
    wr32(a + 8u, w[2]);
    wr32(a + 12u, w[3]);
}
static inline void v4_to_xmm(X86 *c, int reg, V4 x) {
    memcpy(c->xmm[reg], x.v, 16);
}
/* MOVAPS t,s / SHUFPS t,s,imm with every selector the same lane: s[i] in all four. */
static inline V4 v4_splat(V4 s, int i) {
    V4 r = {{s.v[i], s.v[i], s.v[i], s.v[i]}};
    return r;
}
static inline V4 v4_mul(V4 a, V4 b) {
    V4 r;
    for (int i = 0; i < 4; ++i)
        r.v[i] = a.v[i] * b.v[i];
    return r;
}
static inline V4 v4_add(V4 a, V4 b) {
    V4 r;
    for (int i = 0; i < 4; ++i)
        r.v[i] = a.v[i] + b.v[i];
    return r;
}

/* FUN_10299740: EAX = a 4x4 float matrix A, ECX = B; A = A * B, row by row with SSE:
 * row i = ((a[i][1]*B1 + a[i][0]*B0) + a[i][2]*B2) + a[i][3]*B3, every row and B read before the
 * first store. B3 and B1 go through two aligned stack slots ([ESP+0x10], [ESP]) and come back
 * from there after row 0 is stored. The XMM registers end as the original leaves them. The flags
 * are SUB ESP,0x20's. RET. */
/* Off for now (the original runs): with a NaN in either matrix the native NaN's payload or sign can
 * differ from the translation's, which itself depends on how clang orders the SSE operands, and the
 * gain is x1.3. RECOMP_G3_LEAF_SSE=1 turns the native version on. */
static int leaf_sse_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_G3_LEAF_SSE");
        on = e && *e == '1';
    }
    return on;
}
void g3n_10299740(X86 *c) {
    if (!G3OPT(LEAF) || !leaf_sse_on()) {
        fn_10299740(c);
        return;
    }
    const uint32_t a = c->r[0], b = c->r[1], esp0 = c->r[4];
    const uint32_t ebp = esp0 - 4u;
    const uint32_t sp = (ebp & 0xfffffff0u) - 0x20u;
    if (overlaps(sp, esp0 - sp, a, 0x40u)) {
        fn_10299740(c);
        return;
    }
    wr32(ebp, c->r[5]);
    const V4 a0 = v4_load(a);
    const V4 b3 = v4_load(b + 0x30u);
    const V4 b1 = v4_load(b + 0x10u);
    const V4 b0 = v4_load(b);
    const V4 b2 = v4_load(b + 0x20u);
    const V4 a1 = v4_load(a + 0x10u);
    const V4 a2 = v4_load(a + 0x20u);
    const V4 a3 = v4_load(a + 0x30u);
    v4_store(sp + 0x10u, b3);
    v4_store(sp, b1);
    V4 r = v4_add(v4_mul(v4_splat(a0, 1), b1), v4_mul(v4_splat(a0, 0), b0));
    r = v4_add(r, v4_mul(v4_splat(a0, 2), b2));
    const V4 x0 = v4_load(sp + 0x10u); /* MOVAPS XMM0,[ESP+0x10] */
    r = v4_add(r, v4_mul(v4_splat(a0, 3), x0));
    v4_store(a, r);
    const V4 s1 = v4_load(sp); /* MULPS XMM6,[ESP], then MOVAPS XMM1,[ESP] */
    r = v4_add(v4_mul(v4_splat(a1, 1), s1), v4_mul(v4_splat(a1, 0), b0));
    r = v4_add(r, v4_mul(v4_splat(a1, 2), b2));
    const V4 x1 = v4_load(sp);
    r = v4_add(r, v4_mul(v4_splat(a1, 3), x0));
    v4_store(a + 0x10u, r);
    V4 r2 = v4_add(v4_mul(v4_splat(a2, 1), x1), v4_mul(v4_splat(a2, 0), b0));
    r2 = v4_add(r2, v4_mul(v4_splat(a2, 2), b2));
    V4 r3 = v4_add(v4_mul(v4_splat(a3, 1), x1), v4_mul(v4_splat(a3, 0), b0));
    r3 = v4_add(r3, v4_mul(v4_splat(a3, 2), b2));
    const V4 x7 = v4_mul(v4_splat(a2, 3), x0);
    r2 = v4_add(r2, x7);
    const V4 x1e = v4_mul(v4_splat(a3, 3), x0);
    v4_store(a + 0x20u, r2);
    r3 = v4_add(r3, x1e);
    v4_store(a + 0x30u, r3);
    v4_to_xmm(c, 0, x0);
    v4_to_xmm(c, 1, x1e);
    v4_to_xmm(c, 2, r3);
    v4_to_xmm(c, 3, a3);
    v4_to_xmm(c, 4, b0);
    v4_to_xmm(c, 5, b2);
    v4_to_xmm(c, 6, r2);
    v4_to_xmm(c, 7, x7);
    flags_sub32(c, ebp & 0xfffffff0u, 0x20u);
    ret_pop(c, 0);
}

/* FUN_102995d0(float s): ESI = a vector it sets to the three floats at EAX (each through FLD/FSTP)
 * and then scales by s through SharedBase bCVector::Scale(float) (thiscall on ESI, the float in
 * the slot PUSH ECX made, the import called as the translation calls it). EAX = ESI, RET 4. */
void g3n_102995d0(X86 *c) {
    if (!G3OPT(LEAF)) {
        fn_102995d0(c);
        return;
    }
    static uint64_t ic_scale;
    const uint32_t src = c->r[0], dst = c->r[6], esp0 = c->r[4];
    const double x = ld(src);
    wr32(esp0 - 4u, c->r[1]);
    wrf32(dst, fto_float(c, x));
    const double y = ld(src + 4u);
    wrf32(dst + 4u, fto_float(c, y));
    const double z = ld(src + 8u);
    wrf32(dst + 8u, fto_float(c, z));
    const double s = ld(esp0 + 4u);
    wrf32(esp0 - 4u, fto_float(c, s));
    ffree_below(c, 1);
    c->r[1] = dst;
    c->r[4] = esp0 - 8u;
    wr32(esp0 - 8u, ENGINE_SCALE_RET);
    recomp_call_cached(c, rd32(ENGINE_SCALE_IAT), &ic_scale);
    c->r[0] = c->r[6];
    ret_pop(c, 4);
}
