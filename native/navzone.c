/* native/navzone.c - gCNavZone_PS::Test3DPointInZone (Game.dll 10a27180) as one native function, group "navzone"
 * (G3OPT_NAVZONE; docs/perf/jaskinia.md).
 *
 * Every NPC's navigation asks each frame whether it is still in its zone (gCNavigation_PS::Process 10a30720 ->
 * IsInZone 10a3e540 -> gCNavigationMap::GetZone 10a3c620 -> this): a winding angle around the zone's points, each
 * step four guest calls into SharedBase and an acos through its CRT. In the cave near Reddock without a window
 * (session 28) it was 7.6% of the game thread.
 *
 * bool Test3DPointInZone(bCVector const &p, bool fallback), thiscall, RET 8. The point goes into the zone's space
 * (bCMatrix::operator* 10019ed0 with the matrix at this+0x104 -> SetTransformed 10032be0), q. With the zone's points
 * P[0..n-1] (this+0x14, n at this+0x18, 12 bytes each) and y set to 0: v = P[n-1] - q, w = P[0] - q
 * (GetInvTranslated 10011bd0), their lengths in x and z (GetMagnitude2D 10010c90 -> 10010e50 and the CRT's sqrt
 * 10085580); either 0 returns `fallback`. Both scaled by 1/length (Scale 100103b0), the angle between them
 * (GetAngleUnitRad 10011570: the dot product clamped to [-1, 1], the CRT's acos 100859b0), over pi - 0.001 returns
 * `fallback`. Then for each angle: the cross product (SetCrossProduct 10010570), and when its square
 * (GetSquareMagnitude 10010e20) is not 0 the angle is wrapped into [-pi, pi], made positive, and negated when the
 * cross product's y is above 0. For i = 1..n-1: v = w, w = P[i] - q with y = 0, its length (GetMagnitude 10010c50),
 * 0 or an angle over the limit makes the sum 2 pi and ends the loop, else the angle is added. True when |sum| > pi.
 *
 * The x87 steps are the original's through the kit's helpers (fx87 per FADD/FSUB/FMUL/FDIV/FSQRT under the precision
 * control, fto_float per FSTP to a float, fdivz, atan2 exact for FPATAN), values kept in registers as doubles. The
 * CRT's acos takes its x87 path here: FSQRT((1 + x)(1 - x)) and FPATAN under (cw & 0x300) | 0x7f, the same precision;
 * +1 gives 0, -1 FLDPI. The original writes nothing but its own frame, so this computes everything first and runs
 * the original instead when a value is a NaN or infinite (the x87 exception flags and the CRT's error paths), when
 * the rounding control is not to nearest, when either CRT function would take another exit (SharedBase's fast flag
 * 100d4118, the SSE2 acos at 102442c0 with every exception masked, a pending precision exception the CRT's exit
 * reports), or for a zone without points.
 *
 * Not reproduced: EAX above AL (the callers test AL), ECX, EDX and XMM0-1, which the original leaves as its last
 * scratch values and no caller reads. native/tests/navzone_test.c compares the rest with the translation. */
#include "chunk.h"
#include "g3_opt.h"

#include <math.h>

void fn_10a27180(X86 *c);

#define SB_ONE_F 0x10090a38u       /* 1.0f: SetTransformed's w test, GetAngleUnitRad's upper clamp */
#define SB_MINUS_ONE_D 0x10090af0u /* -1.0: GetAngleUnitRad's lower test */
#define SB_MINUS_ONE_F 0x10090aa0u /* -1.0f: its lower clamp */
#define SB_PI_F 0x1008b490u        /* g_fPI */
#define SB_TWO_PI_F 0x1008b494u    /* g_f2PI */
#define SB_W_EPS 0x1008b4b0u       /* SetTransformed: |w| under this gives the zero vector */
#define SB_SQRT_FASTFLAG 0x100d4118u
#define SB_ACOS_SSE2 0x102442c0u   /* the CRT's SSE2 maths switch */
#define GAME_EPS_D 0x10c181e0u     /* 0.001: the angle limit is pi - this */
#define GAME_FLIP_D 0x10c181a0u    /* -1.0: the angle's sign when the cross product's y is above 0 */

