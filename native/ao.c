/* native/ao.c - the ambient occlusion a SpeedTree's emitters cast on one receiving disk, natively (group ao; D-051,
 * docs/perf/ao-drzew.md).
 *
 * eCSpeedTreeRenderBuffer::CreateGemoetry (Engine 10463220) computes the tree's ambient occlusion every time the tree
 * loads, inside the frame: in each of the player's games of 29.10 about 2.0 s of one 3.2-3.4 s freeze. Per receiving
 * triangle disk, 1046a420 and 10359300 walk six emitter trees with eCAOTreeNode::ComputeSphereOcclusionFactor
 * (1035a0b0) or eCAOTreeNode::ComputeOcclusionFactor (10359e90): a node far enough away counts as one disk, a nearer
 * one is opened (eight children) or, as a leaf, gives its disks one by one. Each disk adds its occlusion to the
 * receiver's spherical harmonics through 10359c60 (1035a0b0's) or 10359b10 (10359e90's), which evaluate them in the
 * disk's direction (1035def0) and add the weighted disk (1035dbb0).
 *
 * g3n_1035a0b0 and g3n_10359e90 are the whole walk in C, the SharedBase vector calls it makes (SetVector, Transform,
 * TransformDirection, Normalize, InvTranslate, GetSquareMagnitude, Scale, GetDotProduct) and the CRT's sqrt
 * included, each x87 step as the translation computes it: fx87 for every FADD/FSUB/FMUL/FDIV, fdivz for the
 * divisions, fto_float for every FSTP to a float, fcomi for every FCOMI (IE on a NaN), MOVSS copies bit for bit. What
 * the caller sees is the original's: the harmonics (nine floats, written in the original's order), the disk counter
 * eCAmbientOcclusionPreprocessor::ms_iDiskCounter (10862db0), the x87 status word, EBX, EBP, ESI, EDI and ESP. The
 * original's callers (1046a420, 10359300, the walk itself) read none of EAX, ECX, EDX or the XMM registers after the
 * call; everything else the original writes is its own stack below the final ESP.
 *
 * Every other path of the CRT's sqrt (a negative, infinite or NaN argument, the CRT's fast flag, the control word
 * case native/leaf.c leaves to the original) gives the original the whole call: the harmonics, the counter and the
 * status word go back to what they were, and the original runs from the start, its own nested walks included.
 * native/tests/ao_test.c runs both from the same state. */
#include "chunk.h"
#include "g3_opt.h"

#include <math.h>
#include <string.h>

void fn_1035a0b0(X86 *c);
void fn_10359e90(X86 *c);

#define AO_DISK_COUNTER 0x10862db0u /* eCAmbientOcclusionPreprocessor::ms_iDiskCounter */
#define IAT_EPSILON 0x105f6864u     /* SharedBase g_fEpsilon */
#define IAT_PI 0x105f63fcu          /* SharedBase g_fPI */
#define ENG_ONE_F 0x1068612cu       /* float 1.0f, the cosine's upper bound */
#define ENG_K_NODE 0x106e86e8u      /* double: a node is one disk while radius * this <= its square distance */
#define ENG_K_SPHERE 0x106e8790u    /* double, 10359c60's distance term */
#define ENG_K_ORIENTED 0x106e85b8u  /* double, 10359b10's distance term; also the harmonics' z^2 factor */
#define ENG_SH0 0x106e85e0u         /* doubles: the harmonics' band factors */
#define ENG_SH1 0x106e85d8u
#define ENG_SH2 0x106e85d0u
#define ENG_SH3 0x106e85c8u
#define ENG_SH4 0x106e85c0u
#define ENG_SH_OFFSET 0x10686158u   /* double subtracted from the z^2 term */
#define ENG_SQRT_FASTFLAG 0x10861e38u /* Engine's CRT sqrt takes its other exit when this is set */
#define SB_SQRT_FASTFLAG 0x100d4118u  /* SharedBase's (Normalize's) */
#define SB_W_EPS 0x1008b4b0u          /* SharedBase SetTransformed: |w| below this gives the zero vector */
#define SB_ONE_F 0x10090a38u          /* SharedBase 1.0f, the matrix's [3][3] of the affine case */

