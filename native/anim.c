/* native/anim.c - character animation: sampling and blending poses down the bone hierarchy.
 *
 * Each g3n_<addr> stands in for the translated fn_<addr> (native/g3_overrides.h) and runs its
 * native version only under G3OPT(ANIM); otherwise it calls the original. docs/perf/anim.md has
 * the profile (eCAnimation OnProcess 10292d40 -> 102a7a00 -> 1054d520 -> 1054d620 -> the pose
 * recursion 1055b5b0 and its leaves).
 *
 * The x87 code here is transcribed instruction by instruction onto a local register stack (LX
 * below): the same pushes, pops, exchanges and operations as the translation, each result through
 * the kit's helpers (fx87 for FADD/FSUB/FMUL/FDIV, fdivz, fto_float for every FSTP to a float,
 * fcomi/fucomi), stack slots the code stores and reloads kept as locals. At the end the real x87
 * stack is left as the original leaves it: the slots it pushed into tagged empty (LX tracks the
 * deepest it went) and its results pushed. The compiler keeps the local stack in registers; the
 * translation's cost was the guest stack's bookkeeping (tags, st_bits, st_exact on every push,
 * pop and exchange) and the flags. Stack bytes below the final ESP (spills, argument slots the
 * code overwrites) are dead once it returns and are not written. EFLAGS are not kept. */
#include "chunk.h"
#include "g3_opt.h"

#include <string.h>

void fn_1054a020(X86 *c); /* Engine FUN_1054a020: a track's pose copied out */
void fn_1054bf00(X86 *c); /* Engine FUN_1054bf00: quaternion blend in place (ECX = q0 and out, EDX = q1) */
void fn_1054b510(X86 *c); /* Engine FUN_1054b510: two packed key quaternions decoded and blended */
void fn_10540e00(X86 *c); /* Engine FUN_10540e00: a box grown by a point (fastcall) */
void fn_1055bce0(X86 *c); /* Engine FUN_1055bce0: quaternion blend (nlerp with the short arc) */

/* ---- a local x87 stack ------------------------------------------------------------------- */

typedef struct {
    double s[8]; /* s[0] the bottom */
    unsigned n, max;
} LX;

static inline void lx_push(LX *f, double v) {
    f->s[f->n++] = v;
    if (f->n > f->max)
        f->max = f->n;
}
#define LST(f, i) ((f)->s[(f)->n - 1u - (i)])
static inline void lx_pop(LX *f) {
    f->n--;
}
static inline void lx_xch(LX *f, unsigned i) {
    const double t = LST(f, 0);
    LST(f, 0) = LST(f, i);
    LST(f, i) = t;
}
/* FSTP ST(i) */
static inline void lx_stp(LX *f, unsigned i) {
    LST(f, i) = LST(f, 0);
    f->n--;
}

/* The original's pushes and pops leave ST(-1)..ST(-n) of the entry stack empty. */
static inline void an_ffree_below(X86 *c, unsigned n) {
    if (n > 8u)
        n = 8u;
    for (unsigned i = 1; i <= n; ++i) {
        const unsigned p = (c->fpu_top - i) & 7u;
        ftag_put(c, p, FTAG_EMPTY);
        c->st_exact[p] = 0;
    }
}

/* Ends the local stack: the deepest slots empty, what is left on it pushed for the caller. */
static inline void lx_end(X86 *c, const LX *f) {
    an_ffree_below(c, f->max);
    for (unsigned i = 0; i < f->n; ++i)
        fpush(c, f->s[i]);
}

static inline double an_ld(uint32_t a) {
    return (double)rdf32(a);
}

/* FLD float / FSTP float through a slot: the float as stored. */
static inline double an_f(const X86 *c, double v) {
    return (double)fto_float(c, v);
}

static inline void an_ret(X86 *c, uint32_t args) {
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u + args;
    recomp_return(c);
}

static inline void an_xmm(X86 *c, int reg, uint32_t bits) {
    c->xmm[reg][0] = bits;
    c->xmm[reg][1] = c->xmm[reg][2] = c->xmm[reg][3] = 0u;
}

static inline float an_bf(uint32_t b) {
    float f;
    memcpy(&f, &b, 4);
    return f;
}