unsigned g3n_navzone_native, g3n_navzone_original; /* native/tests/navzone_test.c */

typedef struct {
    X86 *c;
    int bad; /* a NaN or an infinity: the original runs instead */
} Nz;

static inline double nz_a(Nz *n, double r) { /* an arithmetic result */
    const double v = fx87(n->c, r);
    if (!isfinite(v))
        n->bad = 1;
    return v;
}
static inline float nz_f(Nz *n, double r) { /* FSTP float ptr */
    const float v = fto_float(n->c, r);
    if (!isfinite(v))
        n->bad = 1;
    return v;
}
static inline double nz_ld(Nz *n, uint32_t at) { /* FLD float ptr */
    const float v = rdf32(at);
    if (!isfinite(v))
        n->bad = 1;
    return (double)v;
}

/* bCMatrix::operator*(bCVector const &) 10019ed0: the vector copied out, SetTransformed 10032be0 into a
 * temporary, copied back. */
static void nz_transform(Nz *n, uint32_t m, uint32_t p, float o[3]) {
    X86 *c = n->c;
    const double x = nz_ld(n, p), y = nz_ld(n, p + 4u), z = nz_ld(n, p + 8u);
    if (rdf32(m + 0xcu) == 0.0f && rdf32(m + 0x1cu) == 0.0f && rdf32(m + 0x2cu) == 0.0f &&
        rdf32(m + 0x3cu) == rdf32(SB_ONE_F)) { /* 10032d46 */
        double t = nz_a(n, nz_a(n, nz_ld(n, m + 0x10u) * y) + nz_a(n, x * nz_ld(n, m)));
        t = nz_a(n, t + nz_a(n, nz_ld(n, m + 0x20u) * z));
        o[0] = nz_f(n, nz_a(n, t + nz_ld(n, m + 0x30u)));
        t = nz_a(n, nz_a(n, nz_ld(n, m + 0x4u) * x) + nz_a(n, nz_ld(n, m + 0x14u) * y));
        t = nz_a(n, t + nz_a(n, nz_ld(n, m + 0x24u) * z));
        o[1] = nz_f(n, nz_a(n, t + nz_ld(n, m + 0x34u)));
        t = nz_a(n, nz_a(n, nz_ld(n, m + 0x8u) * x) + nz_a(n, nz_ld(n, m + 0x18u) * y));
        t = nz_a(n, t + nz_a(n, nz_ld(n, m + 0x28u) * z));
        o[2] = nz_f(n, nz_a(n, t + nz_ld(n, m + 0x38u)));
        return;
    }
    /* 10032c66 */
    const double sx = nz_f(n, x);
    double w = nz_a(n, nz_a(n, nz_ld(n, m + 0xcu) * sx) + nz_a(n, nz_ld(n, m + 0x1cu) * y));
    w = nz_a(n, w + nz_a(n, nz_ld(n, m + 0x2cu) * z));
    const float wf = nz_f(n, nz_a(n, w + nz_ld(n, m + 0x3cu)));
    const float aw = nz_f(n, fabs((double)wf));
    if (nz_ld(n, SB_W_EPS) > (double)aw) {
        o[0] = o[1] = o[2] = 0.0f;
        return;
    }
    double t = nz_a(n, nz_a(n, nz_ld(n, m + 0x10u) * y) + nz_a(n, sx * nz_ld(n, m)));
    t = nz_a(n, t + nz_a(n, nz_ld(n, m + 0x20u) * z));
    o[0] = nz_f(n, nz_a(n, t + nz_ld(n, m + 0x30u)));
    t = nz_a(n, nz_a(n, nz_ld(n, m + 0x4u) * x) + nz_a(n, nz_ld(n, m + 0x14u) * y));
    t = nz_a(n, t + nz_a(n, nz_ld(n, m + 0x24u) * z));
    o[1] = nz_f(n, nz_a(n, t + nz_ld(n, m + 0x34u)));
    t = nz_a(n, nz_a(n, nz_ld(n, m + 0x8u) * x) + nz_a(n, nz_ld(n, m + 0x18u) * y));
    t = nz_a(n, t + nz_a(n, nz_ld(n, m + 0x28u) * z));
    o[2] = nz_f(n, nz_a(n, t + nz_ld(n, m + 0x38u)));
    const float inv = nz_f(n, nz_a(n, fdivz(c, 1.0, (double)wf)));
    o[0] = nz_f(n, nz_a(n, (double)o[0] * (double)inv));
    o[1] = nz_f(n, nz_a(n, (double)o[1] * (double)inv));
    o[2] = nz_f(n, nz_a(n, (double)inv * (double)o[2]));
}