typedef struct {
    X86 *c;
    uint32_t disk, sh, m; /* the receiving eSOrientedDisk, the bTSphericalHarmonics3<float>, the bCMatrix */
    int oriented;         /* 10359e90's walk (10359b10's disks), else 1035a0b0's (10359c60's) */
    int bail;             /* the CRT's sqrt would go another way: undone, the original runs */
    double k_node, k_disk, k_or, sh0, sh1, sh2, sh3, sh4, sh_off;
    uint32_t eps, pi;     /* where g_fEpsilon and g_fPI are */
} Ao;

static __thread int ao_original; /* the original runs, its nested walks with it */
unsigned g3n_ao_bails, g3n_ao_bails_late; /* calls given back to the original, those after a harmonics write (tests) */

static inline double ld(uint32_t a) {
    return (double)rdf32(a);
}
static inline float ff(X86 *c, double v) { /* FSTP float ptr */
    return fto_float(c, v);
}
static inline int ja(X86 *c, double st0, double st1) { /* FCOMI ST0,ST1 then JA */
    fcomi(c, st0, st1);
    return !c->eflags_cf && !c->eflags_zf;
}
/* The argument the CRT's sqrt takes its plain FSQRT for: sign clear, not infinite or NaN. */
static inline int sqrt_plain(float v) {
    uint32_t b;
    memcpy(&b, &v, 4);
    return !(b & 0x80000000u) && (b & 0x7f800000u) != 0x7f800000u;
}

/* ---- SharedBase's vector calls ---------------------------------------------------------------------------------- */

/* bCVector::Transform(bCMatrix const &) 10010440: SetTransformed (10032be0) into a temporary, MOVSS back. */
static void sb_transform(Ao *a, float v[3]) {
    X86 *c = a->c;
    const uint32_t m = a->m;
    const double x = v[0], y = v[1], z = v[2];
    float o0, o1, o2;
    if (rdf32(m + 0xcu) == 0.0f && rdf32(m + 0x1cu) == 0.0f && rdf32(m + 0x2cu) == 0.0f &&
        rdf32(m + 0x3cu) == rdf32(SB_ONE_F)) { /* 10032d46 */
        double t = fx87(c, fx87(c, ld(m + 0x10u) * y) + fx87(c, x * ld(m)));
        t = fx87(c, t + fx87(c, ld(m + 0x20u) * z));
        o0 = ff(c, fx87(c, t + ld(m + 0x30u)));
        t = fx87(c, fx87(c, ld(m + 0x4u) * x) + fx87(c, ld(m + 0x14u) * y));
        t = fx87(c, t + fx87(c, ld(m + 0x24u) * z));
        o1 = ff(c, fx87(c, t + ld(m + 0x34u)));
        t = fx87(c, fx87(c, ld(m + 0x8u) * x) + fx87(c, ld(m + 0x18u) * y));
        t = fx87(c, t + fx87(c, ld(m + 0x28u) * z));
        o2 = ff(c, fx87(c, t + ld(m + 0x38u)));
    } else {
        const double sx = ff(c, x); /* FLD [ESI], FSTP to the frame, FLD it back */
        double w = fx87(c, fx87(c, ld(m + 0xcu) * sx) + fx87(c, ld(m + 0x1cu) * y));
        w = fx87(c, w + fx87(c, ld(m + 0x2cu) * z));
        const float wf = ff(c, fx87(c, w + ld(m + 0x3cu)));
        const float aw = ff(c, fabs((double)wf));
        if (ja(c, ld(SB_W_EPS), (double)aw)) {
            o0 = o1 = o2 = 0.0f;
        } else {
            double t = fx87(c, fx87(c, ld(m + 0x10u) * y) + fx87(c, sx * ld(m)));
            t = fx87(c, t + fx87(c, ld(m + 0x20u) * z));
            o0 = ff(c, fx87(c, t + ld(m + 0x30u)));
            t = fx87(c, fx87(c, ld(m + 0x4u) * x) + fx87(c, ld(m + 0x14u) * y));
            t = fx87(c, t + fx87(c, ld(m + 0x24u) * z));
            o1 = ff(c, fx87(c, t + ld(m + 0x34u)));
            t = fx87(c, fx87(c, ld(m + 0x8u) * x) + fx87(c, ld(m + 0x18u) * y));
            t = fx87(c, t + fx87(c, ld(m + 0x28u) * z));
            o2 = ff(c, fx87(c, t + ld(m + 0x38u)));
            const float inv = ff(c, fx87(c, fdivz(c, 1.0, (double)wf)));
            o0 = ff(c, fx87(c, (double)o0 * (double)inv));
            o1 = ff(c, fx87(c, (double)o1 * (double)inv));
            o2 = ff(c, fx87(c, (double)inv * (double)o2));
        }
    }
    v[0] = o0;
    v[1] = o1;
    v[2] = o2;
}