/* ---- FUN_1054a020: a track's pose ------------------------------------------------------------ */

/* bool FUN_1054a020(int track, float out[10]), thiscall, RET 8. Track k is 0x3c bytes at
 * [this+0x6c] + 0x3c*k; a negative +4 means it has no pose (AL = 0), otherwise its ten floats
 * from +0x14 (a quaternion and two vectors) go to out through FLD/FSTP. EAX = the track's address
 * with AL as the result, ECX = out, EDX = 15 * k. */
void g3n_1054a020(X86 *c) {
    if (!G3OPT(ANIM)) {
        fn_1054a020(c);
        return;
    }
    const uint32_t k = rd32(c->r[4] + 4u);
    c->r[2] = (k << 4) - k;
    const uint32_t t = rd32(c->r[1] + 0x6cu) + c->r[2] * 4u;
    if ((int32_t)rd32(t + 4u) < 0) {
        c->r[0] = t & 0xffffff00u;
        an_ret(c, 8);
        return;
    }
    const uint32_t out = rd32(c->r[4] + 8u);
    c->r[1] = out;
    for (uint32_t i = 0; i < 40u; i += 4u)
        wrf32(out + i, fto_float(c, an_ld(t + 0x14u + i)));
    c->r[0] = (t & 0xffffff00u) | 1u;
    an_ffree_below(c, 1);
    an_ret(c, 8);
}

/* ---- FUN_1055bce0: quaternion blend --------------------------------------------------------- */

/* void FUN_1055bce0(q0 = EAX, out = EDX; float const *q1, float t), RET 8: out = q0 * (1 - t) +
 * q1 * t' componentwise, t' = -0.0f - t (SUBSS) when the two are more than 90 degrees apart
 * (dot < 0), else t. Every component through a float slot first; the dot product as the
 * original sums it. EAX = out, ECX = q1, XMM0 = q0.y or t'. */