/* GetInvTranslated 10011bd0: o = P - q, component by component. */
static void nz_sub(Nz *n, uint32_t p, const float q[3], float o[3]) {
    for (int i = 0; i < 3; ++i)
        o[i] = nz_f(n, nz_a(n, nz_ld(n, p + 4u * (uint32_t)i) - (double)q[i]));
}

/* The CRT's sqrt 10085580 of a float slot, then FSTP float. */
static float nz_sqrt(Nz *n, float s) {
    if (signbit(s)) /* its error path */
        n->bad = 1;
    return nz_f(n, nz_a(n, sqrt((double)s)));
}

/* GetMagnitude2D(1) 10010c90: x and z. */
static float nz_mag2d(Nz *n, const float v[3]) {
    const double xx = nz_a(n, (double)v[0] * v[0]);
    const double zz = nz_a(n, (double)v[2] * v[2]);
    return nz_sqrt(n, nz_f(n, nz_a(n, zz + xx)));
}

/* The sum of squares of GetMagnitude 10010c50 and GetSquareMagnitude 10010e20: (y y + x x) + z z. */
static float nz_square(Nz *n, const float v[3]) {
    const double xx = nz_a(n, (double)v[0] * v[0]);
    const double yy = nz_a(n, (double)v[1] * v[1]);
    const double s = nz_a(n, yy + xx);
    return nz_f(n, nz_a(n, s + nz_a(n, (double)v[2] * v[2])));
}

/* Scale(float) 100103b0. */
static void nz_scale(Nz *n, float v[3], float s) {
    v[0] = nz_f(n, nz_a(n, (double)v[0] * s));
    v[1] = nz_f(n, nz_a(n, (double)v[1] * s));
    v[2] = nz_f(n, nz_a(n, (double)s * v[2]));
}

/* The CRT's acos 100859b0 on its x87 path (10085a08), x in [-1, 1]. */
static double nz_acos(Nz *n, double x) {
    if (x == 1.0)
        return 0.0;
    if (x == -1.0)
        return 3.14159265358979323846; /* FLDPI */
    const double p = nz_a(n, 1.0 + x), q = nz_a(n, 1.0 - x);
    const double s = nz_a(n, sqrt(nz_a(n, q * p)));
    return fx87_exact(n->c, atan2(s, x));
}

/* GetAngleUnitRad 10011570 (this = v, the argument w), then the caller's FSTP float. */
static float nz_angle(Nz *n, const float v[3], const float w[3]) {
    double t = nz_a(n, (double)w[1] * v[1]);
    t = nz_a(n, t + nz_a(n, (double)w[0] * v[0]));
    t = nz_a(n, t + nz_a(n, (double)w[2] * v[2]));
    const float d = nz_f(n, t);
    const float one = rdf32(SB_ONE_F);
    const float upper = (double)d > 1.0 ? one : d;
    float x;
    if (rdf64(SB_MINUS_ONE_D) > (double)upper)
        x = rdf32(SB_MINUS_ONE_F);
    else
        x = (double)d > 1.0 ? one : d;
    return nz_f(n, nz_acos(n, (double)x));
}