/* bCVector::TransformDirection(bCMatrix const &) 10010480: 10032da0 into a temporary, MOVSS back. */
static void sb_direction(Ao *a, float v[3]) {
    X86 *c = a->c;
    const uint32_t m = a->m;
    const double x = v[0], y = v[1], z = v[2];
    double t = fx87(c, fx87(c, ld(m + 0x10u) * y) + fx87(c, x * ld(m)));
    const float o0 = ff(c, fx87(c, t + fx87(c, ld(m + 0x20u) * z)));
    t = fx87(c, fx87(c, ld(m + 0x4u) * x) + fx87(c, ld(m + 0x14u) * y));
    const float o1 = ff(c, fx87(c, t + fx87(c, ld(m + 0x24u) * z)));
    t = fx87(c, fx87(c, ld(m + 0x8u) * x) + fx87(c, ld(m + 0x18u) * y));
    const float o2 = ff(c, fx87(c, t + fx87(c, ld(m + 0x28u) * z)));
    v[0] = o0;
    v[1] = o1;
    v[2] = o2;
}

/* bCVector::Normalize() 10010060, its sqrt SharedBase's CRT (10085580). */
static void sb_normalize(Ao *a, float v[3]) {
    X86 *c = a->c;
    const float fy = ff(c, (double)v[1]), fx = ff(c, (double)v[0]), fz = ff(c, (double)v[2]);
    const double xx = fx87(c, (double)fx * (double)fx);
    const double yy = fx87(c, (double)fy * (double)fy);
    const double xy = fx87(c, yy + xx);
    const double zz = fx87(c, (double)fz * (double)fz);
    const float s = ff(c, fx87(c, xy + zz));
    if (!sqrt_plain(s)) {
        a->bail = 1;
        return;
    }
    const float len = ff(c, fx87(c, sqrt((double)s)));
    if (ja(c, (double)len, 0.0)) {
        const float inv = ff(c, fx87(c, fdivz(c, 1.0, (double)len)));
        v[0] = ff(c, fx87(c, (double)fx * (double)inv));
        v[1] = ff(c, fx87(c, (double)fy * (double)inv));
        v[2] = ff(c, fx87(c, (double)inv * (double)fz));
    }
}

/* SetVector 100138d0 (FLD/FSTP) from guest memory. */
static inline void sb_set(X86 *c, float v[3], uint32_t from) {
    v[0] = ff(c, ld(from));
    v[1] = ff(c, ld(from + 4u));
    v[2] = ff(c, ld(from + 8u));
}