void g3n_1055bce0(X86 *c) {
    if (!G3OPT(ANIM)) {
        fn_1055bce0(c);
        return;
    }
    const uint32_t esp0 = c->r[4], q0 = c->r[0], out = c->r[2];
    const uint32_t q1 = rd32(esp0 + 4u);
    const uint32_t tbits = rd32(esp0 + 8u);
    LX f = {{0}, 0, 0};
    lx_push(&f, (double)an_bf(tbits));    /* FLD t */
    c->r[1] = q1;
    lx_push(&f, LST(&f, 0));              /* FLD ST0 */
    const uint32_t a1b = rd32(q0 + 4u);   /* MOVSS XMM0,[EAX+4] */
    an_xmm(c, 0, a1b);
    lx_push(&f, 1.0);                     /* FLD1 */
    /* FSUBRP: ST1 = ST0 - ST1 */
    LST(&f, 1) = fx87(c, LST(&f, 0) - LST(&f, 1));
    lx_pop(&f);
    const double s1c = an_f(c, LST(&f, 0)); /* FSTP [ESP+0x1c]: 1 - t */
    lx_pop(&f);
    const double s14 = an_f(c, an_ld(q1 + 4u));
    const double s10 = an_f(c, an_ld(q0));
    const double s0c = an_f(c, an_ld(q1));
    const double s00 = an_f(c, an_ld(q0 + 8u));
    const double s24 = an_f(c, an_ld(q1 + 8u));
    const double s08 = an_f(c, an_ld(q0 + 0xcu));
    const double s04 = an_f(c, an_ld(q1 + 0xcu));
    lx_push(&f, s0c);
    lx_push(&f, LST(&f, 0));
    lx_push(&f, s10);
    lx_push(&f, LST(&f, 0));
    LST(&f, 2) = fx87(c, LST(&f, 2) * LST(&f, 0)); /* FMULP ST2 */
    lx_pop(&f);
    lx_push(&f, s14);
    lx_push(&f, LST(&f, 0));
    lx_push(&f, (double)an_bf(a1b)); /* [ESP+0x18] = q0.y by MOVSS */
    lx_push(&f, LST(&f, 0));
    LST(&f, 2) = fx87(c, LST(&f, 2) * LST(&f, 0));
    lx_pop(&f);
    lx_xch(&f, 4);
    LST(&f, 1) = fx87(c, LST(&f, 1) + LST(&f, 0)); /* FADDP */
    lx_pop(&f);
    lx_push(&f, s24);
    LST(&f, 0) = fx87(c, LST(&f, 0) * s00);
    LST(&f, 1) = fx87(c, LST(&f, 1) + LST(&f, 0));
    lx_pop(&f);
    lx_push(&f, s04);
    LST(&f, 0) = fx87(c, LST(&f, 0) * s08);
    LST(&f, 1) = fx87(c, LST(&f, 1) + LST(&f, 0));
    lx_pop(&f);
    const double dot = an_f(c, LST(&f, 0)); /* FSTP [ESP+0x18] */
    lx_pop(&f);
    lx_push(&f, dot);
    lx_push(&f, 0.0);
    fcomi(c, LST(&f, 0), LST(&f, 1)); /* FCOMIP ST0,ST1 */
    lx_pop(&f);
    lx_pop(&f);
    if (!(c->eflags_cf || c->eflags_zf)) {
        const uint32_t kb = rd32(0x106e849cu); /* -0.0f */
        lx_stp(&f, 4);
        const float tp = an_bf(kb) - an_bf(tbits); /* SUBSS XMM0,[ESP+0x28] */
        uint32_t tpb;
        memcpy(&tpb, &tp, 4);
        an_xmm(c, 0, tpb);
        lx_push(&f, (double)an_bf(tpb));
        lx_xch(&f, 4);
    }
    lx_push(&f, s1c);
    c->r[0] = out;
    lx_push(&f, LST(&f, 0));
    LST(&f, 5) = fx87(c, LST(&f, 5) * LST(&f, 0)); /* FMULP ST5 */
    lx_pop(&f);
    lx_push(&f, LST(&f, 5));
    LST(&f, 3) = fx87(c, LST(&f, 3) * LST(&f, 0)); /* FMULP ST3 */
    lx_pop(&f);
    lx_xch(&f, 4);
    LST(&f, 2) = fx87(c, LST(&f, 2) + LST(&f, 0)); /* FADDP ST2,ST0 */
    lx_pop(&f);
    lx_xch(&f, 1);
    wrf32(out, fto_float(c, LST(&f, 0)));
    lx_pop(&f);
    LST(&f, 0) = fx87(c, LST(&f, 0) * LST(&f, 2)); /* FMUL ST2 */
    lx_push(&f, LST(&f, 3));
    LST(&f, 2) = fx87(c, LST(&f, 2) * LST(&f, 0));
    lx_pop(&f);
    LST(&f, 1) = fx87(c, LST(&f, 1) + LST(&f, 0));
    lx_pop(&f);
    wrf32(out + 4u, fto_float(c, LST(&f, 0)));
    lx_pop(&f);
    lx_push(&f, s24);
    LST(&f, 0) = fx87(c, LST(&f, 0) * LST(&f, 1)); /* FMUL ST1 */
    lx_push(&f, s00);
    LST(&f, 0) = fx87(c, LST(&f, 0) * LST(&f, 3)); /* FMUL ST3 */
    LST(&f, 1) = fx87(c, LST(&f, 1) + LST(&f, 0));
    lx_pop(&f);
    wrf32(out + 8u, fto_float(c, LST(&f, 0)));
    lx_pop(&f);
    LST(&f, 0) = fx87(c, LST(&f, 0) * s04); /* FMUL [ESP+4] */
    lx_push(&f, s08);
    LST(&f, 2) = fx87(c, LST(&f, 2) * LST(&f, 0)); /* FMULP ST2 */
    lx_pop(&f);
    LST(&f, 1) = fx87(c, LST(&f, 1) + LST(&f, 0));
    lx_pop(&f);
    wrf32(out + 0xcu, fto_float(c, LST(&f, 0)));
    lx_pop(&f);
    lx_end(c, &f);
    c->r[4] = esp0;
    an_ret(c, 8);
}

/* ---- FUN_10540e00: a box grown by a point -------------------------------------------------- */