/* The angle's sign and wrap from the cross product (10a272a1..10a2732e, the same in the loop). */
static float nz_orient(Nz *n, const float v[3], const float w[3], float angle) {
    float x[3];
    x[0] = nz_f(n, nz_a(n, nz_a(n, (double)v[1] * w[2]) - nz_a(n, (double)v[2] * w[1])));
    x[1] = nz_f(n, nz_a(n, nz_a(n, (double)v[2] * w[0]) - nz_a(n, (double)v[0] * w[2])));
    x[2] = nz_f(n, nz_a(n, nz_a(n, (double)v[0] * w[1]) - nz_a(n, (double)w[0] * v[1])));
    if (nz_square(n, x) == 0.0f)
        return angle;
    const double pi = nz_ld(n, SB_PI_F), two_pi = nz_ld(n, SB_TWO_PI_F);
    double a = angle;
    if (a > pi)
        a = nz_f(n, nz_a(n, a - two_pi));
    if (-pi > a)
        a = nz_f(n, nz_a(n, a + two_pi));
    float r = nz_f(n, fabs(a));
    if ((double)x[1] > 0.0)
        r = nz_f(n, nz_a(n, (double)r * rdf64(GAME_FLIP_D)));
    return r;
}

/* bool gCNavZone_PS::Test3DPointInZone(bCVector const &, bool) */
void g3n_10a27180(X86 *c) {
    const uint16_t cw = c->fpu_cw, sw = c->fpu_sw;
    const uint32_t self = c->r[1], esp = c->r[4];
    const int32_t count = (int32_t)rd32(self + 0x18u);
    if (!G3OPT(NAVZONE) || (cw & 0xc00u) || ((cw >> 8) & 3u) == 1u || count < 1 || rd32(SB_SQRT_FASTFLAG) ||
        (rd32(SB_ACOS_SSE2) && (cw & 0x7fu) == 0x7fu) || (cw != 0x027fu && (cw & 0x20u) && (sw & 0x20u))) {
        ++g3n_navzone_original;
        fn_10a27180(c);
        return;
    }
    const uint32_t points = rd32(self + 0x14u);
    const uint8_t fallback = (uint8_t)rd32(esp + 8u);
    Nz n = {c, 0};
    uint8_t result;
    float q[3], v[3], w[3];
    nz_transform(&n, self + 0x104u, rd32(esp + 4u), q);
    nz_sub(&n, points + 12u * (uint32_t)(count - 1), q, v);
    nz_sub(&n, points, q, w);
    v[1] = 0.0f;
    w[1] = 0.0f;
    const float len_v = nz_mag2d(&n, v), len_w = nz_mag2d(&n, w);
    const double limit = nz_a(&n, nz_ld(&n, SB_PI_F) - rdf64(GAME_EPS_D));
    if (len_v == 0.0f || len_w == 0.0f) {
        result = fallback;
    } else {
        nz_scale(&n, v, nz_f(&n, nz_a(&n, fdivz(c, 1.0, (double)len_v))));
        nz_scale(&n, w, nz_f(&n, nz_a(&n, fdivz(c, 1.0, (double)len_w))));
        float angle = nz_angle(&n, v, w);
        if ((double)nz_f(&n, fabs((double)angle)) > limit) {
            result = fallback;
        } else {
            float sum = nz_orient(&n, v, w, angle);
            for (int32_t i = 1; i < count && !n.bad; ++i) {
                v[0] = w[0];
                v[1] = w[1];
                v[2] = w[2];
                float t[3];
                nz_sub(&n, points + 12u * (uint32_t)i, q, t);
                w[0] = t[0];
                w[1] = 0.0f;
                w[2] = t[2];
                const float len = nz_sqrt(&n, nz_square(&n, w));
                if (len == 0.0f) {
                    sum = rdf32(SB_TWO_PI_F);
                    break;
                }
                nz_scale(&n, w, nz_f(&n, nz_a(&n, fdivz(c, 1.0, (double)len))));
                angle = nz_angle(&n, v, w);
                if ((double)nz_f(&n, fabs((double)angle)) > limit) {
                    sum = rdf32(SB_TWO_PI_F);
                    break;
                }
                angle = nz_orient(&n, v, w, angle);
                sum = nz_f(&n, nz_a(&n, (double)angle + sum));
            }
            result = (double)nz_f(&n, fabs((double)sum)) > nz_ld(&n, SB_PI_F);
        }
    }
    if (n.bad) {
        c->fpu_sw = sw;
        ++g3n_navzone_original;
        fn_10a27180(c);
        return;
    }
    ++g3n_navzone_native;
    c->r[0] = (c->r[0] & ~0xffu) | result;
    c->eip = rd32(esp);
    c->r[4] = esp + 12u;
    recomp_return(c);
}