/* InvTranslate 1001ae70 by the receiving disk's position, then GetSquareMagnitude 10010e20. */
static float sb_distance2(Ao *a, float v[3]) {
    X86 *c = a->c;
    for (int i = 0; i < 3; ++i)
        v[i] = ff(c, fx87(c, (double)v[i] - ld(a->disk + 4u * (uint32_t)i)));
    const double xx = fx87(c, (double)v[0] * (double)v[0]);
    const double yy = fx87(c, (double)v[1] * (double)v[1]);
    return ff(c, fx87(c, fx87(c, yy + xx) + fx87(c, (double)v[2] * (double)v[2])));
}

/* GetDotProduct 1001b580: ECX = n (guest memory or a local's floats), the argument v. */
static inline float sb_dot(X86 *c, double nx, double ny, double nz, const float v[3]) {
    const double t = fx87(c, fx87(c, (double)v[1] * ny) + fx87(c, (double)v[0] * nx));
    return ff(c, fx87(c, t + fx87(c, (double)v[2] * nz)));
}

/* ---- the harmonics ---------------------------------------------------------------------------------------------- */

/* 1035def0: the harmonics in direction v. */
static float sh_eval(Ao *a, const float v[3]) {
    X86 *c = a->c;
    const uint32_t s = a->sh;
    const double y = ff(c, (double)v[1]), x = ff(c, (double)v[0]), z = ff(c, (double)v[2]);
    double r = fx87(c, fx87(c, ld(s + 4u) * x) + ld(s));
    r = fx87(c, r + fx87(c, ld(s + 8u) * y));
    r = fx87(c, fx87(c, ld(s + 0xcu) * z) + r);
    r = fx87(c, r + fx87(c, fx87(c, z * x) * ld(s + 0x10u)));
    r = fx87(c, r + fx87(c, fx87(c, z * y) * ld(s + 0x14u)));
    const double xy6 = fx87(c, fx87(c, y * x) * ld(s + 0x18u));
    r = fx87(c, xy6 + r);
    const double zk = fx87(c, z * a->k_or);
    const double g = fx87(c, fx87(c, z * zk) - a->sh_off);
    r = fx87(c, r + fx87(c, g * ld(s + 0x1cu)));
    const double xx = fx87(c, x * x), yy = fx87(c, y * y);
    const double d = fx87(c, xx - yy);
    return ff(c, fx87(c, r + fx87(c, d * ld(s + 0x20u))));
}

/* 1035dbb0: the disk in direction v with weight w added to the harmonics, in the original's order. */
static void sh_add(Ao *a, const float v[3], float wf) {
    X86 *c = a->c;
    const uint32_t s = a->sh;
    const double w = ff(c, (double)wf);
    const double vx = v[0], vy = v[1], vz = v[2];
    wrf32(s, ff(c, fx87(c, fx87(c, a->sh0 * w) + ld(s))));
    const double k1 = fx87(c, a->sh1 * w);
    wrf32(s + 4u, ff(c, fx87(c, fx87(c, vx * k1) + ld(s + 4u))));
    wrf32(s + 8u, ff(c, fx87(c, fx87(c, vy * k1) + ld(s + 8u))));
    wrf32(s + 0xcu, ff(c, fx87(c, fx87(c, k1 * vz) + ld(s + 0xcu))));
    const double k2 = fx87(c, a->sh2 * w);
    wrf32(s + 0x10u, ff(c, fx87(c, fx87(c, fx87(c, vx * vz) * k2) + ld(s + 0x10u))));
    wrf32(s + 0x14u, ff(c, fx87(c, fx87(c, fx87(c, vz * vy) * k2) + ld(s + 0x14u))));
    wrf32(s + 0x18u, ff(c, fx87(c, fx87(c, k2 * fx87(c, vx * vy)) + ld(s + 0x18u))));
    const double z = ff(c, vz);
    const double g = fx87(c, fx87(c, z * fx87(c, a->k_or * z)) - a->sh_off);
    wrf32(s + 0x1cu, ff(c, fx87(c, fx87(c, g * fx87(c, w * a->sh3)) + ld(s + 0x1cu))));
    const double d = fx87(c, fx87(c, vx * vx) - fx87(c, vy * vy));
    wrf32(s + 0x20u, ff(c, fx87(c, fx87(c, d * fx87(c, w * a->sh4)) + ld(s + 0x20u))));
}

