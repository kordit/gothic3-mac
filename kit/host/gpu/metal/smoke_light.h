// Remaster run 14 lit smoke (review/run14/smoke-realism-design.md 4 stage 1 and 7): the host half, pure C++ (no Metal),
// so the offline tests (dx/tests/d3d9_tests.cpp) check the same code the renderer runs.
//
// How the kind reaches the draw: the game draws each emitter's particles as one batch inside 006da2f0 (the per-emitter
// end of the render loop 00503d00, EDI = the emitter). The core mod (mods/core/nfsmw/smoke_light.h) wraps 006da2f0 for
// that caller only, maps the emitter's key to a kind and sets private render states around the call (never device
// state the game reads; the kit's pipeline keys ignore them):
//   248 = kind (1..7, 0 = none), 245..247 = sun direction toward the sun x strength (float bits, world, z up),
//   242..244 = the emitter's position (float bits, world: the flare's inner light).
// All are set back to 0 after the draw, so nothing leaks to the rain or any later draw. With the setting off the mod
// sets nothing and the kit never builds the variant (byte-identical shaders, no new pipelines).
#pragma once

#include "../../../dx/d3d9_msl.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace remaster_smoke {

enum : uint32_t {
    RS_KIND = 248,                 // private render state: the kind
    RS_SUN = 245,                  // 245, 246, 247: sun x, y, z (float bits)
    RS_POS = 242,                  // 242, 243, 244: emitter position x, y, z (float bits)
    KIND_TYRE = 1,
    KIND_FLARE_RED = 2,
    KIND_STACK_STEAM = 3,
    KIND_STACK_SOOT = 4,
    KIND_FOG = 5,
    KIND_STEAM = 6,
    KIND_FLARE_GREEN = 7,
    KIND_MAX = 7,
};

// Per-kind start values (the owner tunes them; design 4 "per-kind parameters" and 7).
struct Kind {
    float s, g, albedo, kss;   // relight strength, HG anisotropy, albedo, alpha self-shadow
    float tilt, tpow, glow;    // alpha-gradient normal tilt, transmission power, forward-glow weight
    float amb, tint, erode;    // ambient level, fog shade tint, steam edge erosion
    float near0, near_range;   // near-camera fade start / range (m; range 0 = none)
    float soft_dist;           // soft-particle fade distance (m; 0 = the renderer's own)
    float light[4];            // flare: rgb, intensity
    float radius;              // flare light radius (m)
    bool flat;                 // flat-lit (fog): no sprite normal
};
inline Kind kind_defaults(uint32_t kind) {
    //            s     g     alb   kss   tilt  tpow  glow  amb   tint  erode near0 nrng soft  light                   rad   flat
    switch (kind) {
    case KIND_TYRE:        return {0.7f, 0.6f, 0.85f, 0.5f, 0.15f, 2.0f, 0.6f, 0.35f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, {0, 0, 0, 0}, 1.5f, false};
    case KIND_FLARE_RED:   return {0.7f, 0.6f, 0.9f, 0.5f, 0.15f, 2.0f, 0.6f, 0.35f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, {1.0f, 0.22f, 0.14f, 0.6f}, 1.5f, false};
    case KIND_STACK_STEAM: return {0.7f, 0.75f, 0.95f, 0.4f, 0.15f, 2.0f, 0.7f, 0.35f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, {0, 0, 0, 0}, 1.5f, false};
    case KIND_STACK_SOOT:  return {0.6f, 0.3f, 0.3f, 0.8f, 0.15f, 2.0f, 0.3f, 0.35f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, {0, 0, 0, 0}, 1.5f, false};
    case KIND_FOG:         return {0.5f, 0.8f, 0.9f, 0.0f, 0.0f, 1.0f, 0.35f, 0.5f, 0.5f, 0.0f, 1.0f, 3.0f, 4.0f, {0, 0, 0, 0}, 1.5f, true};
    case KIND_STEAM:       return {0.7f, 0.75f, 0.95f, 0.5f, 0.15f, 2.0f, 0.6f, 0.35f, 0.0f, 0.35f, 0.5f, 1.5f, 0.0f, {0, 0, 0, 0}, 1.5f, false};
    case KIND_FLARE_GREEN: return {0.7f, 0.6f, 0.9f, 0.5f, 0.15f, 2.0f, 0.6f, 0.35f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, {0.2f, 1.0f, 0.35f, 0.6f}, 1.5f, false};
    default:               return {0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, {0, 0, 0, 0}, 1.5f, true};
    }
}

