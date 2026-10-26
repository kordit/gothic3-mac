/* native/particles.c - stage 1: particle update (PUP Opt.ParticleUpdate).
 *
 * Each g3n_<addr> stands in for the translated fn_<addr> (native/g3_overrides.h) and runs its
 * native version only under G3OPT(PARTICLES); otherwise it calls the original.
 *
 * The native versions repeat the original's x87 arithmetic step for step with the kit's own
 * rounding helpers: fx87 (to float under PC = single, the game's setting, unrounded otherwise),
 * fto_float for every FSTP to a float slot (rounding control), fdivz for FDIV, and the same
 * status-word side effects (IE from FCOMI on a NaN and from a NaN result). What the original keeps
 * in x87 registers is a local double here and what it spills to a float stack slot is rounded the
 * same way, so the results are the translation's bit for bit; native/tests/particles_test.c
 * checks that against the translated functions, registers, x87 state and memory.
 *
 * Native: eCColorScale::CalcValue, eCVectorScale::CalcValueLinear, eCParticle_PS::CalcColor and
 * CalcColorFactor, the leaves TickParticles calls per particle, and TickParticles and
 * UpdateVertices themselves. Their calls out (SharedBase vector, matrix and colour methods through
 * the IAT, vtable methods, TraceRayNearestHit, SpawnParticle, PlayCollisionSound, the vertex
 * buffer lock, the particle functions above) are guest calls made from host code, with the
 * original's return addresses, registers, stack and x87 state; see the comment above
 * g3n_103b5800. Engine callees go through FN_<addr>, so an override applies, but not through
 * CALL_FN: a hook or the profiler sees them as the native's own time, as with vector.c.
 * CalcExtents is in native/pdo.c (group pdo). */
#include "chunk.h"
#include "g3_opt.h"

void fn_103b5800(X86 *c); /* Engine TickParticles */
void fn_103bf2b0(X86 *c); /* Engine CalcValue */
void fn_103c0140(X86 *c); /* Engine CalcValueLinear */
void fn_103af5d0(X86 *c); /* Engine CalcColor */
void fn_103af790(X86 *c); /* Engine CalcColorFactor */
void fn_103afae0(X86 *c); /* Engine CalcExtents */
void fn_103b3630(X86 *c); /* Engine UpdateVertices */

#define ENGINE_ONE_F 0x1068612cu   /* float 1.0f the empty scales return */
#define ENGINE_ONE_D 0x10686158u   /* double CalcColor adds to the color speed */

/* The original's pushes and pops leave ST(-1)..ST(-n) of the entry stack empty. */
static inline void ftag_free_below(X86 *c, unsigned n) {
    for (unsigned i = 1; i <= n; ++i)
        ftag_put(c, (c->fpu_top - i) & 7u, FTAG_EMPTY);
}

/* FLD float, with the widening kept: clang would otherwise fold (float)(double)x back to x, and
 * a signalling NaN an FLD/FSTP pair quiets on x87 (7f800001 -> 7fc00001) would be stored as it
 * was. The translation itself does either, depending on how its block was optimised. */
static inline double fld32(uint32_t a) {
    double d = (double)rdf32(a);
#if defined(__aarch64__)
    __asm__("" : "+w"(d));
#else
    __asm__("" : "+m"(d));
#endif
    return d;
}

/* A value through FSTP float [slot] / FLD float [slot]. */
static inline double spill(const X86 *c, double v) {
    return (double)fto_float(c, v);
}

static inline void xmm_load_f32_bits(X86 *c, int reg, uint32_t bits) {
    c->xmm[reg][0] = bits;
    c->xmm[reg][1] = c->xmm[reg][2] = c->xmm[reg][3] = 0u;
}

/* The key search both scales share: keys of `stride` bytes, the time first. Leaves *lo = the
 * last key at or before t (0 if none), and returns EDX as the loop leaves it: the key equal to t,
 * the first key after t, or count when t is past the end. FUCOMIP for equality, then FCOMIP,
 * which raises IE on a NaN and lets the loop go on. */
static inline int32_t key_search(X86 *c, uint32_t keys, int32_t count, uint32_t stride, double t, int32_t *lo) {
    int32_t esi = 0, edx = 0;
    if (count > 0) {
        uint32_t p = keys;
        for (;;) {
            const double k = fld32(p);
            if (k == t) {
                esi = edx;
                break;
            }
            if (k != k || t != t)
                c->fpu_sw |= 0x0001u;
            else if (k > t)
                break;
            esi = edx;
            ++edx;
            p += stride;
            if (edx >= count)
                break;
        }
    }
    *lo = esi;
    return edx;
}

/* eCColorScale::CalcValue(float t, bCFloatAlphaColor &out) const, as far as RET: this = {key *keys,
 * int count}, key = {float t, r, g, b, a} (20 bytes), out's color at +4..+0x10. Sets EAX, ECX,
 * EDX, XMM0..2 and the x87 state as the original leaves them. */
static void calc_value_core(X86 *c, uint32_t self, float tf, uint32_t out) {
    const int32_t count = (int32_t)rd32(self + 4u);
    if (count == 0) {
        const uint32_t one = rd32(ENGINE_ONE_F);
        xmm_load_f32_bits(c, 0, one);
        wr32(out + 4u, one);
        wr32(out + 0xcu, one);
        wr32(out + 0x10u, one);
        wr32(out + 8u, one);
        c->r[0] = out;
        c->r[1] = self;
        return;
    }
    const double t = (double)tf;
    const uint32_t keys = rd32(self);
    int32_t esi;
    int32_t edx = key_search(c, keys, count, 20u, t, &esi);
    if (edx == count)
        edx = esi;
    const uint32_t lo = keys + (uint32_t)esi * 20u, hi = keys + (uint32_t)edx * 20u;
    const double klo = fld32(lo);
    const double d = spill(c, fx87(c, fld32(hi) - klo));
    c->r[2] = (uint32_t)edx;
    c->r[0] = out;
    if (esi != edx && !(d == 0.0)) {
        const double f = spill(c, fx87(c, fdivz(c, fx87(c, t - klo), d)));
        for (uint32_t i = 4u; i <= 0x10u; i += 4u) {
            const double l = fld32(lo + i);
            const double di = spill(c, fx87(c, fld32(hi + i) - l));
            const double p = spill(c, fx87(c, di * f));
            wrf32(out + i, fto_float(c, fx87(c, l + p)));
        }
        const float ff = (float)f;
        memcpy(&c->r[1], &ff, 4); /* POP ECX takes f from the slot PUSH ECX made */
        ftag_free_below(c, 5);
        return;
    }
    /* The key itself: red through the x87, the rest copied with MOVSS. */
    const float r = fto_float(c, fld32(lo + 4u));
    const uint32_t g = rd32(lo + 8u), b = rd32(lo + 0xcu), a = rd32(lo + 0x10u);
    xmm_load_f32_bits(c, 0, g);
    xmm_load_f32_bits(c, 1, b);
    xmm_load_f32_bits(c, 2, a);
    wrf32(out + 4u, r);
    wr32(out + 0xcu, b);
    wr32(out + 0x10u, a);
    wr32(out + 8u, g);
    c->r[1] = self;
    ftag_free_below(c, esi != edx ? 5 : 4);
}

void g3n_103bf2b0(X86 *c) {
    if (!G3OPT(PARTICLES)) {
        fn_103bf2b0(c);
        return;
    }
    calc_value_core(c, c->r[1], rdf32(c->r[4] + 4u), rd32(c->r[4] + 8u));
    c->eip = rd32(c->r[4]);
    c->r[4] += 12u;
    recomp_return(c);
}

/* eCVectorScale::CalcValueLinear(float t, bCVector &out) const, RET 8: key = {float t, x, y, z}. */
void g3n_103c0140(X86 *c) {
    if (!G3OPT(PARTICLES)) {
        fn_103c0140(c);
        return;
    }
    const uint32_t self = c->r[1];
    const uint32_t out = rd32(c->r[4] + 8u);
    const double t = fld32(c->r[4] + 4u);
    const int32_t count = (int32_t)rd32(self + 4u);
    const uint32_t keys = rd32(self);
    int32_t esi;
    const int32_t edx = key_search(c, keys, count, 16u, t, &esi);
    if (edx == count || edx == esi) {
        const uint32_t k = keys + (uint32_t)esi * 16u + 4u;
        wrf32(out, fto_float(c, fld32(k)));
        wrf32(out + 4u, fto_float(c, fld32(k + 4u)));
        wrf32(out + 8u, fto_float(c, fld32(k + 8u)));
        c->r[0] = k;
        c->r[2] = (uint32_t)edx;
        ftag_free_below(c, count > 0 ? 4 : 1);
    } else {
        const uint32_t lo = keys + (uint32_t)esi * 16u, hi = keys + (uint32_t)edx * 16u;
        const double klo = fld32(lo);
        const double d = spill(c, fx87(c, fld32(hi) - klo));
        if (d == 0.0) {
            wrf32(out, fto_float(c, fld32(lo + 4u)));
            wrf32(out + 4u, fto_float(c, fld32(lo + 8u)));
            wrf32(out + 8u, fto_float(c, fld32(lo + 0xcu)));
        } else {
            const double f = spill(c, fx87(c, fdivz(c, fx87(c, t - klo), d)));
            for (uint32_t i = 0; i < 0xcu; i += 4u) {
                const double l = fld32(lo + 4u + i);
                const double di = spill(c, fx87(c, fld32(hi + 4u + i) - l));
                const double p = spill(c, fx87(c, di * f));
                wrf32(out + i, fto_float(c, fx87(c, l + p)));
            }
            const float ff = (float)f;
            memcpy(&c->r[1], &ff, 4); /* POP ECX: f from the PUSH ECX slot */
        }
        c->r[0] = out;
        c->r[2] = hi;
        ftag_free_below(c, 5);
    }
    c->eip = rd32(c->r[4]);
    c->r[4] += 12u;
    recomp_return(c);
}

/* eCParticle_PS::CalcColor(float t, bCFloatAlphaColor &out) const, RET 8. Off ([this+0x220] == 0):
 * white. On: the color scale at this+0x3d4 at frac(t * (speed + 1)). */
void g3n_103af5d0(X86 *c) {
    if (!G3OPT(PARTICLES)) {
        fn_103af5d0(c);
        return;
    }
    const uint32_t self = c->r[1];
    const uint32_t out = rd32(c->r[4] + 8u);
    c->r[0] = out;
    if (rd8(self + 0x220u) == 0) {
        const uint32_t one = rd32(ENGINE_ONE_F);
        xmm_load_f32_bits(c, 0, one);
        wr32(out + 4u, one);
        wr32(out + 8u, one);
        wr32(out + 0xcu, one);
        wr32(out + 0x10u, one);
    } else {
        const double s = fx87(c, fld32(self + 0x224u) + rdf64(ENGINE_ONE_D));
        const float v = fto_float(c, fx87(c, s * fld32(c->r[4] + 4u)));
        c->r[2] = (uint32_t)(int32_t)v; /* CVTTSS2SI, as the translation converts it */
        const float frac = fto_float(c, fx87(c, (double)v - (double)(int32_t)c->r[2]));
        ftag_free_below(c, 1); /* one value deep before the call */
        calc_value_core(c, self + 0x3d4u, frac, out);
    }
    c->eip = rd32(c->r[4]);
    c->r[4] += 12u;
    recomp_return(c);
}

/* JBE after FCOMI a,b: a <= b, or unordered, which raises IE (FCOMI is the signalling compare). */
static inline int fcomi_be(X86 *c, double a, double b) {
    if (a != a || b != b) {
        c->fpu_sw |= 0x0001u;
        return 1;
    }
    return a <= b;
}
/* JC after FCOMI a,b: a < b, or unordered (IE). */
static inline int fcomi_c(X86 *c, double a, double b) {
    if (a != a || b != b) {
        c->fpu_sw |= 0x0001u;
        return 1;
    }
    return a < b;
}
static inline uint32_t fbits(float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}
static inline double fval(uint32_t b) {
    float f;
    memcpy(&f, &b, 4);
    return (double)f;
}

/* eCParticle_PS::CalcColorFactor(float t, float life, bCFloatAlphaColor &out) const, RET 0xc.
 * Two factors per channel, fade-in (flag +0x1ec, time +0x1f0, colour +0x1fc..+0x208) and fade-out
 * (flag +0x1ed, time +0x1f4, colour +0x210..+0x21c), each 1 unless its flag is on, multiplied
 * into out+4..+0x10. The locals are the original's stack slots: what it stores with MOVSS keeps its
 * bits, what it stores with FSTP goes through fto_float. XMM0 = 1.0f, XMM1 = 0 and XMM2 (the last
 * MOVSS load it made) end as the original leaves them. */
void g3n_103af790(X86 *c) {
    if (!G3OPT(PARTICLES)) {
        fn_103af790(c);
        return;
    }
    const uint32_t self = c->r[1];
    const uint32_t esp = c->r[4];
    const double t = fld32(esp + 4u);
    const uint32_t out = rd32(esp + 0xcu);
    const uint32_t one = rd32(ENGINE_ONE_F);
    uint32_t l0 = 0, l4 = 0;                          /* [ESP], [ESP+4]: the two factors */
    uint32_t lc = one, l14 = one, l1c = one, l24 = one; /* fade-in per channel */
    uint32_t l8 = one, l10 = one, l18 = one, l20 = one; /* fade-out per channel */
    unsigned depth = 3;                               /* FLD1, FLDZ, FLD t */
    xmm_load_f32_bits(c, 0, one);
    c->xmm[1][0] = c->xmm[1][1] = c->xmm[1][2] = c->xmm[1][3] = 0u;

    if (rd8(self + 0x1ecu)) {
        depth = 4;
        const float af = fto_float(c, fld32(self + 0x1f0u));
        const double a = (double)af;
        if (!fcomi_be(c, a, t)) {
            depth = 6;
            xmm_load_f32_bits(c, 2, fbits(af));
            if (af > 0.0f) { /* COMISS a, 0 (a is ordered here: it compared greater than t) */
                const float qf = fto_float(c, fx87(c, fdivz(c, t, a)));
                const double q = (double)qf;
                l0 = fbits(qf);
                uint32_t mb = one;
                if (fcomi_be(c, q, 1.0)) {
                    xmm_load_f32_bits(c, 2, l0);
                    mb = l0;
                }
                if (!fcomi_be(c, 0.0, fval(mb))) {
                    l0 = 0u;
                } else if (!fcomi_be(c, q, 1.0))
                    l0 = one;
                l0 = fbits(fto_float(c, fx87(c, 1.0 - fval(l0))));
            }
            const double f = fval(l0);
            const double p0 = fx87(c, fx87(c, 1.0 - fld32(self + 0x1fcu)) * f);
            lc = fbits(fto_float(c, fx87(c, 1.0 - p0)));
            l14 = fbits(fto_float(c, fx87(c, 1.0 - fx87(c, fx87(c, 1.0 - fld32(self + 0x200u)) * f))));
            l1c = fbits(fto_float(c, fx87(c, 1.0 - fx87(c, fx87(c, 1.0 - fld32(self + 0x204u)) * f))));
            l24 = fbits(fto_float(c, fx87(c, 1.0 - fx87(c, f * fld32(self + 0x208u)))));
        }
    }

    if (rd8(self + 0x1edu)) {
        if (depth < 4)
            depth = 4;
        const double b = (double)fto_float(c, fld32(self + 0x1f4u));
        if (!fcomi_c(c, t, b)) {
            if (depth < 5)
                depth = 5;
            const double life = fld32(esp + 8u);
            double f;
            if (fcomi_be(c, life, b))
                f = fval(l4);
            else {
                depth = 6;
                const double b2 = (double)fto_float(c, fld32(self + 0x1f4u));
                const double num = fx87(c, t - b2), den = fx87(c, life - b2);
                const float rf = fto_float(c, fx87(c, fdivz(c, num, den)));
                const double r = (double)rf;
                l4 = fbits(rf);
                uint32_t mb = one;
                if (fcomi_be(c, r, 1.0)) {
                    xmm_load_f32_bits(c, 2, l4);
                    mb = l4;
                }
                if (!fcomi_be(c, 0.0, fval(mb))) {
                    l4 = 0u;
                    f = fval(l4);
                } else if (!fcomi_be(c, r, 1.0)) {
                    l4 = one;
                    f = fval(l4);
                } else
                    f = r;
            }
            l8 = fbits(fto_float(c, fx87(c, 1.0 - fx87(c, fx87(c, 1.0 - fld32(self + 0x210u)) * f))));
            l10 = fbits(fto_float(c, fx87(c, 1.0 - fx87(c, fx87(c, 1.0 - fld32(self + 0x214u)) * f))));
            l18 = fbits(fto_float(c, fx87(c, 1.0 - fx87(c, fx87(c, 1.0 - fld32(self + 0x218u)) * f))));
            l20 = fbits(fto_float(c, fx87(c, 1.0 - fx87(c, f * fld32(self + 0x21cu)))));
        }
    }

    wrf32(out + 4u, fto_float(c, fx87(c, fval(l8) * fval(lc))));
    wrf32(out + 8u, fto_float(c, fx87(c, fval(l10) * fval(l14))));
    wrf32(out + 0xcu, fto_float(c, fx87(c, fval(l18) * fval(l1c))));
    wrf32(out + 0x10u, fto_float(c, fx87(c, fval(l20) * fval(l24))));
    ftag_free_below(c, depth);
    c->r[0] = out;
    c->eip = rd32(esp);
    c->r[4] = esp + 0x10u;
    recomp_return(c);
}

