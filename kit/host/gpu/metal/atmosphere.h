// atmosphere.h - Remaster run 14 B1 height fog (review/run14/smoke-realism-design.md 8 (B) B1): the pure parts, no Metal
// types. Included by d3d9_metal.mm (the pass) and dx/tests/d3d9_tests.cpp (test_atmosphere).
//
// One full-screen pass at the soft-particle pass break (before the first particle draw; the opaque scene and its
// depth copy are ready). Per pixel: the stored depth -> the world point (the frame's WORLD view-projection, inverted,
// camera-relative) -> the optical depth of two exponential height layers along the view ray, integrated analytically
// (a thin haze layer for aerial perspective and a dense low fog layer; density rho for h <= base, rho exp(-(h - base) / H)
// above, so a fog bank has a level floor and hills rise out of it) -> T = exp(-tau) -> col = col T + L_in (1 - T).
// L_in is the game's own Hoffman-Preetham colour: (Br Phi_R + Bm Phi_M) / (Br + Bm) per channel (Br, Bm from the live
// time-of-day object [0x009b392c] +0x70..+0x94, Henyey-Greenstein g +0x9c), both phases normalised to 4 pi, times the
// sun colour, plus an ambient term; the Mie lobe gives the forward-scatter glow toward the sun (capped).
// tau = 0 (no density) gives T = 1 and exactly the scene; the host also skips the pass when atm_active() is false.
//
// The maths is written once (ATM_MATHS) and used twice: compiled here as C++ (the twin the unit test checks) and
// stringified into the Metal source (kAtmSource), so the shader cannot drift from the test. Scalar code only, /* */
// comments only (it is one macro).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

#include "foundation.h"   // the WORLD view-projection clusters and the camera pick (run 14 F1)