// The 4 pi-normalised Henyey-Greenstein phase (mean 1 over the sphere): mu = cos(angle between the light's travel and
// the direction to the viewer), so mu = 1 is looking into the sun through the smoke.
inline float hg(float mu, float g) {
    g = g < -0.95f ? -0.95f : g > 0.95f ? 0.95f : g;
    const float d = 1.0f + g * g - 2.0f * g * mu;
    return (1.0f - g * g) / std::pow(d > 1e-4f ? d : 1e-4f, 1.5f);
}
// The lit term of one pixel: half-Lambert wrap x alpha self-shadow + (1 - alpha)^p transmission x HG forward glow.
inline float lit_term(const Kind &k, float wrap, float alpha, float mu) {
    const float t = std::pow(alpha < 1.0f ? 1.0f - alpha : 0.0f, k.tpow);
    return wrap * (1.0f - k.kss * alpha) + k.glow * t * hg(mu, k.g);
}
// The reference: a side-lit (mu 0) average sprite (wrap 0.5, alpha 0.5). Its ratio is exactly 1, so the effect keeps the
// game's mean brightness from the side and only redistributes light (brighter toward the sun, flatter with it behind).
inline float ref_term(const Kind &k) { return lit_term(k, 0.5f, 0.5f, 0.0f); }
// The C++ twin of the shader's relight (smoke_epilogue): the factor on the game's colour, before the strength mix.
inline float ratio(const Kind &k, float wrap, float alpha, float mu, float vis) {
    const float lt = k.amb + vis * k.albedo * lit_term(k, wrap, alpha, mu);
    const float lr = k.amb + vis * k.albedo * ref_term(k);
    float r = lt / (lr > 1e-4f ? lr : 1e-4f);
    return r < 0.35f ? 0.35f : r > 3.0f ? 3.0f : r;
}
inline float factor(const Kind &k, float wrap, float alpha, float mu, float vis) {
    const float s = k.s < 0.0f ? 0.0f : k.s > 1.0f ? 1.0f : k.s;
    return 1.0f + (ratio(k, wrap, alpha, mu, vis) - 1.0f) * s;
}

// The camera from a draw's clip rows (clip = M p, rows c[clip..clip+3]): the point with clip x = y = w = 0. False when
// the rows are degenerate (an orthographic or broken matrix).
inline bool eye_from_clip(const float *r0, const float *r1, const float *r3, float eye[3]) {
    const double a[3][3] = {{r0[0], r0[1], r0[2]}, {r1[0], r1[1], r1[2]}, {r3[0], r3[1], r3[2]}};
    const double b[3] = {-r0[3], -r1[3], -r3[3]};
    const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                       a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (!(std::fabs(det) > 1e-12))
        return false;
    for (int c = 0; c < 3; ++c) {
        double m[3][3];
        std::memcpy(m, a, sizeof m);
        for (int r = 0; r < 3; ++r)
            m[r][c] = b[r];
        const double dc = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                          m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        eye[c] = (float)(dc / det);
    }
    return std::isfinite(eye[0]) && std::isfinite(eye[1]) && std::isfinite(eye[2]);
}