/* CalcExtents (103afae0) is native in native/pdo.c, group pdo. */

/* ------------------------------------------------------------------ TickParticles */

/* SharedBase imports (Engine's IAT) TickParticles calls. */
#define IAT_SETVECTOR 0x105f7158u        /* bCVector::SetVector(bCVector const &), RET 4 */
#define IAT_SCALE_V 0x105f6c28u          /* bCVector::Scale(bCVector const &), RET 4 */
#define IAT_GETSCALED 0x105f6748u        /* bCVector::GetScaled(float) const -> out, RET 8 */
#define IAT_TRANSLATE 0x105f67b4u        /* bCVector::Translate(bCVector const &), RET 4 */
#define IAT_INVTRANSLATE 0x105f66ecu     /* bCVector::InvTranslate(bCVector const &), RET 4 */
#define IAT_RANGE3_RANDOM 0x105f6d54u    /* bCRange3::GetRandom() const -> out, RET 4 */
#define IAT_RANGE3_RANDOM_TO 0x105f6d4cu /* bCRange3::GetRandom(bCVector &) const, RET 4 */
#define IAT_SETTOROTATION 0x105f6d20u    /* bCMatrix3::SetToRotation(bCEulerAngles const &), RET 4 */
#define IAT_MUL_MATRIX3 0x105f6d30u      /* bCVector::operator*=(bCMatrix3 const &), RET 4 */
#define IAT_GETMAGNITUDE 0x105f67c0u     /* bCVector::GetMagnitude() const, ST0 */
#define IAT_GETINVTRANSLATED 0x105f6754u /* bCVector::GetInvTranslated(bCVector const &) const, RET 8 */
#define IAT_NORMALIZE 0x105f6904u        /* bCVector::Normalize() */
#define IAT_ISVALID 0x105f6f70u          /* bCPropertyID::IsValid() const, AL */
#define IAT_RANDOMINT 0x105f68c4u        /* bCRange1::GetRandomInt() const, EAX */
#define IAT_REFLECT 0x105f6d28u          /* bCVector::Reflect(bCVector const &) const -> out, RET 8 */
#define IAT_SCALE_F 0x105f6684u          /* bCVector::Scale(float), RET 4 */
#define IAT_GETSQUAREMAG 0x105f677cu     /* bCVector::GetSquareMagnitude() const, ST0 */
#define IAT_DEGTORAD 0x105f7178u         /* data: &g_fDegToRad */

#define ENGINE_PHYS_SCENE 0x10862dccu  /* eCPhysicsScene::s_pCurrentInstance */
#define ENGINE_CLAMP_BASE 0x106e849cu  /* float the velocity clamp subtracts the limit from */
#define ENGINE_GRAVITY_K 0x106e84d8u   /* float gravity factor */
#define ENGINE_TRACE_K 0x106e8548u     /* double: trace length factor */
#define ENGINE_REFLECT_K 0x106e847cu   /* float the reflected velocity is scaled by */
#define ENGINE_MIN_SPEED_K 0x106e8498u /* float: the minimum speed test is off at or below it */
#define ENGINE_HIT_OFFSET 0x1085d410u  /* float: how far off the surface a hit particle is put */
#define ENGINE_ROT_K 0x106e8850u       /* double: rotation speed factor */

#ifndef FN_103b0330
#define FN_103b0330 fn_103b0330
#endif
#ifndef FN_103ba810
#define FN_103ba810 fn_103ba810
#endif
#ifndef FN_103af640
#define FN_103af640 fn_103af640
#endif
#ifndef FN_103af6b0
#define FN_103af6b0 fn_103af6b0
#endif
#ifndef FN_103cd980
#define FN_103cd980 fn_103cd980
#endif
#ifndef FN_103b5190
#define FN_103b5190 fn_103b5190
#endif
#ifndef FN_1048e500
#define FN_1048e500 fn_1048e500
#endif
#ifndef FN_103b0480
#define FN_103b0480 fn_103b0480
#endif
#ifndef FN_103c00c0
#define FN_103c00c0 fn_103c00c0
#endif
#ifndef FN_103af4e0
#define FN_103af4e0 fn_103af4e0
#endif
void fn_103b0330(X86 *c); /* Engine eCParticle2 copy (EAX = dst, ECX = src) */
void fn_103ba810(X86 *c); /* Engine particle array SetCount (EDI = array, EDX = count, ECX) */
void fn_103af640(X86 *c); /* Engine CalcVelocityFactor */
void fn_103af6b0(X86 *c); /* Engine CalcRevolutionFactor */
void fn_103cd980(X86 *c); /* Engine eCPhysicsScene::TraceRayNearestHit */
void fn_103b5190(X86 *c); /* Engine PlayCollisionSound */
void fn_1048e500(X86 *c); /* Engine eCEntityProxy::SetEntity */
void fn_103b0480(X86 *c); /* Engine SpawnParticle */
void fn_103c00c0(X86 *c); /* Engine eCVectorScale::CalcValue */
void fn_103af4e0(X86 *c); /* Engine CalcFrame */

/* One inline cache per import: the slot's target never changes, so every site shares it. */
static uint64_t ic_setvector, ic_scale_v, ic_getscaled, ic_translate, ic_invtranslate, ic_range3_random,
    ic_range3_random_to, ic_settorotation, ic_mul_matrix3, ic_getmagnitude, ic_getinvtranslated, ic_normalize,
    ic_isvalid, ic_randomint, ic_reflect, ic_scale_f, ic_getsquaremag;
/* The virtual calls, one per site as the translation has them. */
static uint64_t ic_vt_5f12, ic_vt_5f37, ic_vt_6011, ic_vt_6061, ic_vt_6089;

/* CALL dword ptr [slot], returning to `ret`. */
static inline void call_iat(X86 *c, uint32_t slot, uint64_t *ic, uint32_t ret) {
    const uint32_t t = rd32(slot);
    c->r[4] -= 4u;
    wr32(c->r[4], ret);
    recomp_call_cached(c, t, ic);
}
/* CALL reg, returning to `ret`. */
static inline void call_reg(X86 *c, uint32_t target, uint64_t *ic, uint32_t ret) {
    c->r[4] -= 4u;
    wr32(c->r[4], ret);
    recomp_call_cached(c, target, ic);
}
/* CALL addr, returning to `ret`. */
static inline void call_fn(X86 *c, void (*fn)(X86 *), uint32_t ret) {
    c->r[4] -= 4u;
    wr32(c->r[4], ret);
    fn(c);
}

/* The x87 slots the original pushed into and popped again: ST(-1)..ST(-n) of the frame's stack
 * (which is the entry stack at every call TickParticles makes) are empty. */
static inline void x87_used(X86 *c, unsigned n) {
    for (unsigned i = 1; i <= n; ++i) {
        const unsigned p = (c->fpu_top - i) & 7u;
        ftag_put(c, p, FTAG_EMPTY);
        c->st_exact[p] = 0;
    }
}

/* The value a callee left in ST0, popped (the slot is freed with the rest by x87_used). */
static inline double x87_take(X86 *c) {
    const double v = c->st[c->fpu_top];
    c->st_exact[c->fpu_top] = 0;
    ftag_put(c, c->fpu_top, FTAG_EMPTY);
    c->fpu_top = (c->fpu_top + 1u) & 7u;
    return v;
}

/* UCOMISS/COMISS a, b then LAHF: AH gets SF ZF AF PF CF (SF = AF = 0) and bit 1. */
static inline void lahf_comis(X86 *c, float a, float b) {
    uint32_t ah = 0x02u;
    if (a != a || b != b)
        ah |= 0x45u;
    else
        ah |= (a == b ? 0x40u : 0u) | (a < b ? 0x01u : 0u);
    c->r[0] = (c->r[0] & 0xffff00ffu) | (ah << 8);
}

static inline void xmm_copy(X86 *c, int dst, int src) {
    for (int i = 0; i < 4; ++i)
        c->xmm[dst][i] = c->xmm[src][i];
}
static inline void xmm_zero(X86 *c, int reg) {
    c->xmm[reg][0] = c->xmm[reg][1] = c->xmm[reg][2] = c->xmm[reg][3] = 0u;
}

/* FLD float [src]; PUSH ECX; FSTP float ptr [ESP]; PUSH out: the float and the out pointer of
 * GetScaled, with ESP at frame - 8. */
static inline void args_scaled(X86 *c, uint32_t sp, double v, uint32_t out) {
    wrf32(sp - 4u, fto_float(c, v));
    wr32(sp - 8u, out);
    c->r[4] = sp - 8u;
    x87_used(c, 1);
}
/* PUSH a: one argument, ESP at frame - 4. */
static inline void arg1(X86 *c, uint32_t sp, uint32_t a) {
    wr32(sp - 4u, a);
    c->r[4] = sp - 4u;
}

/* 103b5a08..103b5aa1 and its copies for y and z: with a limit m = [lim] other than 0, the velocity
 * component [v] becomes min(v, m), or C - m (C = [106e849c]) when that is below -m. The stack
 * slots are the original's: m at sm, the component at va and vb, the minimum at smin, and the
 * scratch floats at +0xc and +0x14. XMM2 is 0 (XORPS before the first). */
static void clamp_axis(X86 *c, uint32_t sp, uint32_t lim, uint32_t v, uint32_t sm, uint32_t va, uint32_t vb,
                       uint32_t smin) {
    const uint32_t mb = rd32(lim);
    xmm_load_f32_bits(c, 0, mb);
    float mf;
    memcpy(&mf, &mb, 4);
    const float x2 = xmm_f32(c, 2);
    lahf_comis(c, mf, x2);
    wr32(sp + sm, mb);
    if (mf == x2) /* JNP: equal and ordered */
        return;
    const double m = fld32(sp + sm);
    const uint32_t vbits = rd32(v);
    xmm_load_f32_bits(c, 1, vbits);
    wrf32(sp + 0xcu, fto_float(c, m));
    wr32(sp + va, vbits);
    const double m2 = fld32(sp + 0xcu);
    wr32(sp + vb, vbits);
    const double v1 = fld32(sp + va);
    wr32(sp + smin, fcomi_be(c, v1, m2) ? vbits : mb);
    wrf32(sp + 0xcu, fto_float(c, m));
    const double nm = -fld32(sp + 0xcu);
    const double mn = fld32(sp + smin);
    if (!fcomi_be(c, nm, mn)) {
        xmm_copy(c, 1, 0);
        xmm_load_f32_bits(c, 0, rd32(ENGINE_CLAMP_BASE));
        xmm_set_f32(c, 0, xmm_f32(c, 0) - xmm_f32(c, 1));
    } else {
        wrf32(sp + 0xcu, fto_float(c, m));
        wrf32(sp + 0x14u, fto_float(c, fld32(sp + vb)));
        const double m4 = fld32(sp + 0xcu);
        const double v4 = fld32(sp + 0x14u);
        if (fcomi_be(c, v4, m4))
            xmm_copy(c, 0, 1);
    }
    wr32(v, c->xmm[0][0]);
    x87_used(c, 3);
}

/* 103b5d6d..103b63bd: the collision ray of one particle. Returns 1 when the particle dies in it
 * (103b6157), 0 when TickParticles goes on at 103b63ba. EBX may change (the spawn loop) and is
 * written back to *ebx as 103b612f leaves it. */