namespace remaster_atm {

// The uniform block (Metal buffer(0)); float arrays have the same layout in C++ and MSL.
struct AtmU {
    float m[16];     // camera-relative inverse view-projection, row-vector: (nx, ny, d, 1) x M = (p - eye) w, w
    float eye[4];    // camera position (world; z up, metres); w unused
    float sun[4];    // unit direction toward the sun; w = HG g
    float br[4];     // the game's Br (Rayleigh); w = sun intensity
    float bm[4];     // the game's Bm (Mie); w = Mie phase cap (the glow's peak, 4 pi normalised)
    float suncol[4]; // sun colour; w unused
    float amb[4];    // ambient in-scatter colour; w = debug view (1: the fog factor 1 - T as grey)
    float l0[4];     // haze layer: density (1/m), base height (m), scale height (m), unused
    float l1[4];     // fog layer: same
    float misc[4];   // start distance (m), far distance (m; also the sky's), max opacity (0..1), strength (x density)
};
struct AtmOut {
    float t, r, g, b;   // transmittance and the in-scatter colour L_in
};

#define ATM_MATHS                                                                                                       \
    /* integral of exp(-h / H) along a straight segment of length L from height ha to hb (both >= 0, above base) */     \
    static inline float atm_seg(float ha, float hb, float L, float H) {                                                \
        float d = hb - ha;                                                                                              \
        if (fabs(d) < 1.0e-3f * H)                                                                                      \
            return L * exp(-0.5f * (ha + hb) / H);                                                                      \
        return L * H * (exp(-ha / H) - exp(-hb / H)) / d;                                                               \
    }                                                                                                                   \
    /* optical depth of one layer, density rho below base and rho exp(-(h - base) / H) above, over [0, s] from height  \
       h0 with vertical direction cosine dz */                                                                          \
    static inline float atm_layer(float h0, float dz, float s, float rho, float base, float H) {                       \
        if (!(rho > 0.0f) || !(s > 0.0f))                                                                               \
            return 0.0f;                                                                                                \
        float Hs = fmax(H, 0.01f);                                                                                      \
        float a = h0 - base;                                                                                            \
        float b = a + s * dz;                                                                                           \
        float below = 0.0f, above = 0.0f;                                                                               \
        if (a <= 0.0f && b <= 0.0f) {                                                                                   \
            below = s;                                                                                                  \
        } else if (a >= 0.0f && b >= 0.0f) {                                                                            \
            above = atm_seg(a, b, s, Hs);                                                                               \
        } else {                                                                                                        \
            float tc = fmin(fmax(-a / dz, 0.0f), s);                                                                    \
            if (a < 0.0f) {                                                                                             \
                below = tc;                                                                                             \
                above = atm_seg(0.0f, fmax(b, 0.0f), s - tc, Hs);                                                       \
            } else {                                                                                                    \
                above = atm_seg(a, 0.0f, tc, Hs);                                                                       \
                below = s - tc;                                                                                         \
            }                                                                                                           \
        }                                                                                                               \
        return rho * (below + above);                                                                                   \
    }                                                                                                                   \
    /* phases x 4 pi (mean 1 over the sphere): Rayleigh 3/4 (1 + c^2); Henyey-Greenstein (1 - g^2) / k^1.5 */           \
    static inline float atm_phase_r(float c) { return 0.75f * (1.0f + c * c); }                                         \
    static inline float atm_phase_m(float c, float g) {                                                                 \
        float gg = fmin(fmax(g, -0.95f), 0.95f);                                                                        \
        float k = fmax(1.0f + gg * gg - 2.0f * gg * c, 1.0e-4f);                                                        \
        return (1.0f - gg * gg) / (k * sqrt(k));                                                                        \
    }                                                                                                                   \
    /* one channel of the game's in-scatter weight (Br Phi_R + Bm Phi_M) / (Br + Bm); 1 without scattering data */      \
    static inline float atm_mix(float br, float bm, float pr, float pm) {                                               \
        float s = br + bm;                                                                                              \
        return s > 1.0e-20f ? (br * pr + bm * pm) / s : 1.0f;                                                           \
    }                                                                                                                   \
    /* the whole pixel: NDC (nx, ny), the stored depth d (0..1, 1 = cleared / sky) */                                   \
    static inline AtmOut atm_pixel(ATM_CONST AtmU &u, float nx, float ny, float d) {                                   \
        AtmOut o;                                                                                                       \
        o.t = 1.0f;                                                                                                     \
        o.r = 0.0f;                                                                                                     \
        o.g = 0.0f;                                                                                                     \
        o.b = 0.0f;                                                                                                     \
        float hx = nx * u.m[0] + ny * u.m[4] + d * u.m[8] + u.m[12];                                                    \
        float hy = nx * u.m[1] + ny * u.m[5] + d * u.m[9] + u.m[13];                                                    \
        float hz = nx * u.m[2] + ny * u.m[6] + d * u.m[10] + u.m[14];                                                   \
        float hw = nx * u.m[3] + ny * u.m[7] + d * u.m[11] + u.m[15];                                                   \
        if (!(hw > 1.0e-30f))                                                                                           \
            return o;                                                                                                   \
        float vx = hx / hw, vy = hy / hw, vz = hz / hw;                                                                 \
        float dist = sqrt(vx * vx + vy * vy + vz * vz);                                                                 \
        if (!(dist > 1.0e-4f))                                                                                          \
            return o;                                                                                                   \
        float dx = vx / dist, dy = vy / dist, dz = vz / dist;                                                           \
        float s = d >= 1.0f ? u.misc[1] : fmin(dist, u.misc[1]);                                                        \
        float s0 = u.misc[0];                                                                                           \
        if (!(s > s0))                                                                                                  \
            return o;                                                                                                   \
        float h0 = u.eye[2] + dz * s0;                                                                                  \
        float L = s - s0;                                                                                               \
        float tau = atm_layer(h0, dz, L, u.l0[0], u.l0[1], u.l0[2]) + atm_layer(h0, dz, L, u.l1[0], u.l1[1], u.l1[2]); \
        tau *= u.misc[3];                                                                                               \
        if (!(tau > 0.0f))                                                                                              \
            return o;                                                                                                   \
        o.t = fmax(exp(-tau), 1.0f - u.misc[2]);                                                                        \
        float c = dx * u.sun[0] + dy * u.sun[1] + dz * u.sun[2];                                                        \
        float pr = atm_phase_r(c);                                                                                      \
        float pm = fmin(atm_phase_m(c, u.sun[3]), u.bm[3]);                                                             \
        float k = u.br[3];                                                                                              \
        o.r = u.suncol[0] * k * atm_mix(u.br[0], u.bm[0], pr, pm) + u.amb[0];                                           \
        o.g = u.suncol[1] * k * atm_mix(u.br[1], u.bm[1], pr, pm) + u.amb[1];                                           \
        o.b = u.suncol[2] * k * atm_mix(u.br[2], u.bm[2], pr, pm) + u.amb[2];                                           \
        return o;                                                                                                       \
    }

// The C++ twin (float overloads, as in Metal).
#define ATM_CONST const
using std::exp;
using std::fabs;
using std::fmax;
using std::fmin;
using std::sqrt;
ATM_MATHS
#undef ATM_CONST

// The blend the pass does (src ONE, dst SRC_ALPHA on rgb; the frame's alpha is kept).
inline float atm_blend(float col, const AtmOut &o) { return col * o.t + o.r * (1.0f - o.t); }

#define ATM_S2(...) #__VA_ARGS__
#define ATM_S(...) ATM_S2(__VA_ARGS__)
// The Metal library: the same maths (ATM_CONST is still a name here; the prefix defines it), a full-screen triangle and
// the fragment that reads the soft-particle depth copy (R32F, the nearest sample). Output rgb = L_in (1 - T), a = T;
// debug: rgb = 1 - T, a = 0 (the fog factor replaces the picture).
inline const char *atm_source() {
    static const char *const s = "#include <metal_stdlib>\nusing namespace metal;\n#define ATM_CONST constant\n"
                                 "struct AtmU { float m[16]; float eye[4]; float sun[4]; float br[4]; float bm[4]; "
                                 "float suncol[4]; float amb[4]; float l0[4]; float l1[4]; float misc[4]; };\n"
                                 "struct AtmOut { float t, r, g, b; };\n" ATM_S(ATM_MATHS) "\n"
                                 "struct AtmVo { float4 pos [[position]]; };\n"
                                 "vertex AtmVo atm_vs(uint vid [[vertex_id]]) {\n"
                                 "    AtmVo o;\n"
                                 "    float2 c = float2(vid == 1u ? 2.0 : 0.0, vid == 2u ? 2.0 : 0.0);\n"
                                 "    o.pos = float4(c * 2.0 - 1.0, 0.0, 1.0);\n"
                                 "    return o;\n"
                                 "}\n"
                                 "fragment float4 atm_fs(AtmVo i [[stage_in]], texture2d<float> z [[texture(0)]],\n"
                                 "                       constant AtmU &u [[buffer(0)]]) {\n"
                                 "    uint2 size = uint2(z.get_width(), z.get_height());\n"
                                 "    uint2 p = min(uint2(i.pos.xy), size - uint2(1));\n"
                                 "    float nx = 2.0 * (float(p.x) + 0.5) / float(size.x) - 1.0;\n"
                                 "    float ny = 1.0 - 2.0 * (float(p.y) + 0.5) / float(size.y);\n"
                                 "    AtmOut o = atm_pixel(u, nx, ny, z.read(p).x);\n"
                                 "    if (u.amb[3] > 0.5)\n"
                                 "        return float4(float3(1.0 - o.t), 0.0);\n"
                                 "    return float4(float3(o.r, o.g, o.b) * (1.0 - o.t), o.t);\n"
                                 "}\n";
    return s;
}

// ---- host side ------------------------------------------------------------------------------------------------------
// The mod's two packets (private SetTransform states 0x202 / 0x203, kit dx/host_d9.h HOST_D9_TS_ATMOS_*):
//  a: sun direction xyz, HG g | Br rgb, - | Bm rgb, sun intensity | sun colour rgb, -
//  b: haze density, base, scale height | fog density, base, scale height | start, far, max opacity |
//     ambient rgb | strength, Mie cap, -, mode (1 = this frame's parameters, at the main view's begin; 2 = the scene
//     end, where a frame without particles gets its pass)
struct Params {
    float a[16], b[16];
};
inline float atm_mode(const Params &p) { return p.b[15]; }
// Any fog at all (the host runs no pass otherwise: exact pixels).
inline bool atm_active(const Params &p) {
    const float k = p.b[12];
    return std::isfinite(k) && k > 0.0f && ((p.b[0] > 0.0f && std::isfinite(p.b[0])) || (p.b[3] > 0.0f && std::isfinite(p.b[3])));
}

// Row-vector 4x4 inverse (double; Gauss-Jordan with partial pivoting). False when singular or not finite.
inline bool atm_inverse(const double a[4][4], double out[4][4]) {
    double m[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j)
            m[i][j] = j < 4 ? a[i][j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int piv = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(m[r][c]) > std::fabs(m[piv][c]))
                piv = r;
        if (!(std::fabs(m[piv][c]) > 1e-300) || !std::isfinite(m[piv][c]))
            return false;
        if (piv != c)
            for (int j = 0; j < 8; ++j)
                std::swap(m[c][j], m[piv][j]);
        const double inv = 1.0 / m[c][c];
        for (int j = 0; j < 8; ++j)
            m[c][j] *= inv;
        for (int r = 0; r < 4; ++r)
            if (r != c) {
                const double f = m[r][c];
                for (int j = 0; j < 8; ++j)
                    m[r][j] -= f * m[c][j];
            }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            out[i][j] = m[i][j + 4];
            if (!std::isfinite(out[i][j]))
                return false;
        }
    return true;
}