inline float bits_f(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
// TEST ONLY: RECOMP_REMASTER_LITSMOKE_K<kind>="s,g,albedo,kss,glow" overrides a kind's start values (owner tuning
// without a rebuild); RECOMP_REMASTER_LITSMOKE_DEBUG=1 shows the relight ratio as grey, =2 the kind as a colour.
inline Kind kind_params(uint32_t kind) {
    Kind k = kind_defaults(kind);
    char name[40];
    std::snprintf(name, sizeof name, "RECOMP_REMASTER_LITSMOKE_K%u", kind);
    if (const char *e = std::getenv(name)) {
        float v[5];
        const int n = std::sscanf(e, "%f%*[,:]%f%*[,:]%f%*[,:]%f%*[,:]%f", &v[0], &v[1], &v[2], &v[3], &v[4]);
        if (n >= 1) k.s = v[0];
        if (n >= 2) k.g = v[1];
        if (n >= 3) k.albedo = v[2];
        if (n >= 4) k.kss = v[3];
        if (n >= 5) k.glow = v[4];
    }
    return k;
}
inline int debug_view() {
    static const int d = [] {
        const char *e = std::getenv("RECOMP_REMASTER_LITSMOKE_DEBUG");
        return e && (*e == '1' || *e == '2') ? *e - '0' : 0;
    }();
    return d;
}

// The draw's parameter block from its render states and clip rows (nullptr: no clip rows). False: draw it plain (no
// kind or no camera). No sun direction: strength 0 (the relight ratio is 1; the fades still apply).
inline bool fill(const uint32_t *rs, const float *clip_rows, float seconds, const Kind &k, D9SmokeParams *p) {
    const uint32_t kind = rs[RS_KIND];
    if (kind < 1 || kind > KIND_MAX || !clip_rows)
        return false;
    std::memset(p, 0, sizeof *p);
    float sx = bits_f(rs[RS_SUN]), sy = bits_f(rs[RS_SUN + 1]), sz = bits_f(rs[RS_SUN + 2]);
    float sl = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (!(sl > 1e-4f) || !std::isfinite(sl)) {   // no sun (night, or the mod found none): ratio 1, fades still apply
        sx = sy = 0.0f;
        sz = 1.0f;
        sl = 0.0f;
    }
    float eye[3];
    if (!eye_from_clip(clip_rows, clip_rows + 4, clip_rows + 12, eye))
        return false;
    const float inv = sl > 0.0f ? 1.0f / sl : 1.0f;
    p->sun[0] = sx * inv;
    p->sun[1] = sy * inv;
    p->sun[2] = sz * inv;
    p->sun[3] = sl > 1.0f ? 1.0f : sl;
    p->eye[0] = eye[0];
    p->eye[1] = eye[1];
    p->eye[2] = eye[2];
    p->k[0] = k.s;
    p->k[1] = k.g;
    p->k[2] = k.albedo;
    p->k[3] = k.kss;
    p->k2[0] = k.tilt;
    p->k2[1] = k.tpow;
    p->k2[2] = k.glow;
    p->amb[0] = k.amb;
    p->amb[1] = ref_term(k);
    p->amb[2] = k.tint;
    p->amb[3] = k.erode;
    if (k.light[3] > 0.0f) {
        const float flicker = 0.85f + 0.15f * std::sin(seconds * 23.0f) * std::sin(seconds * 7.3f + 1.0f);
        p->light[0] = bits_f(rs[RS_POS]);
        p->light[1] = bits_f(rs[RS_POS + 1]);
        p->light[2] = bits_f(rs[RS_POS + 2]);
        p->light[3] = std::isfinite(p->light[0] + p->light[1] + p->light[2]) ? k.light[3] * flicker : 0.0f;
        p->lcol[0] = k.light[0];
        p->lcol[1] = k.light[1];
        p->lcol[2] = k.light[2];
        p->lcol[3] = k.radius;
    }
    p->nearf[0] = k.near0;
    p->nearf[1] = k.near_range > 0.0f ? 1.0f / k.near_range : 0.0f;
    p->nearf[2] = (float)debug_view();
    return true;
}

} // namespace remaster_smoke