static int tick_collide(X86 *c, uint32_t self, uint32_t edi, uint32_t sp, uint32_t *ebx) {
    c->r[1] = rd32(ENGINE_PHYS_SCENE);
    wr32(sp + 0x14u, c->r[1]);
    uint32_t flags = 0xa0u;
    wr32(sp + 0x10u, flags);
    if (rd8(self + 0x2d6u))
        wr32(sp + 0x10u, flags = 0xa1u);
    if (rd8(self + 0x2d5u))
        wr32(sp + 0x10u, flags |= 2u);
    if (!rd8(self + 0x2d4u))
        wr32(sp + 0x10u, flags |= 8u);
    if (!rd8(self + 0x2d7u))
        wr32(sp + 0x10u, flags |= 4u);
    /* 103b5e00: the ray's length */
    c->r[2] = edi + 0x60u;
    arg1(c, sp, c->r[2]);
    c->r[1] = sp + 0x120u;
    call_iat(c, IAT_SETVECTOR, &ic_setvector, 0x103b5e11u);
    c->r[0] = edi + 0x6cu;
    arg1(c, sp, c->r[0]);
    c->r[1] = sp + 0x120u;
    call_iat(c, IAT_SCALE_V, &ic_scale_v, 0x103b5e22u);
    arg1(c, sp, sp + 0x120u);
    c->r[1] = sp + 0x114u;
    call_iat(c, IAT_SETVECTOR, &ic_setvector, 0x103b5e37u);
    c->r[2] = self + 0x2c4u;
    arg1(c, sp, c->r[2]);
    c->r[1] = sp + 0x114u;
    call_iat(c, IAT_SCALE_V, &ic_scale_v, 0x103b5e4bu);
    c->r[1] = sp + 0x114u;
    call_iat(c, IAT_GETMAGNITUDE, &ic_getmagnitude, 0x103b5e58u);
    const double len = fx87(c, x87_take(c) * rdf64(ENGINE_TRACE_K));
    uint32_t b = rd32(edi);
    xmm_load_f32_bits(c, 0, b);
    wr32(sp + 0xb4u, b);
    xmm_load_f32_bits(c, 0, rd32(edi + 4u));
    wrf32(sp + 0xcu, fto_float(c, len));
    x87_used(c, 1);
    wr32(sp - 4u, edi);
    c->r[0] = sp + 0x60u;
    wr32(sp + 0xb8u, c->xmm[0][0]);
    xmm_load_f32_bits(c, 0, rd32(edi + 8u));
    wr32(sp - 8u, c->r[0]);
    c->r[4] = sp - 8u;
    c->r[1] = edi + 0xcu;
    wr32(sp + 0xbcu, c->xmm[0][0]);
    call_iat(c, IAT_GETINVTRANSLATED, &ic_getinvtranslated, 0x103b5e9au);
    /* 103b5e9a: the direction, copied, normalised, scaled by the length, added to the copy */
    for (uint32_t k = 0; k < 12u; k += 4u) {
        xmm_load_f32_bits(c, 0, rd32(sp + 0x60u + k));
        wr32(sp + 0x9cu + k, c->xmm[0][0]);
    }
    c->r[1] = sp + 0x60u;
    call_iat(c, IAT_NORMALIZE, &ic_normalize, 0x103b5ed1u);
    args_scaled(c, sp, fld32(sp + 0xcu), sp + 0x15cu);
    c->r[1] = sp + 0x60u;
    call_iat(c, IAT_GETSCALED, &ic_getscaled, 0x103b5eebu);
    c->r[2] = sp + 0x15cu;
    arg1(c, sp, c->r[2]);
    c->r[1] = sp + 0x9cu;
    call_iat(c, IAT_TRANSLATE, &ic_translate, 0x103b5f00u);
    /* 103b5f00: in local coordinates the ray starts from the entity */
    c->r[0] = rd32(self + 0x84u) - 1u;
    if (c->r[0] == 0u) {
        c->r[0] = rd32(self);
        c->r[2] = rd32(c->r[0] + 0x68u);
        c->r[1] = self;
        call_reg(c, c->r[2], &ic_vt_5f12, 0x103b5f14u);
        c->r[0] += 0x70u;
        arg1(c, sp, c->r[0]);
        c->r[1] = sp + 0xb4u;
        call_iat(c, IAT_TRANSLATE, &ic_translate, 0x103b5f25u);
    }
    /* 103b5f25: TraceRayNearestHit(origin, direction, entity, flags, 0, 0, 0) */
    c->r[0] = rd32(sp + 0x10u);
    c->r[2] = rd32(self);
    wr32(sp - 4u, 0u);
    wr32(sp - 8u, 0u);
    wr32(sp - 12u, 0u);
    wr32(sp - 16u, c->r[0]);
    c->r[4] = sp - 16u;
    c->r[0] = rd32(c->r[2] + 0x68u);
    c->r[1] = self;
    call_reg(c, c->r[0], &ic_vt_5f37, 0x103b5f39u);
    wr32(sp - 20u, c->r[0]);
    wr32(sp - 24u, sp + 0x9cu);
    c->r[1] = rd32(sp + 0x14u);
    c->r[2] = sp + 0xb4u;
    wr32(sp - 28u, c->r[2]);
    c->r[4] = sp - 28u;
    call_fn(c, FN_103cd980, 0x103b5f53u);
    c->r[0] = rd32(rd32(sp + 0x14u) + 0xacu);
    if (rd8(c->r[0]) == 0)
        return 0;

    /* 103b5f66: a hit. Normal to +0x30, point to +0x54 and +0x3c, the point moved back along
     * the ray by the trace length, the normal normalised. */
    {
        const uint32_t r = c->r[0];
        xmm_load_f32_bits(c, 0, rd32(r + 0x74u));
        const double d = fld32(sp + 0xcu);
        wr32(sp + 0x30u, c->xmm[0][0]);
        xmm_load_f32_bits(c, 0, rd32(r + 0x78u));
        wr32(sp + 0x34u, c->xmm[0][0]);
        xmm_load_f32_bits(c, 0, rd32(r + 0x7cu));
        wr32(sp + 0x38u, c->xmm[0][0]);
        xmm_load_f32_bits(c, 0, rd32(r + 0x68u));
        wr32(sp + 0x54u, c->xmm[0][0]);
        xmm_load_f32_bits(c, 0, rd32(r + 0x6cu));
        wr32(sp + 0x58u, c->xmm[0][0]);
        xmm_load_f32_bits(c, 0, rd32(r + 0x70u));
        wr32(sp + 0x5cu, c->xmm[0][0]);
        xmm_load_f32_bits(c, 0, rd32(r + 0x68u));
        wr32(sp + 0x3cu, c->xmm[0][0]);
        wrf32(sp - 4u, fto_float(c, d));
        x87_used(c, 1);
        xmm_load_f32_bits(c, 0, rd32(r + 0x6cu));
        wr32(sp + 0x40u, c->xmm[0][0]);
        xmm_load_f32_bits(c, 0, rd32(r + 0x70u));
        wr32(sp - 8u, sp + 0x174u);
        c->r[4] = sp - 8u;
        c->r[1] = sp + 0x60u;
        wr32(sp + 0x44u, c->xmm[0][0]);
    }
    call_iat(c, IAT_GETSCALED, &ic_getscaled, 0x103b5fe3u);
    c->r[2] = sp + 0x174u;
    arg1(c, sp, c->r[2]);
    c->r[1] = sp + 0x3cu;
    call_iat(c, IAT_INVTRANSLATE, &ic_invtranslate, 0x103b5ff5u);
    c->r[1] = sp + 0x30u;
    call_iat(c, IAT_NORMALIZE, &ic_normalize, 0x103b5fffu);
    c->r[0] = rd32(self + 0x84u) - 1u;
    if (c->r[0] == 0u) {
        c->r[0] = rd32(self);
        c->r[2] = rd32(c->r[0] + 0x68u);
        c->r[1] = self;
        call_reg(c, c->r[2], &ic_vt_6011, 0x103b6013u);
        c->r[0] += 0x70u;
        arg1(c, sp, c->r[0]);
        c->r[1] = sp + 0x3cu;
        call_iat(c, IAT_INVTRANSLATE, &ic_invtranslate, 0x103b6021u);
    }
    arg1(c, sp, edi);
    c->r[1] = self;
    call_fn(c, FN_103b5190, 0x103b6029u);

    /* 103b6029: particles spawned at the hit by the entity's particle system (property set 0x57) */
    {
        uint32_t e = rd32(self + 0x2dcu);
        c->r[0] = e ? rd32(e) : 0u;
        c->r[1] = c->r[0];
        int have = c->r[1] != 0u;
        if (!have) {
            c->r[1] = self + 0x2e0u;
            call_iat(c, IAT_ISVALID, &ic_isvalid, 0x103b604bu);
            if ((c->r[0] & 0xffu) != 0u) {
                c->r[0] = rd32(self + 0x2d8u);
                c->r[2] = rd32(c->r[0]);
                c->r[1] = self + 0x2d8u;
                call_reg(c, c->r[2], &ic_vt_6061, 0x103b6063u);
                wr32(sp + 0xcu, c->r[0]);
                if (c->r[0] != 0u) {
                    arg1(c, sp, c->r[0]);
                    c->r[1] = self + 0x2d8u;
                    call_fn(c, FN_1048e500, 0x103b607bu);
                    c->r[1] = rd32(sp + 0xcu);
                    have = 1;
                }
            }
        }
        if (have) {
            c->r[0] = rd32(c->r[1]);
            c->r[2] = rd32(c->r[0] + 0xecu);
            arg1(c, sp, 0x57u);
            call_reg(c, c->r[2], &ic_vt_6089, 0x103b608bu);
            wr32(sp + 0xcu, c->r[0]);
            if (c->r[0] != 0u) {
                c->r[1] = self + 0x2f4u;
                call_iat(c, IAT_RANDOMINT, &ic_randomint, 0x103b60a3u);
                if ((int32_t)c->r[0] > 0) {
                    wr32(sp + 0x14u, c->r[0]);
                    do {
                        c->r[1] = rd32(sp + 0xcu);
                        c->r[0] = sp + 0x54u;
                        arg1(c, sp, c->r[0]);
                        call_fn(c, FN_103b0480, 0x103b60bdu);
                        c->r[3] = c->r[0];
                        if ((int32_t)c->r[3] >= 0 && rd8(self + 0x2fcu)) {
                            c->r[1] = sp + 0x18cu;
                            arg1(c, sp, c->r[1]);
                            c->r[1] = self + 0x300u;
                            call_iat(c, IAT_RANGE3_RANDOM_TO, &ic_range3_random_to, 0x103b60e0u);
                            c->r[3] *= 0xb4u;
                            c->r[2] = rd32(sp + 0xcu);
                            c->r[3] += rd32(c->r[2] + 0x318u);
                            c->r[0] = sp + 0x30u;
                            arg1(c, sp, c->r[0]);
                            c->r[1] = sp + 0x144u;
                            call_iat(c, IAT_SETVECTOR, &ic_setvector, 0x103b6102u);
                            c->r[1] = sp + 0x18cu;
                            arg1(c, sp, c->r[1]);
                            c->r[1] = sp + 0x144u;
                            call_iat(c, IAT_SCALE_V, &ic_scale_v, 0x103b6117u);
                            c->r[2] = sp + 0x144u;
                            c->r[1] = c->r[3] + 0x18u;
                            arg1(c, sp, c->r[2]);
                            call_iat(c, IAT_TRANSLATE, &ic_translate, 0x103b6128u);
                        }
                        wr32(sp + 0x14u, rd32(sp + 0x14u) - 1u);
                    } while (rd32(sp + 0x14u) != 0u);
                    c->r[3] = *ebx = rd32(sp + 0x2cu);
                }
            }
        }
    }

    /* 103b6133: a particle that bounced too often dies */
    if (rd8(self + 0x285u)) {
        wr32(edi + 0xacu, rd32(edi + 0xacu) + 1u);
        c->r[1] = self + 0x288u;
        call_iat(c, IAT_RANDOMINT, &ic_randomint, 0x103b614fu);
        if (rd32(edi + 0xacu) > c->r[0])
            return 1;
    }

    /* 103b61bb: the velocity reflected on the normal, damped, randomised */
    c->r[1] = sp + 0x30u;
    wr32(sp - 4u, c->r[1]);
    c->r[2] = sp + 0x1c8u;
    wr32(sp - 8u, c->r[2]);
    c->r[4] = sp - 8u;
    c->r[1] = edi + 0x18u;
    call_iat(c, IAT_REFLECT, &ic_reflect, 0x103b61d1u);
    {
        const uint32_t r = c->r[0];
        wrf32(edi + 0x18u, fto_float(c, fld32(r)));
        c->r[1] = edi + 0x18u;
        const double y = fld32(r + 4u);
        wr32(sp - 4u, c->r[1]);
        wrf32(c->r[1] + 4u, fto_float(c, y));
        wrf32(c->r[1] + 8u, fto_float(c, fld32(r + 8u)));
        wrf32(sp - 4u, fto_float(c, fld32(ENGINE_REFLECT_K)));
        c->r[4] = sp - 4u;
        x87_used(c, 1);
    }
    call_iat(c, IAT_SCALE_F, &ic_scale_f, 0x103b61f5u);
    c->r[0] = sp + 0xf0u;
    arg1(c, sp, c->r[0]);
    c->r[1] = self + 0x290u;
    call_iat(c, IAT_RANGE3_RANDOM_TO, &ic_range3_random_to, 0x103b6209u);
    xmm_load_f32_bits(c, 0, rd32(edi + 0x18u));
    wr32(sp + 0xe4u, c->xmm[0][0]);
    xmm_load_f32_bits(c, 0, rd32(edi + 0x1cu));
    wr32(sp + 0xe8u, c->xmm[0][0]);
    xmm_load_f32_bits(c, 0, rd32(edi + 0x20u));
    arg1(c, sp, sp + 0xf0u);
    c->r[1] = sp + 0xe4u;
    wr32(sp + 0xecu, c->xmm[0][0]);
    call_iat(c, IAT_SCALE_V, &ic_scale_v, 0x103b6248u);
    c->r[2] = sp + 0xe4u;
    arg1(c, sp, c->r[2]);
    c->r[1] = edi + 0x18u;
    call_iat(c, IAT_INVTRANSLATE, &ic_invtranslate, 0x103b6259u);
    if (rd8(self + 0x2a8u)) {
        c->r[0] = sp + 0xf0u;
        arg1(c, sp, c->r[0]);
        c->r[1] = self + 0x2acu;
        call_iat(c, IAT_RANGE3_RANDOM_TO, &ic_range3_random_to, 0x103b627au);
        xmm_load_f32_bits(c, 0, rd32(edi + 0x54u));
        wr32(sp + 0xccu, c->xmm[0][0]);
        xmm_load_f32_bits(c, 0, rd32(edi + 0x58u));
        wr32(sp + 0xd0u, c->xmm[0][0]);
        xmm_load_f32_bits(c, 0, rd32(edi + 0x5cu));
        arg1(c, sp, sp + 0xf0u);
        c->r[1] = sp + 0xccu;
        wr32(sp + 0xd4u, c->xmm[0][0]);
        call_iat(c, IAT_SCALE_V, &ic_scale_v, 0x103b62b9u);
        c->r[2] = sp + 0xccu;
        arg1(c, sp, c->r[2]);
        c->r[1] = edi + 0x54u;
        call_iat(c, IAT_INVTRANSLATE, &ic_invtranslate, 0x103b62cau);
        xmm_load_f32_bits(c, 0, rd32(edi + 0x3cu));
        wr32(sp + 0xa8u, c->xmm[0][0]);
        xmm_load_f32_bits(c, 0, rd32(edi + 0x40u));
        c->r[0] = sp + 0xf0u;
        wr32(sp + 0xacu, c->xmm[0][0]);
        xmm_load_f32_bits(c, 0, rd32(edi + 0x44u));
        arg1(c, sp, c->r[0]);
        c->r[1] = sp + 0xa8u;
        wr32(sp + 0xb0u, c->xmm[0][0]);
        call_iat(c, IAT_SCALE_V, &ic_scale_v, 0x103b6309u);
        c->r[1] = sp + 0xa8u;
        arg1(c, sp, c->r[1]);
        c->r[1] = edi + 0x3cu;
        call_iat(c, IAT_INVTRANSLATE, &ic_invtranslate, 0x103b631au);
    }
    /* 103b631a: too slow after the bounce: dies */
    xmm_load_f32_bits(c, 0, rd32(self + 0x2d0u));
    {
        const float lim = xmm_f32(c, 0), k = rdf32(ENGINE_MIN_SPEED_K);
        if (lim > k) { /* COMISS, JBE not taken: ordered and greater */
            c->r[1] = edi + 0x18u;
            call_iat(c, IAT_GETSQUAREMAG, &ic_getsquaremag, 0x103b6334u);
            const double sq = x87_take(c);
            const double v = fld32(self + 0x2d0u);
            const int le = fcomi_be(c, v, sq);
            x87_used(c, 2);
            if (!le)
                return 1;
        }
    }
    /* 103b6344: the particle sits on the surface, nudged along the normal */
    xmm_load_f32_bits(c, 0, rd32(sp + 0x3cu));
    {
        const double off = fld32(ENGINE_HIT_OFFSET);
        wr32(edi + 0xcu, c->xmm[0][0]);
        xmm_load_f32_bits(c, 0, rd32(sp + 0x40u));
        wr32(edi + 0x10u, c->xmm[0][0]);
        xmm_load_f32_bits(c, 0, rd32(sp + 0x44u));
        wr32(edi + 0x14u, c->xmm[0][0]);
        xmm_load_f32_bits(c, 0, rd32(sp + 0x30u));
        wr32(sp + 0xc0u, c->xmm[0][0]);
        xmm_load_f32_bits(c, 0, rd32(sp + 0x34u));
        wr32(sp + 0xc4u, c->xmm[0][0]);
        wrf32(sp - 4u, fto_float(c, off));
        x87_used(c, 1);
        xmm_load_f32_bits(c, 0, rd32(sp + 0x38u));
        c->r[1] = sp + 0xc0u;
        wr32(sp + 0xc8u, c->xmm[0][0]);
        c->r[4] = sp - 4u;
    }
    call_iat(c, IAT_SCALE_F, &ic_scale_f, 0x103b63a9u);
    c->r[1] = sp + 0xc0u;
    arg1(c, sp, c->r[1]);
    c->r[1] = edi + 0xcu;
    call_iat(c, IAT_TRANSLATE, &ic_translate, 0x103b63bau);
    return 0;
}