/* void __fastcall FUN_10540e00(float box[6] = ECX, float const p[3] = EDX): min = min(min, p),
 * max = max(max, p), each pair through two float slots and FCOMIP (a NaN on either side raises
 * IE and keeps p for the min, p for the max as the JBE falls through), the winner stored by
 * MOVSS from its slot. XMM0 ends as the last winner. */
void g3n_10540e00(X86 *c) {
    if (!G3OPT(ANIM)) {
        fn_10540e00(c);
        return;
    }
    const uint32_t box = c->r[1], p = c->r[2];
    uint32_t w = 0;
    for (uint32_t i = 0; i < 3u; ++i) { /* FLD A / FLD B / FCOMIP: B > A keeps A */
        const float a = fto_float(c, an_ld(box + 4u * i));
        const float b = fto_float(c, an_ld(p + 4u * i));
        fcomi(c, (double)b, (double)a);
        const float v = !(c->eflags_cf || c->eflags_zf) ? a : b;
        memcpy(&w, &v, 4);
        wr32(box + 4u * i, w);
    }
    for (uint32_t i = 0; i < 3u; ++i) { /* FLD B / FLD A / FCOMIP: A > B keeps A */
        const float a = fto_float(c, an_ld(box + 0xcu + 4u * i));
        const float b = fto_float(c, an_ld(p + 4u * i));
        fcomi(c, (double)a, (double)b);
        const float v = !(c->eflags_cf || c->eflags_zf) ? a : b;
        memcpy(&w, &v, 4);
        wr32(box + 0xcu + 4u * i, w);
    }
    an_xmm(c, 0, w);
    an_ffree_below(c, 2);
    an_ret(c, 0);
}

/* ---- FUN_1054bf00 / FUN_1054b510: blending between two packed keys ---------------------------- */

/* void FUN_1054bf00(q0 = ECX, q1 = EDX; float t), RET 4: q0 = q0 * (1 - t) + q1 * t' in place,
 * t' = -0.0f - t when dot(q0, q1) < 0. 1055bce0's sibling with another register use and load
 * order: q0's components and q1.x through float slots, q1.y..w read from memory where the
 * original reads them (after q0.x is written). XMM0 = t' when the arc was flipped. */
