/* native/uv.c - the particles' vertex update (eCParticle_PS::UpdateVertices 103b3630), group "uv"
 * (G3OPT_UV).
 *
 * UpdateVertices is native already (native/particles.c, group particles), but every vector, matrix
 * and colour step of a particle's quad is a guest call into SharedBase, and the ones below were
 * still the translation. In the combat profile (work/sample-n10p1.txt) they are the hot part of
 * uv_particle's 131 samples: bCVector::Scale(float) 100103b0 13, GetTranslated 10011140 9,
 * SetCrossProduct 10010570 5, GetNormalized 10011080, operator- 10011040. They are leaves (or call
 * one native leaf), called from all over the game, so they are overrides of their own here: the
 * original's x87 sequence through fx87 and fto_float, its memory order (each component stored
 * before the next is read, so an output that overlaps an input comes out the same), the x87 slots
 * it used tagged empty, EAX/EDX/XMM as it leaves them. The SSE subtracts use the translation's own
 * host operation.
 *
 * Each g3n_<addr> runs its native version only under G3OPT(UV); otherwise it calls the original.
 * native/tests/uv_test.c compares each with the translation. */
#include "chunk.h"
#include "g3_opt.h"

void fn_100103b0(X86 *c); /* SharedBase bCVector::Scale(float) */
void fn_10010570(X86 *c); /* SharedBase bCVector::SetCrossProduct(a, b) */
void fn_10011140(X86 *c); /* SharedBase bCVector::GetTranslated(bCVector const &) const */
void fn_10011080(X86 *c); /* SharedBase bCVector::GetNormalized() const */
void fn_10011040(X86 *c); /* SharedBase bCVector::operator-() const */
void fn_10010060(X86 *c); /* SharedBase bCVector::Normalize (native in leaf.c) */
#ifndef FN_10010060
#define FN_10010060 fn_10010060
#endif

#define SB_ZERO_F 0x10090a48u /* the float operator- subtracts from */

static inline void uv_free(X86 *c, unsigned n) {
    for (unsigned i = 1; i <= n; ++i) {
        const unsigned p = (c->fpu_top - i) & 7u;
        ftag_put(c, p, FTAG_EMPTY);
        c->st_exact[p] = 0;
    }
}

static inline void uv_ret(X86 *c, uint32_t args) {
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u + args;
    recomp_return(c);
}

/* void bCVector::Scale(float s), RET 4: this[i] = this[i] * s, x stored before y is read. */
void g3n_100103b0(X86 *c) {
    if (!G3OPT(UV)) {
        fn_100103b0(c);
        return;
    }
    const uint32_t v = c->r[1];
    const double s = (double)rdf32(c->r[4] + 4u);
    wrf32(v, fto_float(c, fx87(c, (double)rdf32(v) * s)));
    wrf32(v + 4u, fto_float(c, fx87(c, (double)rdf32(v + 4u) * s)));
    wrf32(v + 8u, fto_float(c, fx87(c, s * (double)rdf32(v + 8u))));
    uv_free(c, 3);
    uv_ret(c, 4u);
}

/* void bCVector::SetCrossProduct(bCVector const &a, bCVector const &b), RET 8: this = a x b, each
 * component stored before the next one's operands are read. EAX = &a, EDX = &b. */
void g3n_10010570(X86 *c) {
    if (!G3OPT(UV)) {
        fn_10010570(c);
        return;
    }
    const uint32_t o = c->r[1], a = rd32(c->r[4] + 4u), b = rd32(c->r[4] + 8u);
    c->r[0] = a;
    c->r[2] = b;
    {
        const double p = fx87(c, (double)rdf32(a + 4u) * (double)rdf32(b + 8u));
        const double q = fx87(c, (double)rdf32(a + 8u) * (double)rdf32(b + 4u));
        wrf32(o, fto_float(c, fx87(c, p - q)));
    }
    {
        const double p = fx87(c, (double)rdf32(a + 8u) * (double)rdf32(b));
        const double q = fx87(c, (double)rdf32(a) * (double)rdf32(b + 8u));
        wrf32(o + 4u, fto_float(c, fx87(c, p - q)));
    }
    {
        const double p = fx87(c, (double)rdf32(a) * (double)rdf32(b + 4u));
        const double q = fx87(c, (double)rdf32(b) * (double)rdf32(a + 4u));
        wrf32(o + 8u, fto_float(c, fx87(c, p - q)));
    }
    uv_free(c, 2);
    uv_ret(c, 8u);
}

/* bCVector bCVector::GetTranslated(bCVector const &v) const, RET 8: out = v + this (FLD v / FADD
 * this), component by component. EAX = out, EDX = &v. */
void g3n_10011140(X86 *c) {
    if (!G3OPT(UV)) {
        fn_10011140(c);
        return;
    }
    const uint32_t t = c->r[1], out = rd32(c->r[4] + 4u), v = rd32(c->r[4] + 8u);
    c->r[2] = v;
    c->r[0] = out;
    for (uint32_t i = 0; i < 12u; i += 4u)
        wrf32(out + i, fto_float(c, fx87(c, (double)rdf32(v + i) + (double)rdf32(t + i))));
    uv_free(c, 1);
    uv_ret(c, 8u);
}

/* bCVector bCVector::operator-() const, RET 4: out[i] = 0.0f - this[i] by SUBSS (XMM0 = the 0.0f,
 * MOVAPS to XMM1 for x and y; z in XMM0). EAX = out. */
void g3n_10011040(X86 *c) {
    if (!G3OPT(UV)) {
        fn_10011040(c);
        return;
    }
    const uint32_t t = c->r[1];
    xmm_set_f32(c, 0, rdf32(SB_ZERO_F));
    c->xmm[0][1] = c->xmm[0][2] = c->xmm[0][3] = 0u;
    const uint32_t out = rd32(c->r[4] + 4u);
    c->r[0] = out;
    for (int i = 0; i < 4; ++i)
        c->xmm[1][i] = c->xmm[0][i];
    xmm_set_f32(c, 1, xmm_f32(c, 1) - rdf32(t));
    wrf32(out, xmm_f32(c, 1));
    for (int i = 0; i < 4; ++i)
        c->xmm[1][i] = c->xmm[0][i];
    xmm_set_f32(c, 1, xmm_f32(c, 1) - rdf32(t + 4u));
    xmm_set_f32(c, 0, xmm_f32(c, 0) - rdf32(t + 8u));
    wrf32(out + 4u, xmm_f32(c, 1));
    wrf32(out + 8u, xmm_f32(c, 0));
    uv_ret(c, 4u);
}

/* bCVector bCVector::GetNormalized() const, RET 4: out = this (FLD/FSTP each), then
 * out.Normalize() (10010060, a guest call from the original's frame: PUSH ESI, the return site
 * 1001109c); EAX = out. */
void g3n_10011080(X86 *c) {
    if (!G3OPT(UV)) {
        fn_10011080(c);
        return;
    }
    const uint32_t t = c->r[1], e = c->r[4], out = rd32(e + 4u);
    wr32(e - 4u, c->r[6]);
    for (uint32_t i = 0; i < 12u; i += 4u)
        wrf32(out + i, fto_float(c, (double)rdf32(t + i)));
    uv_free(c, 1);
    c->r[6] = out;
    c->r[1] = out;
    wr32(e - 8u, 0x1001109cu);
    c->r[4] = e - 8u;
    FN_10010060(c);
    c->r[0] = c->r[6];
    c->r[6] = rd32(e - 4u); /* POP ESI */
    c->r[4] = e;
    uv_ret(c, 4u);
}