/* 103b58a0 / 103b6168: a dead particle is replaced by the last one and the array shrinks by one
 * (eCParticle2 copy, then SetCount). The two sites differ only in when EDI becomes &array. */
static void tick_remove(X86 *c, uint32_t self, uint32_t sp, uint32_t *ebx, int edi_first) {
    const uint32_t arr = rd32(self + 0x318u);
    const uint32_t last = rd32(sp + 0x28u) - 0xb4u;
    *ebx -= 1u;
    c->r[3] = *ebx;
    wr32(sp + 0x28u, last);
    c->r[1] = last + arr;
    c->r[0] = rd32(sp + 0x18u) + arr;
    wr32(sp + 0x2cu, *ebx);
    if (edi_first) {
        c->r[2] = arr;
        c->r[7] = self + 0x318u;
        call_fn(c, FN_103b0330, 0x103b6195u);
    } else
        call_fn(c, FN_103b0330, 0x103b58bdu);
    c->r[1] = *ebx ? 0u : 0xffffffffu;
    c->r[2] = *ebx;
    c->r[7] = self + 0x318u;
    call_fn(c, FN_103ba810, edi_first ? 0x103b61a7u : 0x103b58d5u);
    wr32(self + 0x31cu, *ebx);
    /* 103b58db */
    wr32(sp + 0x1cu, rd32(sp + 0x1cu) - 1u);
    wr32(sp + 0x18u, rd32(sp + 0x18u) - 0xb4u);
    xmm_zero(c, 3);
}

/* int eCParticle_PS::TickParticles(float dt), RET 4: ages every particle by dt, kills the ones past
 * their life (removing them from the array when [this+0x1c] says so, counting them otherwise),
 * and moves the rest: velocity, gravity, the speed clamp, the random jitter, the velocity and
 * revolution factors, the collision ray, the rotation, the size, frame and colour scales. Returns
 * the number of dead particles left in place.
 *
 * Everything the original computes itself is native here: the x87 arithmetic through the kit's
 * rounding helpers as in the functions above, the SSE moves, compares and the one SUBSS with the
 * translation's own host operations, every register the original writes and every store it makes,
 * the frame included (callees get pointers into it). Every call it makes is a guest call made from
 * here, in the same order, with the registers, stack and x87 state the original has at that call:
 * SharedBase imports through the IAT slot and recomp_call_cached, virtual calls through the
 * vtable word, Engine functions through FN_<addr> (so overrides such as CalcColor apply). The x87
 * stack is empty (at its entry depth) at every call, as in the original; values are local
 * doubles in between and the slots the original used are tagged empty before the next call.
 * The stale values those empty slots hold are not reproduced. */
void g3n_103b5800(X86 *c) {
    if (!G3OPT(PARTICLES)) {
        fn_103b5800(c);
        return;
    }
    const uint32_t e0 = c->r[4];
    const uint32_t ebp = e0 - 4u;
    const uint32_t self = c->r[1];
    wr32(ebp, c->r[5]);
    c->r[5] = ebp;
    const uint32_t sp = ((ebp & 0xfffffff8u) - 0x1ecu) - 12u;
    wr32(sp + 8u, c->r[3]);
    wr32(sp + 4u, c->r[6]);
    wr32(sp, c->r[7]);
    c->r[6] = self;
    uint32_t ebx = rd32(self + 0x31cu);
    c->r[3] = ebx;
    c->r[0] = 0u;
    c->r[4] = sp;
    wr32(sp + 0x2cu, ebx);
    wr32(sp + 0x20u, 0u);
    wr32(sp + 0x1cu, 0u);
    if ((int32_t)ebx > 0) {
        xmm_zero(c, 3);
        c->r[2] = 0u;
        c->r[1] = ebx * 0xb4u;
        wr32(sp + 0x18u, 0u);
        wr32(sp + 0x28u, c->r[1]);
        for (;;) {
            /* 103b5848 */
            const uint32_t off = c->r[2];
            uint32_t edi = rd32(self + 0x318u);
            const double age0 = fld32(edi + off + 0xa0u);
            edi += off;
            c->r[7] = edi;
            const float af = fto_float(c, fx87(c, age0 + fld32(ebp + 8u)));
            wrf32(sp + 0xcu, af);
            const double a = fld32(sp + 0xcu);
            wrf32(edi + 0xa0u, fto_float(c, a));
            wrf32(sp + 0x10u, fto_float(c, fld32(edi + 0xa4u)));
            const double l = fld32(sp + 0x10u);
            int dead;
            if (a != a || l != l) {
                c->fpu_sw |= 0x0001u;
                dead = 0;
            } else
                dead = a > l;
            if (!dead) {
                c->r[0] = rd32(edi + 0xb0u) & 1u;
                dead = c->r[0] == 1u;
            }
            if (dead) {
                /* 103b5889 */
                c->r[0] = 1u;
                x87_used(c, 2);
                wr32(edi + 0xb0u, rd32(edi + 0xb0u) | 1u);
                if (rd8(self + 0x1cu) == 0)
                    wr32(sp + 0x20u, rd32(sp + 0x20u) + 1u);
                else
                    tick_remove(c, self, sp, &ebx, 0);
                goto next;
            }

            /* 103b58f9: alive. The life fraction age / life, 0 for a life of 0. */
            {
                const uint32_t lb = rd32(sp + 0x10u);
                xmm_load_f32_bits(c, 0, lb);
                float lf;
                memcpy(&lf, &lb, 4);
                const float z = xmm_f32(c, 3);
                lahf_comis(c, lf, z);
                wr32(sp + 0x24u, c->xmm[3][0]);
                if (!(lf == z))
                    wrf32(sp + 0x24u, fto_float(c, fx87(c, fdivz(c, a, l))));
                x87_used(c, 2);
            }
            /* 103b591a: position from velocity, gravity */
            c->r[0] = edi + 0xcu;
            arg1(c, sp, c->r[0]);
            c->r[1] = edi;
            call_iat(c, IAT_SETVECTOR, &ic_setvector, 0x103b5926u);
            c->r[0] = edi + 0x18u;
            arg1(c, sp, c->r[0]);
            c->r[1] = sp + 0x12cu;
            call_iat(c, IAT_SETVECTOR, &ic_setvector, 0x103b5937u);
            c->r[0] = edi + 0x24u;
            arg1(c, sp, c->r[0]);
            c->r[1] = sp + 0x12cu;
            call_iat(c, IAT_SCALE_V, &ic_scale_v, 0x103b5948u);
            args_scaled(c, sp, fld32(ebp + 8u), sp + 0x180u);
            c->r[1] = sp + 0x12cu;
            call_iat(c, IAT_GETSCALED, &ic_getscaled, 0x103b5964u);
            c->r[2] = sp + 0x180u;
            arg1(c, sp, c->r[2]);
            c->r[1] = edi + 0xcu;
            call_iat(c, IAT_TRANSLATE, &ic_translate, 0x103b5975u);
            c->r[1] = self + 0xb8u;
            c->r[0] = sp + 0x168u;
            args_scaled(c, sp, fld32(ENGINE_GRAVITY_K), c->r[0]);
            call_iat(c, IAT_GETSCALED, &ic_getscaled, 0x103b5993u);
            args_scaled(c, sp, fld32(ebp + 8u), sp + 0x1b0u);
            c->r[1] = sp + 0x168u;
            call_iat(c, IAT_GETSCALED, &ic_getscaled, 0x103b59afu);
            c->r[2] = sp + 0x150u;
            args_scaled(c, sp, fld32(ebp + 8u), c->r[2]);
            c->r[1] = sp + 0x1b0u;
            call_iat(c, IAT_GETSCALED, &ic_getscaled, 0x103b59cbu);
            c->r[0] = sp + 0x150u;
            arg1(c, sp, c->r[0]);
            c->r[1] = edi + 0xcu;
            call_iat(c, IAT_TRANSLATE, &ic_translate, 0x103b59dcu);
            args_scaled(c, sp, fld32(ebp + 8u), sp + 0x1a4u);
            c->r[1] = self + 0xb8u;
            call_iat(c, IAT_GETSCALED, &ic_getscaled, 0x103b59f7u);
            c->r[2] = sp + 0x1a4u;
            arg1(c, sp, c->r[2]);
            c->r[1] = edi + 0x18u;
            call_iat(c, IAT_TRANSLATE, &ic_translate, 0x103b5a08u);

            /* 103b5a08: the speed clamp per axis (XORPS XMM2 between the first load and compare) */
            xmm_zero(c, 2);
            clamp_axis(c, sp, self + 0xc4u, edi + 0x18u, 0x6cu, 0x70u, 0x78u, 0x48u);
            clamp_axis(c, sp, self + 0xc8u, edi + 0x1cu, 0x80u, 0x88u, 0x74u, 0x4cu);
            clamp_axis(c, sp, self + 0xccu, edi + 0x20u, 0x84u, 0x7cu, 0x8cu, 0x50u);

            /* 103b5bf4: the random jitter, the velocity factor */
            c->r[0] = sp + 0x1bcu;
            arg1(c, sp, c->r[0]);
            c->r[1] = self + 0xa0u;
            call_iat(c, IAT_RANGE3_RANDOM, &ic_range3_random, 0x103b5c08u);
            wr32(sp + 0xcu, c->r[0]);
            c->r[0] = edi + 0x18u;
            arg1(c, sp, c->r[0]);
            c->r[1] = sp + 0x138u;
            call_iat(c, IAT_SETVECTOR, &ic_setvector, 0x103b5c1du);
            c->r[1] = rd32(sp + 0xcu);
            arg1(c, sp, c->r[1]);
            c->r[1] = sp + 0x138u;
            call_iat(c, IAT_SCALE_V, &ic_scale_v, 0x103b5c2fu);
            c->r[2] = sp + 0x198u;
            args_scaled(c, sp, fld32(ebp + 8u), c->r[2]);
            c->r[1] = sp + 0x138u;
            call_iat(c, IAT_GETSCALED, &ic_getscaled, 0x103b5c4bu);
            c->r[0] = sp + 0x198u;
            arg1(c, sp, c->r[0]);
            c->r[1] = edi + 0x18u;
            call_iat(c, IAT_INVTRANSLATE, &ic_invtranslate, 0x103b5c5cu);
            {
                const double r = fld32(sp + 0x24u);
                c->r[0] = edi + 0x24u;
                wr32(sp - 4u, c->r[0]);
                wrf32(sp - 8u, fto_float(c, r));
                c->r[4] = sp - 8u;
                c->r[1] = self;
                x87_used(c, 1);
            }
            call_fn(c, FN_103af640, 0x103b5c6fu);

            /* 103b5c6f: revolution around the particle's centre */
            if (rd8(self + 0x160u) == 1u) {
                {
                    const double r = fld32(sp + 0x24u);
                    c->r[1] = sp + 0xfcu;
                    wr32(sp - 4u, c->r[1]);
                    wrf32(sp - 8u, fto_float(c, r));
                    c->r[4] = sp - 8u;
                    c->r[1] = self;
                    x87_used(c, 1);
                }
                call_fn(c, FN_103af6b0, 0x103b5c93u);
                {
                    wrf32(sp + 0xcu, fto_float(c, fld32(edi + 0x3cu)));
                    c->r[2] = sp + 0x108u;
                    const double ax = fx87(c, fld32(sp + 0xcu) * fld32(sp + 0xfcu));
                    wr32(sp - 4u, c->r[2]);
                    c->r[1] = sp + 0x1d4u;
                    const double p = fld32(ebp + 8u);
                    wrf32(sp + 0x10cu, fto_float(c, fx87(c, ax * p)));
                    wrf32(sp + 0xcu, fto_float(c, fld32(edi + 0x40u)));
                    const double ay = fx87(c, fx87(c, fld32(sp + 0x100u) * fld32(sp + 0xcu)) * p);
                    wrf32(sp + 0x108u, fto_float(c, ay));
                    wrf32(sp + 0xcu, fto_float(c, fld32(edi + 0x44u)));
                    const double az = fx87(c, p * fx87(c, fld32(sp + 0x104u) * fld32(sp + 0xcu)));
                    wrf32(sp + 0x110u, fto_float(c, az));
                    c->r[4] = sp - 4u;
                    x87_used(c, 3);
                }
                call_iat(c, IAT_SETTOROTATION, &ic_settorotation, 0x103b5d00u);
                c->r[0] = edi + 0xcu;
                arg1(c, sp, c->r[0]);
                c->r[1] = sp + 0x90u;
                call_iat(c, IAT_SETVECTOR, &ic_setvector, 0x103b5d11u);
                c->r[0] = edi + 0x30u;
                arg1(c, sp, c->r[0]);
                c->r[1] = sp + 0x90u;
                call_iat(c, IAT_INVTRANSLATE, &ic_invtranslate, 0x103b5d22u);
                c->r[0] = sp + 0x1d4u;
                arg1(c, sp, c->r[0]);
                c->r[1] = sp + 0x90u;
                call_iat(c, IAT_MUL_MATRIX3, &ic_mul_matrix3, 0x103b5d37u);
                xmm_load_f32_bits(c, 0, rd32(sp + 0x90u));
                c->r[1] = edi + 0xcu;
                wr32(c->r[1], c->xmm[0][0]);
                xmm_load_f32_bits(c, 0, rd32(sp + 0x94u));
                c->r[0] = edi + 0x30u;
                wr32(c->r[1] + 4u, c->xmm[0][0]);
                xmm_load_f32_bits(c, 0, rd32(sp + 0x98u));
                wr32(sp - 4u, c->r[0]);
                c->r[4] = sp - 4u;
                wr32(c->r[1] + 8u, c->xmm[0][0]);
                call_iat(c, IAT_TRANSLATE, &ic_translate, 0x103b5d6du);
            }

            /* 103b5d6d: the hit locals start at 0; the collision ray */
            xmm_zero(c, 3);
            for (uint32_t k = 0x30u; k <= 0x44u; k += 4u)
                wr32(sp + k, 0u);
            wr32(sp + 0x54u, 0u);
            wr32(sp + 0x58u, 0u);
            wr32(sp + 0x5cu, 0u);
            if (rd8(self + 0x284u)) {
                if (tick_collide(c, self, edi, sp, &ebx)) {
                    /* 103b6157 */
                    c->r[0] = 1u;
                    wr32(edi + 0xb0u, rd32(edi + 0xb0u) | 1u);
                    if (rd8(self + 0x1cu)) {
                        tick_remove(c, self, sp, &ebx, 1);
                    } else {
                        wr32(sp + 0x20u, rd32(sp + 0x20u) + 1u);
                        xmm_zero(c, 3);
                    }
                    goto next;
                }
                xmm_zero(c, 3); /* 103b63ba */
            }

            /* 103b63bd: spin */
            if (rd8(self + 0x100u)) {
                c->r[2] = rd32(IAT_DEGTORAD);
                double s = fld32(c->r[2]);
                xmm_load_f32_bits(c, 0, rd32(edi + 0x54u));
                s = fx87(c, s * rdf64(ENGINE_ROT_K));
                wr32(sp + 0xd8u, c->xmm[0][0]);
                xmm_load_f32_bits(c, 0, rd32(edi + 0x58u));
                s = fx87(c, s * fld32(ebp + 8u));
                wr32(sp + 0xdcu, c->xmm[0][0]);
                xmm_load_f32_bits(c, 0, rd32(edi + 0x5cu));
                c->r[1] = sp + 0xd8u;
                wrf32(sp + 0xcu, fto_float(c, s));
                wr32(sp + 0xe0u, c->xmm[0][0]);
                wrf32(sp - 4u, fto_float(c, fld32(sp + 0xcu)));
                c->r[4] = sp - 4u;
                x87_used(c, 1);
                call_iat(c, IAT_SCALE_F, &ic_scale_f, 0x103b641au);
                c->r[0] = sp + 0xd8u;
                arg1(c, sp, c->r[0]);
                c->r[1] = edi + 0x48u;
                call_iat(c, IAT_TRANSLATE, &ic_translate, 0x103b642bu);
                xmm_zero(c, 3);
            }

            /* 103b642e: size from the vector scale at frac(life fraction * (speed + 1)) */
            c->r[0] = edi + 0x6cu;
            if (rd8(self + 0x1b8u)) {
                const double v = fx87(c, fx87(c, fld32(self + 0x1bcu) + rdf64(ENGINE_ONE_D)) * fld32(sp + 0x24u));
                wr32(sp - 4u, c->r[0]);
                wrf32(sp + 0xcu, fto_float(c, v));
                c->r[1] = (uint32_t)(int32_t)rdf32(sp + 0xcu);
                const double w = fld32(sp + 0xcu);
                wr32(sp + 0xcu, c->r[1]);
                const double fr = fx87(c, w - (double)(int32_t)rd32(sp + 0xcu));
                wr32(sp - 8u, c->r[1]);
                c->r[1] = self + 0x3e0u;
                wrf32(sp + 0xcu, fto_float(c, fr));
                wrf32(sp - 8u, fto_float(c, fld32(sp + 0xcu)));
                c->r[4] = sp - 8u;
                x87_used(c, 1);
                call_fn(c, FN_103c00c0, 0x103b6478u);
            } else {
                const uint32_t one = rd32(ENGINE_ONE_F);
                xmm_load_f32_bits(c, 0, one);
                wr32(c->r[0], one);
                wr32(c->r[0] + 4u, one);
                wr32(c->r[0] + 8u, one);
            }
            /* 103b6490: frame, colour, colour factor */
            if (rd8(self + 0x1e4u) == 0) {
                const double r = fld32(sp + 0x24u);
                c->r[2] = edi + 0xa8u;
                wr32(sp - 4u, c->r[2]);
                wrf32(sp - 8u, fto_float(c, r));
                c->r[4] = sp - 8u;
                c->r[1] = self;
                x87_used(c, 1);
                call_fn(c, FN_103af4e0, 0x103b64afu);
            }
            {
                const double r = fld32(sp + 0x24u);
                c->r[0] = edi + 0x78u;
                wr32(sp - 4u, c->r[0]);
                wrf32(sp - 8u, fto_float(c, r));
                c->r[4] = sp - 8u;
                c->r[1] = self;
                x87_used(c, 1);
                call_fn(c, FN_103af5d0, 0x103b64c2u);
            }
            {
                const double life = fld32(edi + 0xa4u);
                c->r[1] = edi + 0x8cu;
                wr32(sp - 4u, c->r[1]);
                wrf32(sp - 8u, fto_float(c, life));
                c->r[1] = self;
                wrf32(sp - 12u, fto_float(c, fld32(edi + 0xa0u)));
                c->r[4] = sp - 12u;
                x87_used(c, 1);
                call_fn(c, FN_103af790, 0x103b64e6u);
            }
        next:
            /* 103b64e6 */
            c->r[0] = rd32(sp + 0x1cu) + 1u;
            c->r[2] = rd32(sp + 0x18u) + 0xb4u;
            wr32(sp + 0x1cu, c->r[0]);
            wr32(sp + 0x18u, c->r[2]);
            if (!((int32_t)c->r[0] < (int32_t)ebx))
                break;
            c->r[1] = rd32(sp + 0x28u);
        }
        c->r[0] = rd32(sp + 0x20u);
    }
    /* 103b650b */
    c->r[7] = rd32(sp);
    c->r[6] = rd32(sp + 4u);
    c->r[3] = rd32(sp + 8u);
    c->r[5] = rd32(ebp);
    c->eip = rd32(e0);
    c->r[4] = e0 + 8u;
    recomp_return(c);
}