void g3n_1054bf00(X86 *c) {
    if (!G3OPT(ANIM)) {
        fn_1054bf00(c);
        return;
    }
    const uint32_t esp0 = c->r[4], a = c->r[1], b = c->r[2];
    const uint32_t tbits = rd32(esp0 + 4u);
    LX f = {{0}, 0, 0};
    lx_push(&f, (double)an_bf(tbits));
    lx_push(&f, LST(&f, 0));
    lx_push(&f, 1.0);
    LST(&f, 1) = fx87(c, LST(&f, 0) - LST(&f, 1)); /* FSUBRP */
    lx_pop(&f);
    const double s14 = an_f(c, LST(&f, 0));
    lx_pop(&f);
    const double s0c = an_f(c, an_ld(a + 4u));
    const double s08 = an_f(c, an_ld(b));
    const double s04 = an_f(c, an_ld(a));
    double s10 = an_f(c, an_ld(a + 8u));
    const double s00 = an_f(c, an_ld(a + 0xcu));
    lx_push(&f, s04);
    lx_push(&f, LST(&f, 0));
    lx_push(&f, s08);
    lx_push(&f, LST(&f, 0));
    LST(&f, 2) = fx87(c, LST(&f, 2) * LST(&f, 0)); /* FMULP ST2 */
    lx_pop(&f);
    lx_push(&f, an_ld(b + 4u));
    lx_push(&f, s0c);
    lx_push(&f, LST(&f, 0));
    LST(&f, 2) = fx87(c, LST(&f, 2) * LST(&f, 0));
    lx_pop(&f);
    lx_xch(&f, 3);
    LST(&f, 1) = fx87(c, LST(&f, 1) + LST(&f, 0)); /* FADDP */
    lx_pop(&f);
    lx_push(&f, an_ld(b + 8u));
    lx_push(&f, s10);
    lx_push(&f, LST(&f, 0));
    LST(&f, 2) = fx87(c, LST(&f, 2) * LST(&f, 0));
    lx_pop(&f);
    lx_xch(&f, 2);
    LST(&f, 1) = fx87(c, LST(&f, 1) + LST(&f, 0));
    lx_pop(&f);
    lx_push(&f, s00);
    LST(&f, 0) = fx87(c, LST(&f, 0) * an_ld(b + 0xcu)); /* FMUL [EDX+0xc] */
    LST(&f, 1) = fx87(c, LST(&f, 1) + LST(&f, 0));
    lx_pop(&f);
    s10 = an_f(c, LST(&f, 0)); /* FSTP [ESP+0x10]: the dot product */
    lx_pop(&f);
    lx_push(&f, s10);
    lx_push(&f, 0.0);
    fcomi(c, LST(&f, 0), LST(&f, 1)); /* FCOMIP */
    lx_pop(&f);
    lx_pop(&f);
    if (!(c->eflags_cf || c->eflags_zf)) {
        const uint32_t kb = rd32(0x106e849cu); /* -0.0f */
        lx_stp(&f, 4);
        const float tp = an_bf(kb) - an_bf(tbits); /* SUBSS XMM0,[ESP+0x1c] */
        uint32_t tpb;
        memcpy(&tpb, &tp, 4);
        an_xmm(c, 0, tpb);
        lx_push(&f, (double)an_bf(tpb));
        lx_xch(&f, 4);
    }
    lx_push(&f, s14);
    lx_push(&f, LST(&f, 0));
    LST(&f, 5) = fx87(c, LST(&f, 5) * LST(&f, 0)); /* FMULP ST5 */
    lx_pop(&f);
    lx_push(&f, LST(&f, 5));
    LST(&f, 3) = fx87(c, LST(&f, 3) * LST(&f, 0)); /* FMULP ST3 */
    lx_pop(&f);
    lx_xch(&f, 4);
    LST(&f, 2) = fx87(c, LST(&f, 2) + LST(&f, 0)); /* FADDP ST2,ST0 */
    lx_pop(&f);
    lx_xch(&f, 1);
    wrf32(a, fto_float(c, LST(&f, 0)));
    lx_pop(&f);
    lx_push(&f, an_ld(b + 4u));
    LST(&f, 0) = fx87(c, LST(&f, 0) * LST(&f, 4)); /* FMUL ST4 */
    lx_push(&f, LST(&f, 3));
    LST(&f, 3) = fx87(c, LST(&f, 3) * LST(&f, 0)); /* FMULP ST3 */
    lx_pop(&f);
    LST(&f, 2) = fx87(c, LST(&f, 2) + LST(&f, 0)); /* FADDP ST2,ST0 */
    lx_pop(&f);
    lx_xch(&f, 1);
    wrf32(a + 4u, fto_float(c, LST(&f, 0)));
    lx_pop(&f);
    lx_push(&f, an_ld(b + 8u));
    LST(&f, 0) = fx87(c, LST(&f, 0) * LST(&f, 3)); /* FMUL ST3 */
    lx_push(&f, LST(&f, 2));
    LST(&f, 2) = fx87(c, LST(&f, 2) * LST(&f, 0)); /* FMULP ST2 */
    lx_pop(&f);
    LST(&f, 1) = fx87(c, LST(&f, 1) + LST(&f, 0)); /* FADDP */
    lx_pop(&f);
    wrf32(a + 8u, fto_float(c, LST(&f, 0)));
    lx_pop(&f);
    LST(&f, 0) = fx87(c, LST(&f, 0) * s00); /* FMUL [ESP] */
    lx_push(&f, an_ld(b + 0xcu));
    LST(&f, 2) = fx87(c, LST(&f, 2) * LST(&f, 0)); /* FMULP ST2 */
    lx_pop(&f);
    LST(&f, 1) = fx87(c, LST(&f, 1) + LST(&f, 0)); /* FADDP */
    lx_pop(&f);
    wrf32(a + 0xcu, fto_float(c, LST(&f, 0)));
    lx_pop(&f);
    lx_end(c, &f);
    an_ret(c, 4);
}

/* void FUN_1054b510(int key, double t, float out[4]), thiscall, RET 0x10. Keys are 12 bytes at
 * [this+0xc]: the quaternion of key k and of key k+1 as int16 (+0 and +0xc), each component times
 * the double at 106e84a0 (FILD, FMUL); key k's goes to out, key k+1's to a local, then
 * 1054bf00(out, local, (float)t) blends them in place. EAX = key k+1's w as int, EDX = &local,
 * ECX = out (as 1054bf00 leaves them). */