// The uniforms from the frame's WORLD view-projection VP (row-vector, clip = (p, 1) x VP; D3D depth 0..1) and the
// packets. The eye is the point VP sends to clip (0, 0, z, 0): row 2 of VP^-1, dehomogenised. The inverse is made
// camera-relative in double (p - eye, so float keeps centimetres at 4 km). False: no usable camera.
inline bool atm_uniforms(const double vp[4][4], const Params &p, bool debug, AtmU *u) {
    double inv[4][4];
    if (!atm_inverse(vp, inv) || !(std::fabs(inv[2][3]) > 1e-30))
        return false;
    double eye[3];
    for (int j = 0; j < 3; ++j)
        eye[j] = inv[2][j] / inv[2][3];
    memset(u, 0, sizeof *u);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            u->m[4 * i + j] = (float)(j < 3 ? inv[i][j] - eye[j] * inv[i][3] : inv[i][3]);
    for (int j = 0; j < 3; ++j)
        u->eye[j] = (float)eye[j];
    double sl = std::sqrt((double)p.a[0] * p.a[0] + (double)p.a[1] * p.a[1] + (double)p.a[2] * p.a[2]);
    if (!(sl > 1e-6) || !std::isfinite(sl))
        sl = 0.0;
    for (int j = 0; j < 3; ++j) {
        u->sun[j] = sl > 0.0 ? (float)(p.a[j] / sl) : 0.0f;
        u->br[j] = std::isfinite(p.a[4 + j]) ? std::fmax(p.a[4 + j], 0.0f) : 0.0f;
        u->bm[j] = std::isfinite(p.a[8 + j]) ? std::fmax(p.a[8 + j], 0.0f) : 0.0f;
        u->suncol[j] = p.a[12 + j];
        u->amb[j] = p.b[9 + j];
    }
    u->sun[3] = p.a[3];
    u->br[3] = sl > 0.0 ? p.a[11] : 0.0f;   // no sun direction: no sun glow
    u->bm[3] = p.b[13] > 0.0f ? p.b[13] : 8.0f;
    u->amb[3] = debug ? 1.0f : 0.0f;
    for (int j = 0; j < 3; ++j) {
        u->l0[j] = p.b[j];
        u->l1[j] = p.b[3 + j];
    }
    u->misc[0] = std::fmax(p.b[6], 0.0f);
    u->misc[1] = p.b[7] > 0.0f ? p.b[7] : 3000.0f;
    u->misc[2] = std::fmin(std::fmax(p.b[8], 0.0f), 1.0f);
    u->misc[3] = p.b[12];
    for (const float *f = &u->m[0]; f < &u->misc[0] + 4; ++f)
        if (!std::isfinite(*f))
            return false;
    return true;
}