/* ------------------------------------------------------------------ UpdateVertices */

/* SharedBase imports UpdateVertices calls besides the ones above. */
#define IAT_MATRIX_ASSIGN 0x105f7180u    /* bCMatrix::operator=(bCMatrix const &), RET 4 */
#define IAT_MATRIX_INVERT_TO 0x105f68a8u /* bCMatrix::GetInverted(bCMatrix &) const, RET 4 */
#define IAT_GETZERO 0x105f6d34u          /* static bCVector::GetZero() -> EAX */
#define IAT_FCOLOR_CTOR 0x105f6a7cu      /* bCFloatAlphaColor::bCFloatAlphaColor() */
#define IAT_FCOLOR_VFT 0x105f6a74u       /* data: bCFloatAlphaColor's vftable */
#define IAT_FCOLOR_SET 0x105f6a70u       /* bCFloatAlphaColor::SetColor(r, g, b, a), RET 0x10 */
#define IAT_GETINVERTED 0x105f68b0u      /* bCVector::GetInverted() const -> out, RET 4 */
#define IAT_TRANSFORMDIR 0x105f684cu     /* bCVector::TransformDirection(bCMatrix const &), RET 4 */
#define IAT_GETUP 0x105f6d38u            /* static bCVector::GetUp() -> EAX */
#define IAT_ISEQUAL 0x105f686cu          /* bCVector::IsEqual(bCVector const &) const, AL, RET 4 */
#define IAT_SETCOORDS 0x105f649cu        /* bCVector::SetCoordinates(x, y, z), RET 0xc */
#define IAT_SETCROSS 0x105f67f4u         /* bCVector::SetCrossProduct(a, b), RET 8 */
#define IAT_MATRIX_INVERT_TO2 0x105f6810u /* bCMatrix::GetInverted(bCMatrix &) const, RET 4 */
#define IAT_MUL_MATRIX 0x105f6728u       /* bCVector::operator*=(bCMatrix const &), RET 4 */
#define IAT_GETTRANSLATED 0x105f6774u    /* bCVector::GetTranslated(bCVector const &) const -> out, RET 8 */
#define IAT_AXISANGLE_ROT 0x105f6d3cu    /* bCMatrix3::SetToRotation(bCAxisAngle const &), RET 4 */
#define IAT_GETNORMALIZED 0x105f6870u    /* bCVector::GetNormalized() const -> out, RET 4 */
#define IAT_GETDOT 0x105f6854u           /* bCVector::GetDotProduct(bCVector const &) const, ST0, RET 4 */
#define IAT_BCOLOR_CTOR 0x105f6bc4u      /* bCByteAlphaColor::bCByteAlphaColor(bCFloatAlphaColor const &) */
#define IAT_BCOLOR_ARGB 0x105f6adcu      /* bCByteAlphaColor::GetColor_ARGB_8888() const, EAX */
#define IAT_BCOLOR_VFT 0x105f63ecu       /* data: bCByteAlphaColor's vftable */
#define IAT_FCOLOR_CLAMP 0x105f6c4cu     /* bCFloatAlphaColor::Clamp() */

#define ENGINE_WORLD 0x10862a88u   /* the object whose +0x5e8 is the frame number */
#define ENGINE_MINUS_ONE 0x106e847cu

#ifndef FN_103afa70
#define FN_103afa70 fn_103afa70
#endif
#ifndef FN_103b2d20
#define FN_103b2d20 fn_103b2d20
#endif
#ifndef FN_10277b80
#define FN_10277b80 fn_10277b80
#endif
#ifndef FN_102ec7c0
#define FN_102ec7c0 fn_102ec7c0
#endif
#ifndef FN_102f6cb0
#define FN_102f6cb0 fn_102f6cb0
#endif
#ifndef FN_102f6cf0
#define FN_102f6cf0 fn_102f6cf0
#endif
#ifndef FN_103abc20
#define FN_103abc20 fn_103abc20
#endif
#ifndef FN_103abc50
#define FN_103abc50 fn_103abc50
#endif
#ifndef FN_102995d0
#define FN_102995d0 fn_102995d0
#endif
#ifndef FN_10379d20
#define FN_10379d20 fn_10379d20
#endif
#ifndef FN_103bf0c0
#define FN_103bf0c0 fn_103bf0c0
#endif
void fn_103afa70(X86 *c); /* Engine DepthSortParticles */
void fn_103b2d20(X86 *c); /* Engine CreateVerticesFX */
void fn_10277b80(X86 *c); /* Engine the render module singleton */
void fn_102ec7c0(X86 *c); /* Engine GetActiveCamera */
void fn_102f6cb0(X86 *c); /* Engine eCGfxVertexData::LockBuffer */
void fn_102f6cf0(X86 *c); /* Engine eCGfxVertexData::UnlockBuffer */
void fn_103abc20(X86 *c); /* Engine eCParticle2 dead test (ECX) */
void fn_103abc50(X86 *c); /* Engine eCParticle2 invisible test (ESI) */
void fn_102995d0(X86 *c); /* Engine scaled copy: [ESI] = [EAX] * f, EAX = ESI */
void fn_10379d20(X86 *c); /* Engine floor (ST0) */
void fn_103bf0c0(X86 *c); /* Engine ceil-like (ST0) */

static uint64_t ic_matrix_assign, ic_matrix_invert_to, ic_getzero, ic_fcolor_ctor, ic_fcolor_set, ic_getinverted,
    ic_transformdir, ic_getup, ic_isequal, ic_setcoords, ic_setcross, ic_matrix_invert_to2, ic_mul_matrix,
    ic_gettranslated, ic_axisangle_rot, ic_getnormalized, ic_getdot, ic_bcolor_ctor, ic_bcolor_argb, ic_fcolor_clamp;
static uint64_t ic_vt_36a8, ic_vt_4697, ic_vt_46ce, ic_vt_4c46;

static inline void push(X86 *c, uint32_t v) {
    c->r[4] -= 4u;
    wr32(c->r[4], v);
}
/* PUSH ECX; ... FSTP float ptr [ESP]: a float argument in the slot ECX was pushed into. */
static inline void push_f(X86 *c, double v) {
    push(c, c->r[1]);
    wrf32(c->r[4], fto_float(c, v));
}
/* LAHF after an x87 compare (the kit's fcomi/fucomi left the flags) */
static inline void lahf(X86 *c) {
    c->r[0] = (c->r[0] & 0xffff00ffu) | (((x86_get_eflags(c) & 0xd5u) | 0x02u) << 8);
}
/* FLD float [a]; FSTP float [s]; FLD float [s]; FLDZ; FUCOMIP; FSTP ST0; LAHF; TEST AH,0x44:
 * returns whether JNP is taken (the value is an ordered 0). */
static inline int uv_is_zero(X86 *c, uint32_t a, uint32_t s) {
    wrf32(s, fto_float(c, fld32(a)));
    const double v = fld32(s);
    fucomi(c, 0.0, v);
    lahf(c);
    x87_used(c, 2);
    return v == 0.0;
}
/* MOV ECX,[v]; PUSH ECX; LEA EDX,[out]; PUSH EDX; MOV ECX,EAX; CALL [slot] (GetTranslated or
 * GetInvTranslated of the vector in EAX by the one at [v]). */
static inline void uv_translated(X86 *c, uint32_t slot, uint64_t *ic, uint32_t v, uint32_t out, uint32_t ret) {
    c->r[1] = rd32(v);
    push(c, c->r[1]);
    c->r[2] = out;
    push(c, out);
    c->r[1] = c->r[0];
    call_iat(c, slot, ic, ret);
}
/* reg = a; PUSH reg; LEA ECX,[self]; CALL [slot] */
static inline void uv_call1(X86 *c, int reg, uint32_t a, uint32_t self, uint32_t slot, uint64_t *ic, uint32_t ret) {
    c->r[reg] = a;
    push(c, a);
    c->r[1] = self;
    call_iat(c, slot, ic, ret);
}
/* LEA r1,[b]; PUSH; LEA r2,[a]; PUSH; LEA ECX,[self]; CALL SetCrossProduct (self = a x b) */
static inline void uv_cross(X86 *c, int r1, uint32_t b, int r2, uint32_t a, uint32_t self, uint32_t ret) {
    c->r[r1] = b;
    push(c, b);
    c->r[r2] = a;
    push(c, a);
    c->r[1] = self;
    call_iat(c, IAT_SETCROSS, &ic_setcross, ret);
}

/* 103b4109..103b43f6: one of the three spins. With the angle [EBX+aoff] other than 0, a rotation
 * about the axis at `axis` (through the bCAxisAngle at SP+0xac and the bCMatrix3 at SP+0xbc) is
 * applied to the eight corner vectors and the right vector. The registers that carry &matrix
 * rotate through ECX EDX EAX from `first`. */
static void uv_spin(X86 *c, uint32_t sp, uint32_t p, uint32_t aoff, uint32_t axis, int aa_reg, int first,
                    uint32_t rot_ret, const uint32_t *rets) {
    static const uint32_t vecs[9] = {0x118u, 0x98u, 0x124u, 0x60u, 0x54u, 0x78u, 0x6cu, 0x84u, 0x24u};
    static const int regs[3] = {1, 2, 0}; /* ECX EDX EAX */
    for (uint32_t k = 0; k < 12u; k += 4u) {
        xmm_load_f32_bits(c, 0, rd32(axis + k));
        wr32(sp + 0xacu + k, c->xmm[0][0]);
    }
    c->r[aa_reg] = sp + 0xacu;
    xmm_load_f32_bits(c, 0, rd32(p + aoff));
    push(c, sp + 0xacu);
    c->r[1] = sp + 0xbcu;
    wr32(sp + 0xb8u, c->xmm[0][0]);
    call_iat(c, IAT_AXISANGLE_ROT, &ic_axisangle_rot, rot_ret);
    if (first == 0)
        c->r[6] = rd32(IAT_MUL_MATRIX3); /* 103b4174 MOV ESI,[0x105f6d30] */
    for (int k = 0; k < 9; ++k) {
        const int r = regs[(first + k) % 3];
        c->r[r] = sp + 0xbcu;
        push(c, sp + 0xbcu);
        c->r[1] = sp + vecs[k];
        call_reg(c, c->r[6], &ic_mul_matrix3, rets[k]);
    }
}