void g3n_1054b510(X86 *c) {
    if (!G3OPT(ANIM)) {
        fn_1054b510(c);
        return;
    }
    const uint32_t esp0 = c->r[4];
    const uint32_t k = rd32(esp0 + 4u);
    const uint32_t key = rd32(c->r[1] + 0xcu) + k * 12u;
    const uint32_t out = rd32(esp0 + 0x10u);
    const double scale = rdf64(0x106e84a0u);
    const uint32_t s = esp0 - 0x10u; /* the frame after SUB ESP,0x10 */
    /* key k: FILD / FMUL ST1 (into ST1, then FXCH) for x, FMUL ST1 into ST0 for y, z, w */
    wrf32(out, fto_float(c, fx87(c, scale * (double)(int16_t)rd16(key))));
    wrf32(out + 4u, fto_float(c, fx87(c, (double)(int16_t)rd16(key + 2u) * scale)));
    wrf32(out + 8u, fto_float(c, fx87(c, (double)(int16_t)rd16(key + 4u) * scale)));
    wrf32(out + 0xcu, fto_float(c, fx87(c, (double)(int16_t)rd16(key + 6u) * scale)));
    /* key k+1 into the local at ESP+4 (after the PUSH ECX: s+0 .. s+0xc), read back by 1054bf00 */
    wrf32(s, fto_float(c, fx87(c, (double)(int16_t)rd16(key + 0xcu) * scale)));
    wrf32(s + 4u, fto_float(c, fx87(c, (double)(int16_t)rd16(key + 0xeu) * scale)));
    wrf32(s + 8u, fto_float(c, fx87(c, (double)(int16_t)rd16(key + 0x10u) * scale)));
    wrf32(s + 0xcu, fto_float(c, fx87(c, (double)(int16_t)rd16(key + 0x12u) * scale)));
    c->r[0] = (uint32_t)(int32_t)(int16_t)rd16(key + 0x12u);
    an_ffree_below(c, 2);
    /* FLD double t / FSTP float / FLD / FSTP [ESP]: t as a float in the pushed ECX slot */
    const float tf = fto_float(c, rdf64(esp0 + 8u));
    an_ffree_below(c, 1);
    c->r[4] = s - 4u;
    wrf32(c->r[4], tf);
    c->r[1] = out;
    c->r[2] = s;
    c->r[4] -= 4u;
    wr32(c->r[4], 0x1054b5d0u);
    g3n_1054bf00(c);
    c->r[4] += 0x10u;
    an_ret(c, 0x10);
}

/* ---- FUN_1054a580: a rotation track sampled at a time --------------------------------------- */

void fn_1054a580(X86 *c); /* Engine FUN_1054a580 */
void fn_1054a9a0(X86 *c); /* Engine FUN_1054a9a0: the key at a time, searched from scratch */
#ifndef FN_1054a9a0
#define FN_1054a9a0 fn_1054a9a0
#endif

/* Four packed int16 components times `scale` into the slots ESP+0xc..+0x18 and then out (FILD,
 * FMUL ST1, FSTP into the slot, later FLD/FSTP to out). The ints go through the slot [ESP+0x20]
 * as the original stages them; ECX/EDX end as its MOVSX leave them. `pop_last` is the second
 * copy's FMULP, which consumes the scale. */
static inline void ka_decode(X86 *c, LX *f, uint32_t key, uint32_t esp, uint32_t out) {
    double v[4];
    for (uint32_t i = 0; i < 4u; ++i) {
        const int32_t w = (int16_t)rd16(key + 2u * i);
        wr32(esp + 0x20u, (uint32_t)w);
        lx_push(f, (double)w);
        v[i] = an_f(c, fx87(c, LST(f, 0) * LST(f, 1)));
        lx_pop(f);
        wrf32(esp + 0xcu + 4u * i, (float)v[i]);
    }
    c->r[1] = (uint32_t)(int32_t)(int16_t)rd16(key + 4u);
    c->r[2] = (uint32_t)(int32_t)(int16_t)rd16(key + 6u);
    for (uint32_t i = 0; i < 4u; ++i)
        wrf32(out + 4u * i, fto_float(c, v[i]));
}