// The frame's view-projection for the pass (COMB14B: the bare plurality pick found no camera at the bay, Camden and
// Rosewood - the game builds WVP per object, so the scene's VP splits into last-bit variants and instanced blocks
// outnumber it). In order: (1) with the mod's camera V_g (SetTransform 0x200 at the main view's begin), the largest
// same_block cluster whose V_g^-1 x block has the projection form (foundation pick_clusters) - P = V_g^-1 x VP is kept;
// (2) with the camera and a kept P: V_g x P (a frame whose scenery cluster is too small, e.g. looking at the sky);
// (3) without the camera: the largest cluster under the share rule. 0 = none.
enum { ATM_VP_NONE = 0, ATM_VP_CAMERA = 1, ATM_VP_KEPT_P = 2, ATM_VP_CLUSTER = 3 };
inline int atm_choose_vp(const remaster_fnd::Plurality &pl, const remaster_fnd::M4 *view, remaster_fnd::M4 *p_kept,
                         bool *have_p, remaster_fnd::M4 *vp) {
    using namespace remaster_fnd;
    M4 vinv{};
    const bool cam = view && inverse(*view, &vinv);
    double c[3] = {0.0, 0.0, 0.0};
    if (cam)
        camera_position(*view, c);
    const Pick pk = pick_clusters(pl, 8, 0.25, cam ? &vinv : nullptr, std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]));
    if (cam && pk.valid && pk.by_camera && pk.block) {
        *vp = from_const_block(pk.block);
        *p_kept = mul(vinv, *vp);
        *have_p = true;
        return ATM_VP_CAMERA;
    }
    if (cam && *have_p) {
        *vp = mul(*view, *p_kept);
        return ATM_VP_KEPT_P;
    }
    if (!cam && pk.valid && pk.block) {
        *vp = from_const_block(pk.block);
        return ATM_VP_CLUSTER;
    }
    return ATM_VP_NONE;
}

} // namespace remaster_atm