/* 103b3820..103b4bf9: the four vertices of one particle. */
static void uv_particle(X86 *c, uint32_t self, uint32_t sp, uint32_t p) {
    /* 103b384b: the orientation of the quad by [this+0x13c]: the right vector A at SP+0x24, the
     * up vector B at SP+0x40, flags at SP+0x1a/0x1b which cross products to redo */
    const uint32_t A = sp + 0x24u, B = sp + 0x40u, C = sp + 0x34u;
    c->r[0] = rd32(self + 0x13cu) - 1u;
    switch (c->r[0]) {
    case 0: /* 103b3864 */
        uv_call1(c, 1, sp + 0x3d8u, A, IAT_SETVECTOR, &ic_setvector, 0x103b3876u);
        uv_call1(c, 2, p + 0x18u, sp + 0x1d4u, IAT_SETVECTOR, &ic_setvector, 0x103b3887u);
        uv_call1(c, 0, p + 0x24u, sp + 0x1d4u, IAT_SCALE_V, &ic_scale_v, 0x103b3898u);
        uv_call1(c, 1, sp + 0x21cu, sp + 0x1d4u, IAT_GETINVERTED, &ic_getinverted, 0x103b38adu);
        uv_call1(c, 2, sp + 0x21cu, B, IAT_SETVECTOR, &ic_setvector, 0x103b38bfu);
        wr8(sp + 0x1bu, 1);
        wr8(sp + 0x1au, 0);
        break;
    case 2: /* 103b38c9 */
        uv_call1(c, 0, self + 0x140u, A, IAT_SETVECTOR, &ic_setvector, 0x103b38dau);
        uv_call1(c, 1, p + 0x18u, sp + 0x1c8u, IAT_SETVECTOR, &ic_setvector, 0x103b38ebu);
        uv_call1(c, 2, p + 0x24u, sp + 0x1c8u, IAT_SCALE_V, &ic_scale_v, 0x103b38fcu);
        uv_call1(c, 0, sp + 0x258u, sp + 0x1c8u, IAT_GETINVERTED, &ic_getinverted, 0x103b3911u);
        uv_call1(c, 1, sp + 0x258u, B, IAT_SETVECTOR, &ic_setvector, 0x103b3923u);
        c->r[0] = rd32(self + 0x15cu) - 1u;
        if (c->r[0] == 0u)
            uv_call1(c, 2, rd32(sp + 0x50u), A, IAT_TRANSFORMDIR, &ic_transformdir, 0x103b393du);
        wr8(sp + 0x1au, 1);
        wr8(sp + 0x1bu, 0);
        break;
    case 1:   /* 103b394c */
    case 3: { /* 103b3a19 */
        const int c4 = c->r[0] == 3u;
        if (!c4) {
            uv_call1(c, 0, self + 0x140u, A, IAT_SETVECTOR, &ic_setvector, 0x103b395du);
        } else {
            uv_call1(c, 2, p + 0x18u, sp + 0x1e0u, IAT_SETVECTOR, &ic_setvector, 0x103b3a2au);
            uv_call1(c, 0, p + 0x24u, sp + 0x1e0u, IAT_SCALE_V, &ic_scale_v, 0x103b3a3bu);
            uv_call1(c, 1, sp + 0x1e0u, A, IAT_SETVECTOR, &ic_setvector, 0x103b3a4du);
        }
        c->r[0] = rd32(self + 0x15cu) - 1u;
        if (c->r[0] == 0u)
            uv_call1(c, c4 ? 2 : 1, rd32(sp + 0x50u), A, IAT_TRANSFORMDIR, &ic_transformdir,
                     c4 ? 0x103b3a67u : 0x103b3977u);
        call_iat(c, IAT_GETUP, &ic_getup, c4 ? 0x103b3a6du : 0x103b397du);
        push(c, c->r[0]);
        c->r[1] = A;
        call_iat(c, IAT_ISEQUAL, &ic_isequal, c4 ? 0x103b3a78u : 0x103b3988u);
        /* the side vector: (-z, 0, x) or, along the up axis, (0, -z, y), crossed with A */
        xmm_load_f32_bits(c, 0, rd32(ENGINE_CLAMP_BASE));
        xmm_set_f32(c, 0, xmm_f32(c, 0) - rdf32(sp + 0x2cu));
        c->r[4] = sp - 12u;
        c->r[1] = C;
        const int eq = (c->r[0] & 0xffu) != 0u;
        uint32_t out, ret1, ret2;
        if (!eq) {
            wrf32(sp - 4u, fto_float(c, fld32(sp + 0x24u)));
            wrf32(sp - 8u, fto_float(c, 0.0));
            wr32(sp - 12u, c->xmm[0][0]);
            ret1 = c4 ? 0x103b3aaau : 0x103b39bau;
            ret2 = c4 ? 0x103b3ac1u : 0x103b39d1u;
            out = c4 ? sp + 0x2a0u : sp + 0x270u;
        } else {
            wrf32(sp - 4u, fto_float(c, fld32(sp + 0x28u)));
            wr32(sp - 8u, c->xmm[0][0]);
            wrf32(sp - 12u, fto_float(c, 0.0));
            ret1 = c4 ? 0x103b3ae4u : 0x103b39f6u;
            ret2 = c4 ? 0x103b3afbu : 0x103b3a0du;
            out = c4 ? sp + 0x204u : sp + 0x288u;
        }
        x87_used(c, 1);
        call_iat(c, IAT_SETCOORDS, &ic_setcoords, ret1);
        if (c4)
            uv_cross(c, 0, A, 1, C, out, ret2);
        else
            uv_cross(c, 2, A, 0, C, out, ret2);
        if (c4)
            c->r[2] = out;
        else
            c->r[1] = out;
        push(c, out);
        c->r[1] = B;
        call_iat(c, IAT_SETVECTOR, &ic_setvector, 0x103b3b29u);
        wr8(sp + 0x1bu, 0);
        wr8(sp + 0x1au, 0);
        break;
    }
    default: /* 103b3b05 */
        uv_call1(c, 0, sp + 0x3d8u, A, IAT_SETVECTOR, &ic_setvector, 0x103b3b17u);
        c->r[1] = sp + 0x3c8u;
        push(c, c->r[1]);
        c->r[1] = B;
        call_iat(c, IAT_SETVECTOR, &ic_setvector, 0x103b3b29u);
        wr8(sp + 0x1bu, 0);
        wr8(sp + 0x1au, 0);
        break;
    }

    /* 103b3b33: the normal C = A x B, then B or A redone from it, all three normalised */
    uv_cross(c, 2, B, 0, A, sp + 0x27cu, 0x103b3b4au);
    uv_call1(c, 1, sp + 0x27cu, C, IAT_SETVECTOR, &ic_setvector, 0x103b3b5cu);
    c->r[1] = C;
    call_iat(c, IAT_NORMALIZE, &ic_normalize, 0x103b3b66u);
    if (rd8(sp + 0x1bu)) {
        uv_cross(c, 2, C, 0, A, sp + 0x1ecu, 0x103b3b84u);
        uv_call1(c, 1, sp + 0x1ecu, B, IAT_SETVECTOR, &ic_setvector, 0x103b3b96u);
    }
    if (rd8(sp + 0x1au)) {
        uv_cross(c, 2, C, 0, B, sp + 0x264u, 0x103b3bb4u);
        uv_call1(c, 1, sp + 0x264u, A, IAT_SETVECTOR, &ic_setvector, 0x103b3bc6u);
    }
    c->r[1] = B;
    call_iat(c, IAT_NORMALIZE, &ic_normalize, 0x103b3bd0u);
    c->r[1] = A;
    call_iat(c, IAT_NORMALIZE, &ic_normalize, 0x103b3bdau);
    /* 103b3bda: local coordinates: the three into the entity's frame (rotation only) */
    c->r[0] = rd32(self + 0x84u) - 1u;
    if (c->r[0] == 0u) {
        c->r[1] = rd32(sp + 0x50u);
        c->r[2] = sp + 0x378u;
        push(c, c->r[2]);
        call_iat(c, IAT_MATRIX_INVERT_TO2, &ic_matrix_invert_to2, 0x103b3bf7u);
        c->r[4] = sp - 12u;
        wrf32(sp - 4u, fto_float(c, 0.0));
        wrf32(sp - 8u, fto_float(c, 0.0));
        c->r[1] = sp + 0x3a8u;
        wrf32(sp - 12u, fto_float(c, 0.0));
        x87_used(c, 1);
        call_iat(c, IAT_SETCOORDS, &ic_setcoords, 0x103b3c14u);
        c->r[6] = rd32(IAT_MUL_MATRIX);
        c->r[0] = sp + 0x378u;
        push(c, c->r[0]);
        c->r[1] = A;
        call_reg(c, c->r[6], &ic_mul_matrix, 0x103b3c28u);
        c->r[1] = sp + 0x378u;
        push(c, c->r[1]);
        c->r[1] = B;
        call_reg(c, c->r[6], &ic_mul_matrix, 0x103b3c36u);
        c->r[2] = sp + 0x378u;
        push(c, c->r[2]);
        c->r[1] = C;
        call_reg(c, c->r[6], &ic_mul_matrix, 0x103b3c44u);
    }
    /* 103b3c44: the size, [EBX+0x60] * k scaled by [EBX+0x6c], at SP+0xe0 */
    push_f(c, fld32(ENGINE_GRAVITY_K));
    c->r[0] = sp + 0x234u;
    push(c, c->r[0]);
    c->r[1] = p + 0x60u;
    x87_used(c, 1);
    call_iat(c, IAT_GETSCALED, &ic_getscaled, 0x103b3c5fu);
    uv_call1(c, 1, sp + 0x234u, sp + 0xe0u, IAT_SETVECTOR, &ic_setvector, 0x103b3c74u);
    uv_call1(c, 2, p + 0x6cu, sp + 0xe0u, IAT_SCALE_V, &ic_scale_v, 0x103b3c85u);

    /* 103b3c85: the corners. Each is B * ((k1 +- 1) * size.y) + C * ((k0 +- 1) * size.x) with
     * k0, k1 = [this+0x14c], [this+0x150], then the four texture-space corners at +-1. */
    {
        const double one = rdf64(ENGINE_ONE_D);
        /* corner 0: (+, +) */
        wrf32(sp + 0x30u, fto_float(c, fld32(self + 0x150u)));
        push(c, c->r[1]);
        {
            const double k0 = fld32(self + 0x14cu);
            c->r[0] = B;
            wrf32(sp + 0x20u, fto_float(c, k0));
        }
        c->r[6] = sp + 0x300u;
        wrf32(sp + 0x1cu, fto_float(c, fld32(sp + 0xe0u)));
        wrf32(sp + 0x30u, fto_float(c, fx87(c, fx87(c, fld32(sp + 0x30u) + one) * fld32(sp + 0xe4u))));
        wrf32(sp - 4u, fto_float(c, fld32(sp + 0x30u)));
        x87_used(c, 1);
        call_fn(c, FN_102995d0, 0x103b3cd1u);
        {
            double v = fld32(sp + 0x20u);
            wr32(sp + 0x14u, c->r[0]);
            v = fx87(c, v + one);
            push(c, c->r[1]);
            c->r[0] = C;
            c->r[6] = sp + 0x2acu;
            wrf32(sp + 0x30u, fto_float(c, fx87(c, v * fld32(sp + 0x1cu))));
            wrf32(sp - 4u, fto_float(c, fld32(sp + 0x30u)));
            x87_used(c, 1);
        }
        call_fn(c, FN_102995d0, 0x103b3cffu);
        uv_translated(c, IAT_GETTRANSLATED, &ic_gettranslated, sp + 0x14u, sp + 0x294u, 0x103b3d14u);
        uv_call1(c, 0, sp + 0x294u, sp + 0x118u, IAT_SETVECTOR, &ic_setvector, 0x103b3d29u);

        /* corners 1..3: (+, -), (-, +), (-, -) */
        static const struct {
            double s1, s0;
            uint32_t o1, o0, out, dst, r1, r2, r3, r4;
        } k[3] = {
            {1.0, -1.0, 0x318u, 0x330u, 0x24cu, 0x98u, 0x103b3d75u, 0x103b3da3u, 0x103b3db8u, 0x103b3dcdu},
            {-1.0, 1.0, 0x348u, 0x2c4u, 0x1f8u, 0x124u, 0x103b3e19u, 0x103b3e47u, 0x103b3e5cu, 0x103b3e71u},
            {-1.0, -1.0, 0x36cu, 0x2dcu, 0x198u, 0u, 0x103b3ebdu, 0x103b3eebu, 0x103b3f00u, 0u},
        };
        for (int i = 0; i < 3; ++i) {
            wrf32(sp + 0x14u, fto_float(c, fld32(self + 0x150u)));
            push(c, c->r[1]);
            {
                const double k0 = fld32(self + 0x14cu);
                c->r[0] = B;
                wrf32(sp + 0x30u, fto_float(c, k0));
            }
            c->r[6] = sp + k[i].o1;
            wrf32(sp + 0x20u, fto_float(c, fld32(sp + 0xe0u)));
            {
                const double a = k[i].s1 > 0 ? fx87(c, fld32(sp + 0x14u) + one) : fx87(c, fld32(sp + 0x14u) - one);
                wrf32(sp + 0x14u, fto_float(c, fx87(c, a * fld32(sp + 0xe4u))));
            }
            wrf32(sp - 4u, fto_float(c, fld32(sp + 0x14u)));
            x87_used(c, 1);
            call_fn(c, FN_102995d0, k[i].r1);
            {
                double v = fld32(sp + 0x30u);
                wr32(sp + 0x1cu, c->r[0]);
                v = k[i].s0 > 0 ? fx87(c, v + one) : fx87(c, v - one);
                push(c, c->r[1]);
                c->r[0] = C;
                c->r[6] = sp + k[i].o0;
                wrf32(sp + 0x14u, fto_float(c, fx87(c, v * fld32(sp + 0x20u))));
                wrf32(sp - 4u, fto_float(c, fld32(sp + 0x14u)));
                x87_used(c, 1);
            }
            call_fn(c, FN_102995d0, k[i].r2);
            uv_translated(c, IAT_GETTRANSLATED, &ic_gettranslated, sp + 0x1cu, sp + k[i].out, k[i].r3);
            if (k[i].dst)
                uv_call1(c, 0, sp + k[i].out, sp + k[i].dst, IAT_SETVECTOR, &ic_setvector, k[i].r4);
        }
        /* 103b3f00: corner 3 copied to SP+0x60; then B + C, B - C, -B + C, -B - C (the four
         * texture-space directions) to +0x54, +0x78, +0x6c, +0x84 */
        static const struct {
            double fb, fc;
            uint32_t ob, oc, out, dst, rb, rc, rt;
            int inv;
        } t[4] = {
            {1.0, 1.0, 0x33cu, 0x2f4u, 0x1b0u, 0x54u, 0x103b3f43u, 0x103b3f5du, 0x103b3f72u, 0},
            {1.0, -1.0, 0x360u, 0x30cu, 0x1a4u, 0x78u, 0x103b3fb5u, 0x103b3fd3u, 0x103b3fe8u, 0},
            {1.0, 1.0, 0x354u, 0x324u, 0x18cu, 0x6cu, 0x103b402eu, 0x103b4048u, 0x103b405du, 1},
            {1.0, -1.0, 0x2b8u, 0x2d0u, 0x180u, 0x84u, 0x103b40a0u, 0x103b40beu, 0x103b40d3u, 1},
        };
        uint32_t prev = sp + 0x198u, prev_dst = sp + 0x60u;
        for (int i = 0; i < 4; ++i) {
            /* copy the previous result to its slot, interleaved with the FLD1 and the push as the
             * original has them */
            xmm_load_f32_bits(c, 0, rd32(prev));
            wr32(prev_dst, c->xmm[0][0]);
            xmm_load_f32_bits(c, 0, rd32(prev + 4u));
            wr32(prev_dst + 4u, c->xmm[0][0]);
            xmm_load_f32_bits(c, 0, rd32(prev + 8u));
            push(c, c->r[1]);
            if (i == 2) {
                wrf32(c->r[4], fto_float(c, 1.0));
                wr32(prev_dst + 8u, c->xmm[0][0]);
                c->r[0] = B;
                c->r[6] = sp + t[i].ob;
            } else {
                c->r[0] = B;
                wrf32(c->r[4], fto_float(c, 1.0));
                c->r[6] = sp + t[i].ob;
                wr32(prev_dst + 8u, c->xmm[0][0]);
            }
            x87_used(c, 1);
            call_fn(c, FN_102995d0, t[i].rb);
            {
                const double f = t[i].fc > 0 ? 1.0 : fld32(ENGINE_MINUS_ONE);
                wr32(sp + 0x14u, c->r[0]);
                push(c, c->r[1]);
                c->r[0] = C;
                wrf32(c->r[4], fto_float(c, f));
                c->r[6] = sp + t[i].oc;
                x87_used(c, 1);
            }
            call_fn(c, FN_102995d0, t[i].rc);
            if (t[i].inv)
                uv_translated(c, IAT_GETINVTRANSLATED, &ic_getinvtranslated, sp + 0x14u, sp + t[i].out, t[i].rt);
            else
                uv_translated(c, IAT_GETTRANSLATED, &ic_gettranslated, sp + 0x14u, sp + t[i].out, t[i].rt);
            prev = sp + t[i].out;
            prev_dst = sp + t[i].dst;
        }
        for (uint32_t q = 0; q < 12u; q += 4u) {
            xmm_load_f32_bits(c, 0, rd32(prev + q));
            wr32(prev_dst + q, c->xmm[0][0]);
        }
    }

    /* 103b4109: the three spins: about C by [EBX+0x48], about B by [EBX+0x4c], about A by
     * [EBX+0x50] */
    {
        static const uint32_t r0[9] = {0x103b418bu, 0x103b419cu, 0x103b41adu, 0x103b41bbu, 0x103b41c9u,
                                       0x103b41d7u, 0x103b41e5u, 0x103b41f6u, 0x103b4204u};
        static const uint32_t r1[9] = {0x103b4288u, 0x103b4299u, 0x103b42aau, 0x103b42b8u, 0x103b42c6u,
                                       0x103b42d4u, 0x103b42e2u, 0x103b42f3u, 0x103b4301u};
        static const uint32_t r2[9] = {0x103b437du, 0x103b438eu, 0x103b439fu, 0x103b43adu, 0x103b43bbu,
                                       0x103b43c9u, 0x103b43d7u, 0x103b43e8u, 0x103b43f6u};
        if (!uv_is_zero(c, p + 0x48u, sp + 0x14u))
            uv_spin(c, sp, p, 0x48u, C, 0, 0, 0x103b4174u, r0);
        else
            c->r[6] = rd32(IAT_MUL_MATRIX3); /* 103b4206 */
        if (!uv_is_zero(c, p + 0x4cu, sp + 0x14u))
            uv_spin(c, sp, p, 0x4cu, B, 1, 1, 0x103b4277u, r1);
        if (!uv_is_zero(c, p + 0x50u, sp + 0x14u))
            uv_spin(c, sp, p, 0x50u, A, 2, 2, 0x103b436cu, r2);
    }

    /* 103b43f6: to the particle's position */
    c->r[6] = p + 0xcu;
    push(c, c->r[6]);
    c->r[1] = sp + 0x118u;
    call_iat(c, IAT_TRANSLATE, &ic_translate, 0x103b4407u);
    push(c, c->r[6]);
    c->r[1] = sp + 0x98u;
    call_iat(c, IAT_TRANSLATE, &ic_translate, 0x103b4415u);
    push(c, c->r[6]);
    c->r[1] = sp + 0x124u;
    call_iat(c, IAT_TRANSLATE, &ic_translate, 0x103b4423u);
    push(c, c->r[6]);
    c->r[1] = sp + 0x60u;
    call_iat(c, IAT_TRANSLATE, &ic_translate, 0x103b442eu);
    /* 103b442e: which side faces the camera: (camera - position) . A, in world coordinates */
    {
        const uint32_t esi = c->r[6];
        xmm_load_f32_bits(c, 0, rd32(esi));
        c->r[0] = rd32(self + 0x84u) - 1u;
        wr32(sp + 0x148u, c->xmm[0][0]);
        xmm_load_f32_bits(c, 0, rd32(esi + 4u));
        wr32(sp + 0x14cu, c->xmm[0][0]);
        xmm_load_f32_bits(c, 0, rd32(esi + 8u));
        wr32(sp + 0x150u, c->xmm[0][0]);
        for (uint32_t q = 0; q < 12u; q += 4u) {
            xmm_load_f32_bits(c, 0, rd32(A + q));
            wr32(sp + 0x154u + q, c->xmm[0][0]);
        }
        if (c->r[0] == 0u) {
            c->r[0] = rd32(sp + 0x50u) + 0x30u;
            push(c, c->r[0]);
            c->r[1] = sp + 0x148u;
            call_iat(c, IAT_TRANSLATE, &ic_translate, 0x103b44a4u);
        }
        c->r[0] = rd32(self + 0x15cu) - 1u;
        if (c->r[0] == 0u)
            uv_call1(c, 1, rd32(sp + 0x50u), sp + 0x154u, IAT_TRANSFORMDIR, &ic_transformdir, 0x103b44c1u);
        c->r[2] = sp + 0x148u;
        push(c, c->r[2]);
        c->r[0] = sp + 0x210u;
        push(c, c->r[0]);
        c->r[1] = sp + 0x3e8u;
        call_iat(c, IAT_GETINVTRANSLATED, &ic_getinvtranslated, 0x103b44deu);
        c->r[1] = sp + 0x154u;
        push(c, c->r[1]);
        c->r[2] = sp + 0x2e8u;
        push(c, c->r[2]);
        c->r[1] = sp + 0x210u;
        call_iat(c, IAT_GETNORMALIZED, &ic_getnormalized, 0x103b44fbu);
        c->r[1] = c->r[0];
        call_iat(c, IAT_GETDOT, &ic_getdot, 0x103b4503u);
        wrf32(sp + 0x30u, fto_float(c, x87_take(c)));
        const double d = fld32(sp + 0x30u);
        const int le = fcomi_be(c, 0.0, d);
        x87_used(c, 2);
        if (!le) { /* 0 > d: the back faces the camera, flip A */
            push_f(c, fld32(ENGINE_MINUS_ONE));
            c->r[1] = A;
            x87_used(c, 1);
            call_iat(c, IAT_SCALE_F, &ic_scale_f, 0x103b4527u);
        }
    }

    /* 103b4527: the corner normals by [this+0x234] */
    c->r[0] = rd32(self + 0x234u);
    switch (c->r[0]) {
    case 0:
    case 1:
    case 3: {
        if (c->r[0] == 3u) { /* 103b4598: from the particle towards the camera */
            push(c, c->r[6]);
            c->r[1] = sp + 0x160u;
            call_iat(c, IAT_SETVECTOR, &ic_setvector, 0x103b45a6u);
            c->r[1] = sp + 0x1bcu;
            push(c, c->r[1]);
            c->r[1] = sp + 0x160u;
            call_iat(c, IAT_INVTRANSLATE, &ic_invtranslate, 0x103b45bbu);
            xmm_load_f32_bits(c, 0, rd32(sp + 0x160u));
            xmm_load_f32_bits(c, 1, rd32(sp + 0x164u));
            xmm_load_f32_bits(c, 2, rd32(sp + 0x168u));
            wr32(sp + 0x24u, c->xmm[0][0]);
            wr32(sp + 0x28u, c->xmm[1][0]);
            wr32(sp + 0x2cu, c->xmm[2][0]);
        } else { /* 103b453d */
            xmm_load_f32_bits(c, 0, rd32(sp + 0x24u));
            xmm_load_f32_bits(c, 1, rd32(sp + 0x28u));
            xmm_load_f32_bits(c, 2, rd32(sp + 0x2cu));
        }
        /* 103b45e8: A into the four normal slots */
        static const uint32_t slots[4] = {0x84u, 0x6cu, 0x78u, 0x54u};
        for (int i = 0; i < 4; ++i) {
            wr32(sp + slots[i] + 8u, c->xmm[2][0]);
            wr32(sp + slots[i] + 4u, c->xmm[1][0]);
            wr32(sp + slots[i], c->xmm[0][0]);
        }
        break;
    }
    case 2: /* 103b4554 */
        uv_call1(c, 0, A, sp + 0x54u, IAT_TRANSLATE, &ic_translate, 0x103b4563u);
        uv_call1(c, 1, A, sp + 0x78u, IAT_TRANSLATE, &ic_translate, 0x103b4572u);
        uv_call1(c, 2, A, sp + 0x6cu, IAT_TRANSLATE, &ic_translate, 0x103b4581u);
        uv_call1(c, 0, A, sp + 0x84u, IAT_TRANSLATE, &ic_translate, 0x103b4593u);
        break;
    default:
        break;
    }

    /* 103b463c: billboards locked to the entity: positions 1 and 3 from it */
    int zeroed = 0;
    if (rd8(self + 0x1c0u)) {
        c->r[0] = rd32(self + 0x84u);
        if (c->r[0] == 0u) {
            c->r[2] = rd32(self);
            c->r[0] = rd32(c->r[2] + 0x68u);
            c->r[1] = self;
            call_reg(c, c->r[0], &ic_vt_4697, 0x103b4699u);
            xmm_load_f32_bits(c, 0, rd32(c->r[0] + 0x70u));
            c->r[2] = rd32(self);
            c->r[0] += 0x70u;
            wr32(sp + 0x98u, c->xmm[0][0]);
            xmm_load_f32_bits(c, 0, rd32(c->r[0] + 4u));
            wr32(sp + 0x9cu, c->xmm[0][0]);
            xmm_load_f32_bits(c, 0, rd32(c->r[0] + 8u));
            c->r[0] = rd32(c->r[2] + 0x68u);
            c->r[1] = self;
            wr32(sp + 0xa0u, c->xmm[0][0]);
            call_reg(c, c->r[0], &ic_vt_46ce, 0x103b46d0u);
            xmm_load_f32_bits(c, 0, rd32(c->r[0] + 0x70u));
            c->r[0] += 0x70u;
            wr32(sp + 0x60u, c->xmm[0][0]);
            xmm_load_f32_bits(c, 0, rd32(c->r[0] + 4u));
            wr32(sp + 0x64u, c->xmm[0][0]);
            xmm_load_f32_bits(c, 0, rd32(c->r[0] + 8u));
            wr32(sp + 0x68u, c->xmm[0][0]);
        } else if (c->r[0] == 1u) {
            c->r[0] = 0u;
            xmm_zero(c, 0);
            wr32(sp + 0x98u, 0u);
            wr32(sp + 0x9cu, 0u);
            wr32(sp + 0xa0u, 0u);
            wr32(sp + 0x60u, 0u);
            wr32(sp + 0x64u, 0u);
            wr32(sp + 0x68u, 0u);
            zeroed = 1;
        } else
            c->r[0] -= 1u;
    }
    if (!zeroed)
        xmm_zero(c, 0); /* 103b46f4 */

    /* 103b46f7: texture coordinates, the whole texture unless the texture is a frame atlas */
    {
        const uint32_t one = rd32(ENGINE_ONE_F);
        xmm_load_f32_bits(c, 1, one);
        c->r[0] = 1u;
        wr32(sp + 0xf0u, c->xmm[0][0]);
        wr32(sp + 0x10cu, c->xmm[0][0]);
        wr32(sp + 0xf4u, one);
        wr32(sp + 0x100u, one);
        wr32(sp + 0x114u, c->xmm[0][0]);
        wr32(sp + 0x108u, c->xmm[0][0]);
        wr32(sp + 0xfcu, one);
        wr32(sp + 0x110u, one);
        wr32(sp + 0x104u, c->xmm[0][0]);
    }
    if ((int32_t)rd32(sp + 0x94u) > 1 || (int32_t)rd32(sp + 0x90u) > 1) {
        /* 103b476b: frame and the next frame of the atlas */
        push(c, c->r[1]);
        wrf32(sp + 0x20u, fto_float(c, fld32(p + 0xa8u)));
        wrf32(sp - 4u, fto_float(c, fld32(sp + 0x20u)));
        x87_used(c, 1);
        call_fn(c, FN_10379d20, 0x103b4782u);
        wrf32(sp + 0x1cu, fto_float(c, x87_take(c)));
        x87_used(c, 1);
        push(c, c->r[1]);
        wrf32(sp - 4u, fto_float(c, fld32(sp + 0x20u)));
        x87_used(c, 1);
        call_fn(c, FN_103bf0c0, 0x103b4793u);
        const int cols = rd8(self + 0x1e7u) != 0;
        wrf32(sp + 0x14u, fto_float(c, x87_take(c)));
        {
            const double fr = fld32(p + 0xa8u);
            c->r[0] = (uint32_t)(int32_t)rdf32(sp + 0x1cu);
            const double frac = fx87(c, fr - fld32(sp + 0x1cu));
            c->r[6] = (uint32_t)(int32_t)rdf32(sp + 0x14u);
            c->r[2] = (uint32_t)((int32_t)c->r[0] >> 31);
            wrf32(sp + 0x104u, fto_float(c, frac));
        }
        if (cols) {
            idiv32(c, rd32(sp + 0x94u), 0x103b47beu);
            c->r[1] = c->r[2];
            c->r[2] = c->r[0];
            wr32(sp + 0x20u, c->r[0]);
            c->r[0] = c->r[1];
        } else {
            idiv32(c, rd32(sp + 0x90u), 0x103b47d1u);
            wr32(sp + 0x20u, c->r[2]);
        }
        wr32(sp + 0x1cu, c->r[0]);
        const double a = (double)(int32_t)rd32(sp + 0x1cu);
        c->r[0] += 1u;
        const double du = fld32(sp + 0x130u);
        wr32(sp + 0x14u, c->r[0]);
        c->r[2] += 1u;
        const int two = rd8(self + 0x1e5u) != 0;
        wrf32(sp + 0xf0u, fto_float(c, fx87(c, a * du)));
        const double b = (double)(int32_t)rd32(sp + 0x20u);
        const double dv = fld32(sp + 0xa4u);
        wrf32(sp + 0x10cu, fto_float(c, fx87(c, b * dv)));
        const double a1 = (double)(int32_t)rd32(sp + 0x14u);
        wr32(sp + 0x14u, c->r[2]);
        wrf32(sp + 0xf4u, fto_float(c, fx87(c, a1 * du)));
        const double b1 = (double)(int32_t)rd32(sp + 0x14u);
        wrf32(sp + 0x100u, fto_float(c, fx87(c, b1 * dv)));
        if (two) {
            /* 103b4841: the second frame, for blending */
            c->r[0] = c->r[6];
            c->r[2] = (uint32_t)((int32_t)c->r[0] >> 31);
            if (cols) {
                idiv32(c, rd32(sp + 0x94u), 0x103b484du);
                c->r[1] = c->r[0];
                wr32(sp + 0x20u, c->r[1]);
                c->r[0] = c->r[2];
            } else {
                idiv32(c, rd32(sp + 0x90u), 0x103b485eu);
                c->r[1] = c->r[2];
                wr32(sp + 0x20u, c->r[2]);
            }
            wr32(sp + 0x1cu, c->r[0]);
            const double e = (double)(int32_t)rd32(sp + 0x1cu);
            c->r[0] += 1u;
            wr32(sp + 0x14u, c->r[0]);
            c->r[1] += 1u;
            wrf32(sp + 0x114u, fto_float(c, fx87(c, e * du)));
            const double g = (double)(int32_t)rd32(sp + 0x20u);
            wrf32(sp + 0x108u, fto_float(c, fx87(c, g * dv)));
            const double e1 = (double)(int32_t)rd32(sp + 0x14u);
            wr32(sp + 0x14u, c->r[1]);
            wrf32(sp + 0xfcu, fto_float(c, fx87(c, du * e1)));
            wrf32(sp + 0x110u, fto_float(c, fx87(c, dv * (double)(int32_t)rd32(sp + 0x14u))));
        }
        x87_used(c, 4);
        xmm_zero(c, 0); /* 103b48b7 */
    }

    /* 103b48ba: the life fraction, the facing factors, the two vertex colours */
    {
        const double l = (wrf32(sp + 0x14u, fto_float(c, fld32(p + 0xa4u))), fld32(sp + 0x14u));
        xmm_copy(c, 3, 0);
        fucomi(c, 0.0, l);
        lahf(c);
        if (!(l == 0.0)) {
            wrf32(sp + 0x14u, fto_float(c, fx87(c, fdivz(c, fld32(p + 0xa0u), l))));
            xmm_load_f32_bits(c, 3, rd32(sp + 0x14u));
        }
        x87_used(c, 3);
        xmm_load_f32_bits(c, 2, rd32(sp + 0x30u));
        const float d = xmm_f32(c, 2), z = xmm_f32(c, 0);
        if (d > z) /* COMISS, JBE not taken */
            xmm_copy(c, 1, 2);
        else
            xmm_copy(c, 1, 0);
        if (z > d) {
            xmm_load_f32_bits(c, 0, rd32(ENGINE_CLAMP_BASE));
            xmm_set_f32(c, 0, xmm_f32(c, 0) - xmm_f32(c, 2));
        } else
            xmm_copy(c, 0, 2);
        c->r[1] = sp + 0x16cu;
        wr32(sp + 0x174u, c->xmm[0][0]);
        xmm_load_f32_bits(c, 0, rd32(sp + 0x104u));
        push(c, c->r[1]);
        c->r[1] = sp + 0x228u;
        wr32(sp + 0x170u, c->xmm[3][0]);
        wr32(sp + 0x178u, c->xmm[1][0]);
        wr32(sp + 0x17cu, c->xmm[0][0]);
        call_iat(c, IAT_BCOLOR_CTOR, &ic_bcolor_ctor, 0x103b4958u);
        c->r[1] = c->r[0];
        call_iat(c, IAT_BCOLOR_ARGB, &ic_bcolor_argb, 0x103b4960u);
        /* 103b4960: the colour times its factor, clamped */
        {
            const double r = fx87(c, fld32(p + 0x90u) * fld32(p + 0x7cu));
            c->r[2] = rd32(IAT_BCOLOR_VFT);
            c->r[1] = sp + 0x134u;
            c->r[6] = c->r[0];
            wrf32(sp + 0x138u, fto_float(c, r));
            wr32(sp + 0x228u, c->r[2]);
            wrf32(sp + 0x13cu, fto_float(c, fx87(c, fld32(p + 0x94u) * fld32(p + 0x80u))));
            wrf32(sp + 0x140u, fto_float(c, fx87(c, fld32(p + 0x98u) * fld32(p + 0x84u))));
            wrf32(sp + 0x144u, fto_float(c, fx87(c, fld32(p + 0x9cu) * fld32(p + 0x88u))));
            x87_used(c, 1);
        }
        call_iat(c, IAT_FCOLOR_CLAMP, &ic_fcolor_clamp, 0x103b49c5u);
        uv_call1(c, 0, sp + 0x134u, sp + 0x240u, IAT_BCOLOR_CTOR, &ic_bcolor_ctor, 0x103b49dau);
        c->r[1] = c->r[0];
        call_iat(c, IAT_BCOLOR_ARGB, &ic_bcolor_argb, 0x103b49e2u);
    }

    /* 103b49e2: the vertex write. Four vertices of 0x30 bytes from EDI - 0x14: position, normal,
     * colour, factor colour, two texture coordinate pairs. */
    {
        const uint32_t edi = c->r[7], esi = c->r[6], eax = c->r[0];
        c->r[1] = rd32(IAT_BCOLOR_VFT);
#define VLD(r, o) xmm_load_f32_bits(c, r, rd32(sp + (o)))
#define VST(r, o) wr32(edi + (uint32_t)(o), c->xmm[r][0])
        VLD(0, 0x118u); VST(0, -0x14); VLD(0, 0x11cu); VST(0, -0x10); VLD(0, 0x120u); VST(0, -0xc);
        VLD(0, 0x54u); VST(0, -0x8); VLD(0, 0x58u); VLD(3, 0x114u); VLD(1, 0x10cu); VST(0, -0x4);
        VLD(0, 0x5cu); VLD(4, 0x108u); VST(0, 0); VLD(0, 0xf0u);
        wr32(edi + 4u, esi);
        wr32(edi + 8u, eax);
        VST(0, 0xc); VST(1, 0x10); VST(3, 0x14); VST(4, 0x18);
        VLD(2, 0x124u); VST(2, 0x1c); VLD(2, 0x128u); VST(2, 0x20); VLD(2, 0x12cu); VST(2, 0x24);
        VLD(2, 0x6cu); VST(2, 0x28); VLD(2, 0x70u); VST(2, 0x2c); VLD(2, 0x74u); VST(2, 0x30);
        VLD(2, 0x100u);
        wr32(edi + 0x34u, esi);
        wr32(edi + 0x38u, eax);
        VST(0, 0x3c); VST(2, 0x40); VST(3, 0x44); VLD(3, 0x110u); VST(3, 0x48);
        VLD(0, 0x60u); VST(0, 0x4c); VLD(0, 0x64u);
        wr32(sp + 0x240u, c->r[1]);
        VST(0, 0x50); VLD(0, 0x68u); VST(0, 0x54);
        VLD(0, 0x84u); VST(0, 0x58); VLD(0, 0x88u); VST(0, 0x5c); VLD(0, 0x8cu); VST(0, 0x60);
        VLD(0, 0xf4u);
        wr32(edi + 0x64u, esi);
        wr32(edi + 0x68u, eax);
        VST(2, 0x70); VLD(2, 0xfcu); VST(0, 0x6c); VST(2, 0x74); VST(3, 0x78);
        VLD(3, 0x98u);
        wr32(sp + 0x4cu, rd32(sp + 0x4cu) + 1u);
        VST(3, 0x7c); VLD(3, 0x9cu); VST(3, 0x80); VLD(3, 0xa0u); VST(3, 0x84);
        VLD(3, 0x78u); VST(3, 0x88); VLD(3, 0x7cu); VST(3, 0x8c); VLD(3, 0x80u); VST(3, 0x90);
        wr32(edi + 0x94u, esi);
        wr32(edi + 0x98u, eax);
        VST(0, 0x9c); VST(1, 0xa0); VST(2, 0xa4); VST(4, 0xa8);
#undef VLD
#undef VST
        c->r[7] = edi + 0xc0u;
    }
}