/* void FUN_1054a580(float t, float out[4]), EAX = &the cached key index, ESI = the track {+0 vtable,
 * +4 key count, +0xc keys of 12 bytes: packed quaternion, then the key's time at +8}, RET 8.
 * Moves the cached index forward or back until key i's time <= t <= key i+1's (or searches anew
 * through 1054a9a0 when it is out of range); before the first key (index -1) and past the last
 * the last key's quaternion goes to out; then the track's own blend (vtable +0xc: key, the
 * fraction (t - t_i) / (t_i+1 - t_i) as a double, out) through the guest.
 *
 * The original's instructions minus EFLAGS. Its x87 stack keeps t from the search to the
 * fraction; the slots it reaches are emptied before each guest call, as its pops leave them. */
void g3n_1054a580(X86 *c) {
    if (!G3OPT(ANIM)) {
        fn_1054a580(c);
        return;
    }
    static uint64_t ic_blend;
    uint32_t *const r = c->r;
    const uint32_t esp0 = r[4];
    r[4] -= 0x10u;
    r[4] -= 4u;
    wr32(r[4], r[3]);
    r[3] = rd32(r[4] + 0x1cu); /* out */
    r[4] -= 4u;
    wr32(r[4], r[5]);
    r[4] -= 4u;
    wr32(r[4], r[7]);
    const uint32_t esp = r[4]; /* ESP0 - 0x1c */
    r[7] = r[0];
    r[1] = rd32(r[7]);
    LX f = {{0}, 0, 0};
    const double t = (double)rdf32(esp + 0x20u);
    int search = (int32_t)r[1] < 0;
    if (!search) {
        r[2] = rd32(r[6] + 4u);
        search = (int32_t)r[1] >= (int32_t)r[2];
    }
    if (search) {
        /* FLD t / PUSH ECX / MOV ECX,ESI / FSTP [ESP] / CALL 1054a9a0 */
        lx_push(&f, t);
        r[4] -= 4u;
        wr32(r[4], r[1]);
        r[1] = r[6];
        wrf32(r[4], fto_float(c, t));
        lx_pop(&f);
        an_ffree_below(c, f.max);
        f.max = 0;
        r[4] -= 4u;
        wr32(r[4], 0x1054a62bu);
        if (recomp_profile_enabled)
            recomp_call(c, 0x1054a9a0u);
        else
            FN_1054a9a0(c);
        lx_push(&f, (double)rdf32(r[4] + 0x20u));
        wr32(r[7], r[0]);
    } else {
        r[5] = rd32(r[6] + 0xcu);
        r[0] = r[1] * 3u;
        const double k = an_f(c, an_ld(r[5] + r[0] * 4u + 8u));
        wrf32(esp + 0x24u, (float)k);
        lx_push(&f, k);
        lx_push(&f, t);
        fcomi(c, LST(&f, 0), LST(&f, 1)); /* FCOMI t, k */
        if (!(c->eflags_cf || c->eflags_zf)) {
            /* t past key i: forward */
            r[5] = r[2] - 1u;
            lx_stp(&f, 1);
            if ((int32_t)r[1] < (int32_t)r[5]) {
                for (;;) {
                    r[1] = rd32(r[7]);
                    r[0] = rd32(r[6] + 0xcu);
                    r[2] = r[1] * 3u;
                    lx_push(&f, an_ld(r[0] + r[2] * 4u + 8u));
                    r[2] = r[0] + r[2] * 4u;
                    lx_xch(&f, 1);
                    fcomi(c, LST(&f, 0), LST(&f, 1));
                    lx_stp(&f, 1);
                    if (!c->eflags_cf) {
                        lx_push(&f, an_ld(r[2] + 0x14u));
                        fcomi(c, LST(&f, 0), LST(&f, 1));
                        lx_pop(&f);
                        if (!c->eflags_cf)
                            break;
                    }
                    r[1] += 1u;
                    wr32(r[7], r[1]);
                    if (!((int32_t)r[1] < (int32_t)r[5]))
                        break;
                }
            }
        } else {
            /* backward */
            lx_xch(&f, 1);
            fcomi(c, LST(&f, 0), LST(&f, 1)); /* FCOMIP k, t */
            lx_pop(&f);
            if (!(c->eflags_cf || c->eflags_zf) && (int32_t)r[1] > 0) {
                for (;;) {
                    r[1] = rd32(r[7]);
                    r[0] = rd32(r[6] + 0xcu);
                    r[2] = r[1] * 3u;
                    lx_push(&f, an_ld(r[0] + r[2] * 4u + 8u));
                    r[2] = r[0] + r[2] * 4u;
                    lx_xch(&f, 1);
                    fcomi(c, LST(&f, 0), LST(&f, 1));
                    lx_stp(&f, 1);
                    if (!c->eflags_cf) {
                        lx_push(&f, an_ld(r[2] + 0x14u));
                        fcomi(c, LST(&f, 0), LST(&f, 1));
                        lx_pop(&f);
                        if (!c->eflags_cf)
                            break;
                    }
                    r[1] -= 1u;
                    wr32(r[7], r[1]);
                    if (!((int32_t)r[1] > 0))
                        break;
                }
            }
        }
    }
    /* 1054a631: [t]; FLD scale */
    lx_push(&f, rdf64(0x106e84a0u));
    const uint32_t keys = rd32(r[6] + 0xcu);
    if (rd32(r[7]) == 0xffffffffu) {
        r[0] = rd32(r[6] + 4u);
        r[2] = keys;
        r[1] = r[0] * 3u;
        r[0] = r[2] + r[1] * 4u - 0xcu;
        ka_decode(c, &f, r[0], esp, r[3]);
    }
    r[0] = rd32(r[6] + 4u) - 1u;
    r[1] = rd32(r[7]) + 1u;
    if ((int32_t)r[1] <= (int32_t)r[0]) {
        lx_pop(&f); /* FSTP ST0: the scale */
    } else {
        r[0] = rd32(r[6] + 4u);
        r[2] = r[0] * 3u;
        r[0] = rd32(r[6] + 0xcu);
        r[0] = r[0] + r[2] * 4u - 0xcu;
        ka_decode(c, &f, r[0], esp, r[3]);
        lx_pop(&f); /* the last FMULP took the scale */
    }
    /* 1054a72f: the fraction */
    r[7] = rd32(r[7]);
    r[1] = rd32(r[6] + 0xcu);
    r[0] = r[7] * 3u;
    const double k0 = an_f(c, an_ld(r[1] + r[0] * 4u + 8u));
    r[0] = r[1] + r[0] * 4u;
    wrf32(esp + 0x20u, (float)k0);
    r[2] = rd32(r[6]);
    lx_push(&f, k0);
    r[4] = esp - 4u;
    wr32(r[4], r[3]);
    lx_push(&f, LST(&f, 0));
    r[4] -= 8u;
    LST(&f, 2) = fx87(c, LST(&f, 2) - LST(&f, 0)); /* FSUBP ST2,ST0: t - k0 */
    lx_pop(&f);
    r[1] = r[6];
    LST(&f, 0) = fx87(c, an_ld(r[0] + 0x14u) - LST(&f, 0)); /* FSUBR [EAX+0x14]: k1 - k0 */
    r[0] = rd32(r[2] + 0xcu);
    LST(&f, 1) = fx87(c, fdivz(c, LST(&f, 1), LST(&f, 0))); /* FDIVP */
    lx_pop(&f);
    const float q = fto_float(c, LST(&f, 0));
    wrf32(r[4] + 0x2cu, q);
    lx_pop(&f);
    wrf64(r[4], (double)q);
    an_ffree_below(c, f.max);
    r[4] -= 4u;
    wr32(r[4], r[7]);
    r[4] -= 4u;
    wr32(r[4], 0x1054a768u);
    recomp_call_cached(c, r[0], &ic_blend);
    r[7] = rd32(r[4]);
    r[5] = rd32(r[4] + 4u);
    r[3] = rd32(r[4] + 8u);
    r[4] += 0xcu + 0x10u;
    (void)esp0;
    an_ret(c, 8);
}