/* The disks' common tail: 1 - the harmonics in direction v, held to [0, 1], times q, added with that weight. */
static void sh_occlude(Ao *a, const float v[3], float q) {
    X86 *c = a->c;
    const float e = sh_eval(a, v);
    const double t = fx87(c, 1.0 - (double)e);
    double f;
    if (ja(c, t, 1.0)) { /* 10359d42 */
        ja(c, t, 1.0);
        f = 1.0;
    } else if (ja(c, 0.0, t)) { /* 10359d4c not taken */
        f = 0.0;
    } else {
        ja(c, t, 1.0);
        f = t;
    }
    sh_add(a, v, ff(c, fx87(c, f * (double)q)));
}

/* ---- the disks -------------------------------------------------------------------------------------------------- */

/* 10359c60 (sphere) and 10359b10 (oriented): emitter e {position, normal, radius, area}, v the vector from the
 * receiver to it, d2 its square length. Returns after the counter and the harmonics are written. */
static void disk(Ao *a, const float e[8], float v[3], float d2) {
    X86 *c = a->c;
    wr32(AO_DISK_COUNTER, rd32(AO_DISK_COUNTER) + 1u);
    const float ad = ff(c, fabs((double)d2));
    if (ja(c, ld(a->eps), (double)ad))
        return;
    if (!sqrt_plain(d2)) {
        a->bail = 1;
        return;
    }
    const float len = ff(c, fx87(c, sqrt((double)d2)));
    const float inv = ff(c, fx87(c, fdivz(c, 1.0, (double)len)));
    for (int i = 0; i < 3; ++i) /* Scale(float) 100103b0 */
        v[i] = ff(c, fx87(c, (double)v[i] * (double)inv));
    const uint32_t n = a->disk + 0xcu;
    float cosr = sb_dot(c, ld(n), ld(n + 4u), ld(n + 8u), v);
    if (ja(c, 0.0, (double)cosr))
        return;
    const float one = rdf32(ENG_ONE_F);
    if (cosr > one)
        cosr = one;
    if (!a->oriented) {
        const double pi = ff(c, ld(a->pi));
        const float area = ff(c, fx87(c, (double)e[6] * pi));
        const double num = fx87(c, (double)area * (double)cosr);
        const double far = fx87(c, pi * fx87(c, (double)d2 * a->k_disk));
        const float q = ff(c, fx87(c, fdivz(c, num, fx87(c, (double)area + far))));
        sh_occlude(a, v, q);
        return;
    }
    const float cose = sb_dot(c, (double)e[3], (double)e[4], (double)e[5], v);
    const double pi = ff(c, ld(a->pi));
    const float area = ff(c, fx87(c, (double)e[6] * pi));
    const double acose = ff(c, fabs((double)cose));
    const double num = fx87(c, fx87(c, acose * (double)area) * (double)cosr);
    const double far = fx87(c, fx87(c, pi * (double)d2) * a->k_or);
    const float q = ff(c, fx87(c, fdivz(c, num, fx87(c, (double)area + far))));
    sh_occlude(a, v, q);
}

/* ---- the walk --------------------------------------------------------------------------------------------------- */