/* bool eCParticle_PS::UpdateVertices(void): once per frame (the frame number at [this+0x348]),
 * sorts the particles by depth, makes sure the vertex buffer exists, and writes four vertices per
 * live, visible particle into it: a quad oriented by [this+0x13c] (to the camera, along the
 * velocity, along a fixed axis), sized by the size scale, spun by the particle's three angles,
 * with normals by [this+0x234], texture coordinates from the frame atlas, and the colour and
 * colour factor. Then the vertex count goes to the primitive. Returns AL = 0 when there is no
 * buffer, render module or camera.
 *
 * Native as TickParticles is (see there): everything the original computes itself, its every
 * register write and store, and its 157 calls out as guest calls with the original's return sites,
 * registers, stack and x87 state. The calls with the original's own conventions keep them:
 * FUN_102995d0 takes EAX and ESI, the two particle tests take ECX and ESI, the spins call
 * operator*= through ESI. */
void g3n_103b3630(X86 *c) {
    if (!G3OPT(PARTICLES)) {
        fn_103b3630(c);
        return;
    }
    const uint32_t e0 = c->r[4];
    const uint32_t ebp0 = e0 - 4u;
    wr32(ebp0, c->r[5]);
    const uint32_t s0 = (ebp0 & 0xfffffff8u) - 0x428u;
    const uint32_t sp = s0 - 16u;
    wr32(s0 - 4u, c->r[3]);
    wr32(s0 - 8u, ebp0);
    wr32(s0 - 12u, c->r[6]);
    wr32(s0 - 16u, c->r[7]);
    c->r[4] = sp;
    const uint32_t self = c->r[1];
    c->r[5] = self;
    c->r[0] = rd32(rd32(ENGINE_WORLD) + 0x5e8u);
    if (rd32(self + 0x348u) == c->r[0])
        goto done;
    wr32(self + 0x348u, c->r[0]);
    call_fn(c, FN_103afa70, 0x103b3664u);
    c->r[1] = self;
    call_fn(c, FN_103b2d20, 0x103b366bu);
    if ((c->r[0] & 0xffu) == 0u)
        goto fail;
    call_fn(c, FN_10277b80, 0x103b367eu);
    c->r[3] = 0u;
    if (c->r[0] == 0u)
        goto fail;
    c->r[2] = sp + 0x4cu;
    c->r[1] = c->r[0] + 0x5b8u;
    push(c, c->r[2]);
    wr32(sp + 0x4cu, 0u);
    call_fn(c, FN_102ec7c0, 0x103b3698u);
    c->r[6] = rd32(sp + 0x4cu);
    if (c->r[6] == 0u)
        goto fail;
    /* 103b36a0: the entity's matrix, the camera's and its inverse */
    c->r[0] = rd32(self);
    c->r[2] = rd32(c->r[0] + 0x68u);
    c->r[1] = self;
    call_reg(c, c->r[2], &ic_vt_36a8, 0x103b36aau);
    c->r[7] = c->r[0] + 0x40u;
    c->r[6] += 0x14u;
    push(c, c->r[6]);
    c->r[1] = sp + 0x3f8u;
    wr32(sp + 0x50u, c->r[7]);
    call_iat(c, IAT_MATRIX_ASSIGN, &ic_matrix_assign, 0x103b36c2u);
    uv_call1(c, 0, sp + 0x3b8u, sp + 0x3f8u, IAT_MATRIX_INVERT_TO, &ic_matrix_invert_to, 0x103b36d7u);
    call_iat(c, IAT_GETZERO, &ic_getzero, 0x103b36ddu);
    push(c, c->r[0]);
    c->r[1] = sp + 0x1bcu;
    call_iat(c, IAT_SETVECTOR, &ic_setvector, 0x103b36ebu);
    if (rd32(self + 0x84u) == 0u) {
        c->r[7] += 0x30u;
        push(c, c->r[7]);
        c->r[1] = sp + 0x1bcu;
        call_iat(c, IAT_TRANSLATE, &ic_translate, 0x103b3704u);
    }
    /* 103b3704: lock the vertex buffer */
    c->r[1] = sp + 0xf8u;
    push(c, c->r[1]);
    c->r[1] = rd32(self + 0x39cu);
    push(c, 0x2000u);
    push(c, 0u);
    push(c, 0u);
    wr32(sp + 0xf8u, 0u);
    call_fn(c, FN_102f6cb0, 0x103b3725u);
    c->r[0] = rd32(sp + 0xf8u);
    c->r[7] = c->r[0];
    if (c->r[0] == 0u)
        goto fail;
    /* 103b3736: the atlas steps 1 / (columns + 1) and 1 / (rows + 1) */
    c->r[0] = rd32(self + 0x1d4u) + 1u;
    wr32(sp + 0x94u, c->r[0]);
    {
        const double a = (double)(int32_t)rd32(sp + 0x94u);
        c->r[0] = rd32(self + 0x1d8u) + 1u;
        wr32(sp + 0x90u, c->r[0]);
        const double r = fx87(c, fdivz(c, 1.0, a));
        c->r[1] = sp + 0x134u;
        wrf32(sp + 0x130u, fto_float(c, r));
        wrf32(sp + 0xa4u, fto_float(c, fx87(c, fdivz(c, 1.0, (double)(int32_t)rd32(sp + 0x90u)))));
        x87_used(c, 3);
    }
    /* 103b3781: the two colour temporaries, cleared */
    call_iat(c, IAT_FCOLOR_CTOR, &ic_fcolor_ctor, 0x103b3787u);
    c->r[2] = rd32(IAT_FCOLOR_VFT);
    c->r[4] = sp - 16u;
    wrf32(sp - 4u, fto_float(c, 0.0));
    wrf32(sp - 8u, fto_float(c, 0.0));
    c->r[1] = sp + 0x134u;
    wrf32(sp - 12u, fto_float(c, 0.0));
    wr32(sp + 0x134u, c->r[2]);
    wrf32(sp - 16u, fto_float(c, 0.0));
    x87_used(c, 1);
    call_iat(c, IAT_FCOLOR_SET, &ic_fcolor_set, 0x103b37b5u);
    c->r[1] = sp + 0x16cu;
    call_iat(c, IAT_FCOLOR_CTOR, &ic_fcolor_ctor, 0x103b37c2u);
    c->r[0] = rd32(IAT_FCOLOR_VFT);
    c->r[4] = sp - 16u;
    wrf32(sp - 4u, fto_float(c, 0.0));
    c->r[1] = sp + 0x16cu;
    wrf32(sp - 8u, fto_float(c, 0.0));
    wr32(sp + 0x16cu, c->r[0]);
    wrf32(sp - 12u, fto_float(c, 0.0));
    wrf32(sp - 16u, fto_float(c, 0.0));
    x87_used(c, 1);
    call_iat(c, IAT_FCOLOR_SET, &ic_fcolor_set, 0x103b37efu);
    /* 103b37ef: the particles */
    c->r[0] = rd32(self + 0x31cu);
    wr32(sp + 0x4cu, 0u);
    if ((int32_t)c->r[0] > 0) {
        wr32(sp + 0xa8u, 0u);
        c->r[7] += 0x14u;
        wr32(sp + 0xecu, c->r[0]);
        do {
            const uint32_t p = rd32(self + 0x318u) + rd32(sp + 0xa8u);
            c->r[3] = p;
            c->r[1] = p;
            call_fn(c, FN_103abc20, 0x103b3834u);
            if ((c->r[0] & 0xffu) == 0u) {
                c->r[6] = p;
                call_fn(c, FN_103abc50, 0x103b3843u);
                if ((c->r[0] & 0xffu) == 0u)
                    uv_particle(c, self, sp, p);
            }
            /* 103b4bf9 */
            wr32(sp + 0xa8u, rd32(sp + 0xa8u) + 0xb4u);
            wr32(sp + 0xecu, rd32(sp + 0xecu) - 1u);
        } while (rd32(sp + 0xecu) != 0u);
        c->r[3] = 0u;
    }
    /* 103b4c14: unlock, the primitive's vertex count = 2 * quads (0 if negative) */
    c->r[1] = rd32(self + 0x39cu);
    call_fn(c, FN_102f6cf0, 0x103b4c1fu);
    c->r[0] = rd32(sp + 0x4cu);
    c->r[5] = rd32(self + 0x398u);
    c->r[1] = rd32(c->r[5] + 0xcu);
    c->r[0] += c->r[0];
    c->r[2] = (((int32_t)c->r[0] < (int32_t)c->r[3]) ? 1u : 0u) - 1u;
    c->r[2] &= c->r[0];
    c->r[0] = rd32(c->r[1]);
    c->r[0] = rd32(c->r[0] + 0x30u);
    wr32(sp + 0xa4u, c->r[2]);
    call_reg(c, c->r[0], &ic_vt_4c46, 0x103b4c48u);
    c->r[1] = rd32(sp + 0xa4u);
    wr32(c->r[0] + 8u, c->r[1]);
done:
    c->r[0] = (c->r[0] & 0xffffff00u) | 1u;
    goto out;
fail:
    c->r[0] &= 0xffffff00u;
out:
    c->r[7] = rd32(sp);
    c->r[6] = rd32(sp + 4u);
    c->r[5] = rd32(sp + 8u);
    c->r[3] = rd32(sp + 12u);
    c->r[5] = rd32(ebp0);
    c->eip = rd32(e0);
    c->r[4] = e0 + 4u;
    recomp_return(c);
}