static void walk(Ao *a, uint32_t node) {
    X86 *c = a->c;
    if (a->bail || !(rd8(node + 0x68u) & 8u))
        return;
    float e[8]; /* the node as one disk: position, normal, radius, area (local_40) */
    sb_set(c, e, node);
    sb_set(c, e + 3, node + 0xcu);
    e[6] = rdf32(node + 0x18u);
    e[7] = rdf32(node + 0x1cu);
    sb_transform(a, e);
    sb_direction(a, e + 3);
    sb_normalize(a, e + 3);
    if (a->bail)
        return;
    float v[3] = {e[0], e[1], e[2]};
    const float d2 = sb_distance2(a, v);
    const float r = rdf32(node + 0x18u);
    if (!(0.0f >= r)) { /* COMISS 0, r; JNC: taken only for 0 >= r */
        if (!ja(c, fx87(c, (double)r * a->k_node), (double)d2)) {
            disk(a, e, v, d2);
            return;
        }
    }
    if (!((rd32(node + 0x68u) >> 1) & 1u)) {
        for (uint32_t k = 0x54u; k >= 0x38u; k -= 4u) {
            const uint32_t child = rd32(node + k);
            if (child)
                walk(a, child);
            if (a->bail)
                return;
        }
        return;
    }
    const int32_t n = (int32_t)rd32(node + 0x60u);
    for (int32_t i = 0; i < n && !a->bail; ++i) {
        const uint32_t rec = (rd32(rd32(node + 0x5cu) + 4u * (uint32_t)i) << 5) + rd32(rd32(node + 0x58u));
        float d[8]; /* local_20 */
        sb_set(c, d, rec);
        for (int k = 3; k < 8; ++k)
            d[k] = rdf32(rec + 4u * (uint32_t)k);
        sb_transform(a, d);
        sb_direction(a, d + 3);
        sb_normalize(a, d + 3);
        if (a->bail)
            return;
        float w[3]; /* SetVector(local_4c, local_20): FLD/FSTP */
        w[0] = ff(c, (double)d[0]);
        w[1] = ff(c, (double)d[1]);
        w[2] = ff(c, (double)d[2]);
        const float dd = sb_distance2(a, w);
        disk(a, d, w, dd);
    }
}

static void run(X86 *c, int oriented, void (*orig)(X86 *)) {
    const uint16_t cw = c->fpu_cw, sw = c->fpu_sw;
    if (!G3OPT(AO) || ao_original || rd32(ENG_SQRT_FASTFLAG) || rd32(SB_SQRT_FASTFLAG) ||
        (cw != 0x027fu && (cw & 0x20u) && (sw & 0x20u))) {
        orig(c);
        return;
    }
    Ao a = {c, rd32(c->r[4] + 4u), rd32(c->r[4] + 8u), rd32(c->r[4] + 0xcu), oriented, 0,
            rdf64(ENG_K_NODE), rdf64(ENG_K_SPHERE), rdf64(ENG_K_ORIENTED), rdf64(ENG_SH0), rdf64(ENG_SH1),
            rdf64(ENG_SH2), rdf64(ENG_SH3), rdf64(ENG_SH4), rdf64(ENG_SH_OFFSET), rd32(IAT_EPSILON), rd32(IAT_PI)};
    uint8_t sh[36];
    memcpy(sh, g_mem + a.sh, sizeof sh);
    const uint32_t counter = rd32(AO_DISK_COUNTER);
    walk(&a, c->r[1]);
    if (a.bail) {
        ++g3n_ao_bails;
        if (memcmp(g_mem + a.sh, sh, sizeof sh))
            ++g3n_ao_bails_late;
        memcpy(g_mem + a.sh, sh, sizeof sh);
        wr32(AO_DISK_COUNTER, counter);
        c->fpu_sw = sw;
        ao_original = 1;
        orig(c);
        ao_original = 0;
        return;
    }
    /* the slot the original's sqrt, Normalize and GetSquareMagnitude calls pass ST0 through */
    for (unsigned i = 1; i <= 2; ++i)
        ftag_put(c, (c->fpu_top - i) & 7u, FTAG_EMPTY);
    c->eip = rd32(c->r[4]);
    c->r[4] += 0x10u;
    recomp_return(c);
}

/* eCAOTreeNode::ComputeSphereOcclusionFactor(eSOrientedDisk &, bTSphericalHarmonics3<float> &, bCMatrix const &) */
void g3n_1035a0b0(X86 *c) {
    run(c, 0, fn_1035a0b0);
}
/* eCAOTreeNode::ComputeOcclusionFactor, the same arguments */
void g3n_10359e90(X86 *c) {
    run(c, 1, fn_10359e90);
}
