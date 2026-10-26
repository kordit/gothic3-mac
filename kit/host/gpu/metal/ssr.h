// ssr.h - Remaster run 15 Wave B WB3: screen-space reflections (review/run15/waveb-plan.md WB3; consumes the
// foundation's FndFrame, foundation.h). Included by d3d9_metal.mm (kSsrSource, ssr_params) and by
// dx/tests/d3d9_tests.cpp (test_ssr: the CPU reference ssr_trace_ref / ssr_resolve_ref on synthetic buffers).
// Pure C++ here, no Metal types.
//
// Where: at the scene end (252) after fnd_end and before upscale_end, at the internal (render) size, so MetalFX
// stabilises it when upscaling is on. Inputs: the scene colour, the scene depth, S0 (oct world normal, roughness,
// linear view depth) and S1 (MatClass tag, F0, SurfFlag) from the surface attachments (WB1b).
// Passes: (1) blit the scene into ssr_src + 4 mips; (2) ssr_trace_cs at half size: per half pixel (source pixel 2s)
// the view position from the depth and P^-1, N_view = oct_decode(S0.xy) x V_g, R = reflect(V, N), a screen-space
// march (40 steps in screen space with perspective-correct 1/z, 4 binary refines, thickness 1.5 % of z), output
// (hit pixel x, y, confidence, screen distance px) RGBA32F; (3) ssr_resolve_fs at full size into the scene back buffer
// (blend src-alpha, destination alpha kept: it carries the HDR mark): a depth/normal bilateral upsample of the 4 half
// samples, hit colour from ssr_src at mip = f(roughness, distance), w = strength x confidence x Schlick(F0, N.V) x the
// class weight (car paint 0.5: it already has the game's environment reflection), out = lerp(scene, hit, w).
// Mask v1: tag CAR_PAINT / CAR_CHROME / GLASS / WATER (ROAD only with RECOMP_REMASTER_SSR_ROAD=1: the planar wet-road
// reflection stays first), not emissive, not stale (|S0.w - linear depth| < z/512), roughness < 0.4, not sky.
// TEST fallback (RECOMP_REMASTER_SSR_DEPTHN=1, flag 64): no S0/S1 -> normals from the depth, every non-sky pixel is a
// receiver with roughness 0.1 and F0 0.04 (shows the trace before the surface attachments exist; not a look).
// Conventions are the foundation's: row-vector matrices, view x right / y DOWN / z forward, D3D depth 0..1, pixel
// (i, j) centre p = (i + 0.5, j + 0.5) shows NDC nx = 2 p.x / w - 1 - hx, ny = 1 - 2 p.y / h - hy.
#pragma once

#include "foundation.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace remaster_ssr {

enum : uint32_t {
    F_ROAD = 1,       // ROAD receives (TEST)
    F_SURF = 2,       // S0/S1 present
    F_DITHER = 4,     // per-pixel start dither (interleaved gradient noise); off in the unit tests
    F_BACKFACE = 8,   // reject hits on a surface facing away from the ray (needs S0)
    F_DBG_MASK = 16,  // RECOMP_REMASTER_SSR_DEBUG=1: mask x confidence (grey 0.15 + 0.85 conf in the mask, black out)
    F_DBG_REFL = 32,  // RECOMP_REMASTER_SSR_DEBUG=2: the reflection only (hit colour x w)
    F_DEPTHN = 64,    // TEST fallback: normals from the depth, no S0/S1
    F_DITHER_FRAME = 128,   // run 25 TEST (W3-stable control): the dither start moves every frame (seed in pad)
};

// The kernel parameters (MSL struct SsrParams, same layout: 256 bytes). Matrices as MSL columns: m[4 j + i] = A[i][j],
// so `v * M` in MSL is the row-vector product.
struct SsrParams {
    float Pinv[16];      // NDC -> view
    float P[16];         // view -> clip
    float V[16];         // world -> view (directions use the 3x3)
    float size[2];       // render pixels (full size)
    float h[2];          // the draws' halfpix + jitter, NDC
    float zA, zB;        // linear depth z = zB / (d - zA) (zA = P[2][2], zB = P[3][2])
    float zn, maxdist;   // near plane, the longest ray (view units)
    float thick, strength;
    float paint_w, rough_max;
    uint32_t steps, refine;
    uint32_t flags, pad;
};
static_assert(sizeof(SsrParams) == 256, "SsrParams must match the MSL struct");

inline void put(float *dst, const remaster_fnd::M4 &a) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            dst[4 * j + i] = (float)a.m[i][j];
}

// Parameters from the frame (P = the D3D projection, V = V_g). False when P is not invertible or not a D3D
// projection (P[2][3] must be 1: clip w = view z), or V's 3x3 is not a rotation (no camera: the normals need it).
inline bool ssr_params(const remaster_fnd::M4 &P, const remaster_fnd::M4 &V, uint32_t w, uint32_t h, double jx, double jy,
                       double halfpix_scale, float strength, uint32_t flags, SsrParams *out) {
    remaster_fnd::M4 pinv;
    if (!w || !h || std::fabs(P.m[2][3] - 1.0) > 1e-3 || std::fabs(P.m[3][3]) > 1e-3 || !remaster_fnd::inverse(P, &pinv))
        return false;
    const double det = V.m[0][0] * (V.m[1][1] * V.m[2][2] - V.m[1][2] * V.m[2][1]) -
                       V.m[0][1] * (V.m[1][0] * V.m[2][2] - V.m[1][2] * V.m[2][0]) +
                       V.m[0][2] * (V.m[1][0] * V.m[2][1] - V.m[1][1] * V.m[2][0]);
    if ((flags & F_SURF) && !(std::fabs(det) > 0.5 && std::fabs(det) < 2.0))
        return false;
    SsrParams u{};
    put(u.Pinv, pinv);
    put(u.P, P);
    put(u.V, V);
    u.size[0] = (float)w;
    u.size[1] = (float)h;
    float dx = 0.0f, dy = 0.0f;
    remaster_fnd::jitter_halfpix(jx, jy, w, h, &dx, &dy);
    // the draw's halfpix ((63/64) / guest size, at the backing's scale: d3d9_metal.mm draw) plus the jitter
    u.h[0] = (float)((63.0 / 64.0) / (w * halfpix_scale)) + dx;
    u.h[1] = (float)(-(63.0 / 64.0) / (h * halfpix_scale)) + dy;
    u.zA = (float)P.m[2][2];
    u.zB = (float)P.m[3][2];
    const double zn = P.m[2][2] != 0.0 ? -P.m[3][2] / P.m[2][2] : 0.0;
    u.zn = zn > 0.0 ? (float)zn : 0.1f;
    u.maxdist = 250.0f;   // view units (metres): the march covers what is on screen, clipped at the border
    u.thick = 0.015f;
    u.strength = strength;
    u.paint_w = 0.5f;
    u.rough_max = 0.4f;
    u.steps = 40;
    u.refine = 4;
    u.flags = flags;
    *out = u;
    return true;
}

// ---- the CPU reference (the same arithmetic as kSsrSource, in float) ----------------------------------------------
struct V3 {
    float x, y, z;
};
inline V3 v3(float x, float y, float z) { return V3{x, y, z}; }
inline V3 operator+(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline V3 operator-(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline V3 operator*(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
inline V3 normalize(V3 a) {
    const float l = std::sqrt(dot(a, a));
    return l > 0.0f ? a * (1.0f / l) : a;
}
inline V3 mix3(V3 a, V3 b, float t) { return a + (b - a) * t; }
inline float saturate(float x) { return std::min(1.0f, std::max(0.0f, x)); }
inline float smoothstep(float e0, float e1, float x) {
    const float t = saturate((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}
// v (w = vw) x M, M as MSL columns
inline void mul4(const float *m, float x, float y, float z, float w, float o[4]) {
    for (int j = 0; j < 4; ++j)
        o[j] = x * m[4 * j + 0] + y * m[4 * j + 1] + z * m[4 * j + 2] + w * m[4 * j + 3];
}

// The synthetic / captured buffers the reference reads (row-major, w x h).
struct Images {
    int w = 0, h = 0;
    const float *depth = nullptr;   // D3D depth 0..1
    const float *s0 = nullptr;      // 4 floats per pixel (oct x, oct y, roughness, linear depth)
    const uint8_t *s1 = nullptr;    // 4 bytes per pixel (tag, F0 x 255, flags, 0)
    const float *color = nullptr;   // 3 floats per pixel (the scene; mip 0 only in the reference)
};

inline float class_weight(uint32_t tag, const SsrParams &u) {
    switch (tag) {
    case remaster_fnd::MAT_CAR_PAINT:
        return u.paint_w;
    case remaster_fnd::MAT_CAR_CHROME:
    case remaster_fnd::MAT_GLASS:
    case remaster_fnd::MAT_WATER:
        return 1.0f;
    case remaster_fnd::MAT_ROAD:
        return (u.flags & F_ROAD) ? 1.0f : 0.0f;
    default:
        return 0.0f;
    }
}
inline float default_f0(uint32_t tag) {   // when the writer left S1.g at 0
    switch (tag) {
    case remaster_fnd::MAT_CAR_PAINT:
        return 0.05f;
    case remaster_fnd::MAT_CAR_CHROME:
        return 0.8f;
    case remaster_fnd::MAT_GLASS:
        return 0.04f;
    case remaster_fnd::MAT_WATER:
        return 0.02f;
    default:
        return 0.03f;
    }
}
inline float lin(float d, const SsrParams &u) {
    const float den = d - u.zA;
    return u.zB / (std::fabs(den) > 1e-9f ? den : -1e-9f);
}
inline V3 view_pos(float px, float py, float d, const SsrParams &u) {
    float o[4];
    mul4(u.Pinv, 2.0f * px / u.size[0] - 1.0f - u.h[0], 1.0f - 2.0f * py / u.size[1] - u.h[1], d, 1.0f, o);
    return v3(o[0] / o[3], o[1] / o[3], o[2] / o[3]);
}
inline V3 project(V3 q, const SsrParams &u) {   // (pixel x, pixel y, 1/z)
    float c[4];
    mul4(u.P, q.x, q.y, q.z, 1.0f, c);
    const float iw = 1.0f / c[3];
    return v3((c[0] * iw + u.h[0] + 1.0f) * 0.5f * u.size[0], (1.0f - c[1] * iw - u.h[1]) * 0.5f * u.size[1], iw);
}
inline V3 oct_dec(float ex, float ey) {
    V3 n = v3(ex, ey, 1.0f - std::fabs(ex) - std::fabs(ey));
    const float t = std::max(-n.z, 0.0f);
    n.x += n.x >= 0.0f ? -t : t;
    n.y += n.y >= 0.0f ? -t : t;
    return normalize(n);
}
inline float ign(int x, int y) {   // interleaved gradient noise
    const float f = 0.06711056f * (float)x + 0.00583715f * (float)y;
    const float g = 52.9829189f * (f - std::floor(f));
    return g - std::floor(g);
}

struct Surf {
    V3 pos{}, n{};
    float rough = 1.0f, f0 = 0.0f, cw = 0.0f, zl = 0.0f;
    bool ok = false;
};
inline Surf surface(const Images &im, const SsrParams &u, int x, int y) {
    Surf s;
    const float d = im.depth[y * im.w + x];
    s.pos = view_pos(x + 0.5f, y + 0.5f, d, u);
    s.zl = s.pos.z;
    if (d >= 1.0f)
        return s;
    if (u.flags & F_SURF) {
        const float *a = im.s0 + 4 * (y * im.w + x);
        const uint8_t *b = im.s1 + 4 * (y * im.w + x);
        const uint32_t tag = b[0];
        const float cw = class_weight(tag, u);
        if (cw <= 0.0f || (b[2] & remaster_fnd::SURF_EMISSIVE))
            return s;
        if (std::fabs(a[3] - s.zl) > s.zl / 512.0f)
            return s;   // stale: the last S0 write is not the visible surface
        s.rough = a[2];
        if (s.rough >= u.rough_max)
            return s;
        const V3 nw = oct_dec(a[0], a[1]);
        float o[4];
        mul4(u.V, nw.x, nw.y, nw.z, 0.0f, o);
        s.n = normalize(v3(o[0], o[1], o[2]));
        s.f0 = b[1] ? b[1] / 255.0f : default_f0(tag);
        s.cw = cw;
        s.ok = true;
    } else if (u.flags & F_DEPTHN) {
        auto at = [&](int i, int j) {
            i = std::min(std::max(i, 0), im.w - 1);
            j = std::min(std::max(j, 0), im.h - 1);
            return view_pos(i + 0.5f, j + 0.5f, im.depth[j * im.w + i], u);
        };
        const V3 l = at(x - 1, y), r = at(x + 1, y), t = at(x, y - 1), b = at(x, y + 1);
        const V3 dx = std::fabs(r.z - s.pos.z) < std::fabs(s.pos.z - l.z) ? r - s.pos : s.pos - l;
        const V3 dy = std::fabs(b.z - s.pos.z) < std::fabs(s.pos.z - t.z) ? b - s.pos : s.pos - t;
        V3 n = normalize(cross(dy, dx));
        if (dot(n, s.pos) > 0.0f)
            n = n * -1.0f;
        s.n = n;
        s.rough = 0.1f;
        s.f0 = 0.04f;
        s.cw = 1.0f;
        s.ok = true;
    }
    return s;
}

struct Hit {   // the trace texel: hit pixel (continuous), confidence, screen distance (px); conf 0 = miss
    float px = 0.0f, py = 0.0f, conf = 0.0f, dist = 0.0f;
};
inline Hit march(const Images &im, const SsrParams &u, const Surf &s, int x, int y) {
    Hit r;
    const V3 vd = normalize(s.pos);
    if (dot(s.n, vd) >= 0.0f)
        return r;   // the normal faces away from the camera
    const V3 R = vd - s.n * (2.0f * dot(s.n, vd));
    float L = u.maxdist;
    if (R.z < 0.0f)
        L = std::min(L, (s.pos.z - 1.5f * u.zn) / -R.z);
    if (L <= 0.0f)
        return r;
    const V3 a = project(s.pos, u);
    V3 b = project(s.pos + R * L, u);
    const float dx = b.x - a.x, dy = b.y - a.y;
    float tmax = 1.0f;
    if (dx > 0.0f)
        tmax = std::min(tmax, (u.size[0] - 0.5f - a.x) / dx);
    else if (dx < 0.0f)
        tmax = std::min(tmax, (0.5f - a.x) / dx);
    if (dy > 0.0f)
        tmax = std::min(tmax, (u.size[1] - 0.5f - a.y) / dy);
    else if (dy < 0.0f)
        tmax = std::min(tmax, (0.5f - a.y) / dy);
    if (tmax <= 0.0f)
        return r;
    b = mix3(a, b, tmax);
    const float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
    if (len < 1.0f)
        return r;
    const float t0 = std::min(1.0f, 1.5f / len);
    const float dith = (u.flags & F_DITHER) ? ign(x, y) : 0.5f;
    auto zs_at = [&](V3 c) {
        const int i = std::min(std::max((int)c.x, 0), im.w - 1), j = std::min(std::max((int)c.y, 0), im.h - 1);
        return im.depth[j * im.w + i];
    };
    float tp = 0.0f, zrp = 1.0f / a.z, th = -1.0f;
    for (uint32_t i = 0; i < u.steps; ++i) {
        const float t = t0 + (1.0f - t0) * ((float)i + dith) / (float)u.steps;
        const V3 c = mix3(a, b, t);
        const float zr = 1.0f / c.z, zs = lin(zs_at(c), u);
        if (zr >= zs && zrp <= zs * (1.0f + u.thick)) {
            th = t;
            break;
        }
        tp = t;
        zrp = zr;
    }
    if (th < 0.0f)
        return r;
    float lo = tp, hi = th;
    for (uint32_t k = 0; k < u.refine; ++k) {
        const float m = 0.5f * (lo + hi);
        const V3 c = mix3(a, b, m);
        if (1.0f / c.z >= lin(zs_at(c), u))
            hi = m;
        else
            lo = m;
    }
    const V3 c = mix3(a, b, hi);
    const float dh = zs_at(c);
    const float zr = 1.0f / c.z, zs = lin(dh, u), zlo = 1.0f / mix3(a, b, lo).z;
    if (dh >= 1.0f || zr - zs > u.thick * zs + std::fabs(zr - zlo))
        return r;   // the sky, or behind a thin object
    if ((u.flags & F_BACKFACE) && (u.flags & F_SURF)) {
        const int i = std::min(std::max((int)c.x, 0), im.w - 1), j = std::min(std::max((int)c.y, 0), im.h - 1);
        const float *e = im.s0 + 4 * (j * im.w + i);
        const V3 nw = oct_dec(e[0], e[1]);
        float o[4];
        mul4(u.V, nw.x, nw.y, nw.z, 0.0f, o);
        if (dot(v3(o[0], o[1], o[2]), R) > 0.0f)
            return r;   // the back of a surface
    }
    const float ux = c.x / u.size[0], uy = c.y / u.size[1];
    const float edge = saturate(std::min(std::min(ux, 1.0f - ux), std::min(uy, 1.0f - uy)) / 0.1f);
    const float fcam = smoothstep(-0.4f, 0.0f, R.z);
    const float fdist = 1.0f - smoothstep(0.6f, 1.0f, hi * tmax);
    const float frough = 1.0f - smoothstep(0.6f * u.rough_max, u.rough_max, s.rough);
    r.px = c.x;
    r.py = c.y;
    r.conf = edge * fcam * fdist * frough;
    r.dist = std::sqrt((c.x - a.x) * (c.x - a.x) + (c.y - a.y) * (c.y - a.y));
    return r;
}
// The trace at one source pixel (x, y) (the half-size pass traces source pixel 2s).
inline Hit ssr_trace_ref(const Images &im, const SsrParams &u, int x, int y, Surf *out_surf = nullptr) {
    const Surf s = surface(im, u, x, y);
    if (out_surf)
        *out_surf = s;
    return s.ok ? march(im, u, s, x, y) : Hit{};
}
inline float fresnel(const Surf &s) {
    const float nv = saturate(-dot(s.n, normalize(s.pos)));
    const float k = 1.0f - nv;
    return s.f0 + (1.0f - s.f0) * k * k * k * k * k;
}
// The resolve's weight for a pixel with confidence conf (the composite: out = lerp(scene, hit, w)).
inline float weight(const Surf &s, float conf, const SsrParams &u) {
    return s.ok ? saturate(u.strength * conf * fresnel(s) * s.cw) : 0.0f;
}

// ---- synthetic scene for the unit tests and the GPU check (harness/tools/ssr_gpu_check.mm) ------------------------
// Camera at the origin (view = world rotated by V), 60 deg vertical FOV, near 0.5, far 1000. A ground plane at view
// y = +2 (below the eye; tag `ground_tag`, roughness `rough`) and a wall z = 30 for |x| < wall (WORLD, matte), sky
// beyond. Wall colour: a horizontal ramp (x), ground colour: dark grey.
struct Synth {
    int w = 320, h = 180;
    float wall = 20.0f;
    remaster_fnd::M4 P{}, V{};
    std::vector<float> depth, s0, color;
    std::vector<uint8_t> s1;
    Images images() const {
        Images im;
        im.w = w;
        im.h = h;
        im.depth = depth.data();
        im.s0 = s0.data();
        im.s1 = s1.data();
        im.color = color.data();
        return im;
    }
};
inline remaster_fnd::M4 d3d_projection(double fovy, double aspect, double zn, double zf) {
    remaster_fnd::M4 p{};
    const double ys = 1.0 / std::tan(0.5 * fovy);
    p.m[0][0] = ys / aspect;
    p.m[1][1] = -ys;   // view y down -> NDC y up
    p.m[2][2] = zf / (zf - zn);
    p.m[2][3] = 1.0;
    p.m[3][2] = -zn * zf / (zf - zn);
    return p;
}
// V = identity (world = view) unless yaw != 0 (a rotation about the view y axis: the normal transform is tested).
inline Synth make_synth(uint32_t ground_tag, float rough, double yaw = 0.0, float wall = 20.0f, int w = 320, int h = 180) {
    using namespace remaster_fnd;
    Synth sy;
    sy.w = w;
    sy.h = h;
    sy.wall = wall;
    sy.P = d3d_projection(60.0 * M_PI / 180.0, (double)w / h, 0.5, 1000.0);
    sy.V = identity();
    sy.V.m[0][0] = std::cos(yaw);
    sy.V.m[0][2] = -std::sin(yaw);
    sy.V.m[2][0] = std::sin(yaw);
    sy.V.m[2][2] = std::cos(yaw);
    M4 vinv;
    inverse(sy.V, &vinv);
    SsrParams u;
    ssr_params(sy.P, sy.V, w, h, 0, 0, 1.0, 1.0f, 0, &u);
    sy.depth.assign((size_t)w * h, 1.0f);
    sy.s0.assign((size_t)w * h * 4, 0.0f);
    sy.s1.assign((size_t)w * h * 4, 0);
    sy.color.assign((size_t)w * h * 3, 0.0f);
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            const V3 dir = normalize(view_pos(i + 0.5f, j + 0.5f, 0.5f, u));
            float t = 1e30f;
            int what = 0;
            V3 n{};
            if (dir.y > 1e-6f && 2.0f / dir.y < t) {
                t = 2.0f / dir.y;
                what = 1;
                n = v3(0, -1, 0);
            }
            if (dir.z > 1e-6f && 30.0f / dir.z < t && std::fabs(dir.x * 30.0f / dir.z) < wall) {
                t = 30.0f / dir.z;
                what = 2;
                n = v3(0, 0, -1);
            }
            const size_t k = (size_t)j * w + i;
            const V3 q = dir * t;
            if (!what || q.z > 900.0f)
                continue;   // the sky (or beyond the far plane)
            sy.depth[k] = (float)(sy.P.m[2][2] + sy.P.m[3][2] / q.z);
            // world normal = view normal x V^-1 (rows)
            const double nw[3] = {n.x * vinv.m[0][0] + n.y * vinv.m[1][0] + n.z * vinv.m[2][0],
                                  n.x * vinv.m[0][1] + n.y * vinv.m[1][1] + n.z * vinv.m[2][1],
                                  n.x * vinv.m[0][2] + n.y * vinv.m[1][2] + n.z * vinv.m[2][2]};
            double ou, ov;
            oct_encode(nw, &ou, &ov);
            sy.s0[4 * k + 0] = (float)ou;
            sy.s0[4 * k + 1] = (float)ov;
            sy.s0[4 * k + 2] = what == 1 ? rough : 0.9f;
            sy.s0[4 * k + 3] = q.z;
            sy.s1[4 * k + 0] = (uint8_t)(what == 1 ? ground_tag : MAT_WORLD);
            const float c = what == 2 ? 0.2f + 0.6f * saturate((q.x + 20.0f) / 40.0f) : 0.1f;
            sy.color[3 * k + 0] = c;
            sy.color[3 * k + 1] = what == 2 ? 0.5f : 0.1f;
            sy.color[3 * k + 2] = what == 2 ? 1.0f - c : 0.1f;
        }
    return sy;
}
// The analytic mirror hit of source pixel (x, y) on the ground (V = identity): the pixel where the reflected ray meets
// the wall; false when it does not (sky, or the pixel is not ground).
inline bool synth_analytic(const Synth &sy, const SsrParams &u, int x, int y, float *hx, float *hy) {
    const V3 dir = normalize(view_pos(x + 0.5f, y + 0.5f, 0.5f, u));
    if (dir.y <= 1e-6f)
        return false;
    const V3 g = dir * (2.0f / dir.y);
    const V3 R = v3(dir.x, -dir.y, dir.z);
    if (R.z <= 1e-6f || g.z >= 30.0f)
        return false;
    const V3 q = g + R * ((30.0f - g.z) / R.z);
    if (std::fabs(q.x) >= sy.wall)
        return false;
    const V3 p = project(q, u);
    *hx = p.x;
    *hy = p.y;
    return p.x >= 0 && p.y >= 0 && p.x < sy.w && p.y < sy.h;
}

// ---- the Metal source (compiled only when ssr is on: off = no new MSL anywhere) ------------------------------------
inline const char *ssr_source() {
    return R"MSL(#include <metal_stdlib>
using namespace metal;
struct SsrParams {
    float4x4 Pinv, P, V;
    float2 size, h;
    float zA, zB, zn, maxdist, thick, strength, paint_w, rough_max;
    uint steps, refine, flags, pad;
};
struct SsrSurf { float3 pos; float3 n; float rough; float f0; float cw; bool ok; };
static float ssr_lin(float d, constant SsrParams &u) {
    float den = d - u.zA;
    return u.zB / (fabs(den) > 1e-9 ? den : -1e-9);
}
static float3 ssr_view(float2 p, float d, constant SsrParams &u) {
    float4 v = float4(2.0 * p.x / u.size.x - 1.0 - u.h.x, 1.0 - 2.0 * p.y / u.size.y - u.h.y, d, 1.0) * u.Pinv;
    return v.xyz / v.w;
}
static float3 ssr_project(float3 q, constant SsrParams &u) {
    float4 c = float4(q, 1.0) * u.P;
    float iw = 1.0 / c.w;
    return float3((c.x * iw + u.h.x + 1.0) * 0.5 * u.size.x, (1.0 - c.y * iw - u.h.y) * 0.5 * u.size.y, iw);
}
static float3 ssr_oct(float2 e) {
    float3 n = float3(e, 1.0 - fabs(e.x) - fabs(e.y));
    float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}
static float ssr_class(uint tag, constant SsrParams &u) {
    if (tag == 2u) return u.paint_w;
    if (tag == 3u || tag == 5u || tag == 6u) return 1.0;
    if (tag == 1u) return (u.flags & 1u) ? 1.0 : 0.0;
    return 0.0;
}
static float ssr_f0(uint tag) {
    return tag == 2u ? 0.05 : tag == 3u ? 0.8 : tag == 5u ? 0.04 : tag == 6u ? 0.02 : 0.03;
}
static float ssr_ign(uint2 p) {
    float f = 0.06711056 * float(p.x) + 0.00583715 * float(p.y);
    return fract(52.9829189 * fract(f));
}
static uint2 ssr_px(float2 c, constant SsrParams &u) {
    return uint2(clamp(c, float2(0.0), u.size - 1.0));
}
static SsrSurf ssr_surface(uint2 q, depth2d<float, access::read> z, texture2d<float, access::read> s0,
                           texture2d<float, access::read> s1, constant SsrParams &u) {
    SsrSurf s;
    s.ok = false; s.rough = 1.0; s.f0 = 0.0; s.cw = 0.0; s.n = float3(0.0, 0.0, -1.0); s.pos = float3(0.0, 0.0, 1.0);
    if (u.flags & 2u) {
        float4 b = s1.read(q);   // the tag first: most pixels stop here (one read)
        uint tag = uint(b.r * 255.0 + 0.5), fl = uint(b.b * 255.0 + 0.5);
        float cw = ssr_class(tag, u);
        if (cw <= 0.0 || (fl & 16u) != 0u) return s;
        float d = z.read(q);
        if (d >= 1.0) return s;
        s.pos = ssr_view(float2(q) + 0.5, d, u);
        float4 a = s0.read(q);
        if (fabs(a.w - s.pos.z) > s.pos.z / 512.0) return s;
        s.rough = a.z;
        if (s.rough >= u.rough_max) return s;
        s.n = normalize((float4(ssr_oct(a.xy), 0.0) * u.V).xyz);
        s.f0 = b.g > 0.0 ? b.g : ssr_f0(tag);
        s.cw = cw;
        s.ok = true;
    } else if (u.flags & 64u) {
        float d = z.read(q);
        if (d >= 1.0) return s;
        s.pos = ssr_view(float2(q) + 0.5, d, u);
        int2 m = int2(u.size) - 1;
        int2 iq = int2(q);
        uint2 pl = uint2(clamp(iq - int2(1, 0), int2(0), m)), pr = uint2(clamp(iq + int2(1, 0), int2(0), m));
        uint2 pt = uint2(clamp(iq - int2(0, 1), int2(0), m)), pb = uint2(clamp(iq + int2(0, 1), int2(0), m));
        float3 l = ssr_view(float2(pl) + 0.5, z.read(pl), u), r = ssr_view(float2(pr) + 0.5, z.read(pr), u);
        float3 t = ssr_view(float2(pt) + 0.5, z.read(pt), u), b = ssr_view(float2(pb) + 0.5, z.read(pb), u);
        float3 dx = fabs(r.z - s.pos.z) < fabs(s.pos.z - l.z) ? r - s.pos : s.pos - l;
        float3 dy = fabs(b.z - s.pos.z) < fabs(s.pos.z - t.z) ? b - s.pos : s.pos - t;
        float3 n = normalize(cross(dy, dx));
        s.n = dot(n, s.pos) > 0.0 ? -n : n;
        s.rough = 0.1; s.f0 = 0.04; s.cw = 1.0; s.ok = true;
    }
    return s;
}
static float4 ssr_march(SsrSurf s, uint2 p, depth2d<float, access::read> z, texture2d<float, access::read> s0,
                        constant SsrParams &u) {
    float3 vd = normalize(s.pos);
    if (dot(s.n, vd) >= 0.0) return float4(0.0);
    float3 R = vd - s.n * (2.0 * dot(s.n, vd));
    float L = u.maxdist;
    if (R.z < 0.0) L = min(L, (s.pos.z - 1.5 * u.zn) / -R.z);
    if (L <= 0.0) return float4(0.0);
    float3 a = ssr_project(s.pos, u), b = ssr_project(s.pos + R * L, u);
    float dx = b.x - a.x, dy = b.y - a.y, tmax = 1.0;
    if (dx > 0.0) tmax = min(tmax, (u.size.x - 0.5 - a.x) / dx); else if (dx < 0.0) tmax = min(tmax, (0.5 - a.x) / dx);
    if (dy > 0.0) tmax = min(tmax, (u.size.y - 0.5 - a.y) / dy); else if (dy < 0.0) tmax = min(tmax, (0.5 - a.y) / dy);
    if (tmax <= 0.0) return float4(0.0);
    b = mix(a, b, tmax);
    float len = length(b.xy - a.xy);
    if (len < 1.0) return float4(0.0);
    float t0 = min(1.0, 1.5 / len);
    float dith = (u.flags & 4u) ? fract(ssr_ign(p) + ((u.flags & 128u) ? 0.6180339 * float(u.pad & 1023u) : 0.0)) : 0.5;
    float tp = 0.0, zrp = 1.0 / a.z, th = -1.0;
    for (uint i = 0; i < u.steps; ++i) {
        float t = t0 + (1.0 - t0) * (float(i) + dith) / float(u.steps);
        float3 c = mix(a, b, t);
        float zr = 1.0 / c.z, zs = ssr_lin(z.read(ssr_px(c.xy, u)), u);
        if (zr >= zs && zrp <= zs * (1.0 + u.thick)) { th = t; break; }
        tp = t;
        zrp = zr;
    }
    if (th < 0.0) return float4(0.0);
    float lo = tp, hi = th;
    for (uint k = 0; k < u.refine; ++k) {
        float m = 0.5 * (lo + hi);
        float3 c = mix(a, b, m);
        if (1.0 / c.z >= ssr_lin(z.read(ssr_px(c.xy, u)), u)) hi = m; else lo = m;
    }
    float3 c = mix(a, b, hi);
    uint2 hp = ssr_px(c.xy, u);
    float dh = z.read(hp);
    float zr = 1.0 / c.z, zs = ssr_lin(dh, u), zlo = 1.0 / mix(a, b, lo).z;
    if (dh >= 1.0 || zr - zs > u.thick * zs + fabs(zr - zlo)) return float4(0.0);
    if ((u.flags & 8u) && (u.flags & 2u)) {
        float3 nh = (float4(ssr_oct(s0.read(hp).xy), 0.0) * u.V).xyz;
        if (dot(nh, R) > 0.0) return float4(0.0);
    }
    float2 uv = c.xy / u.size;
    float edge = saturate(min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y)) / 0.1);
    float fcam = smoothstep(-0.4, 0.0, R.z);
    float fdist = 1.0 - smoothstep(0.6, 1.0, hi * tmax);
    float frough = 1.0 - smoothstep(0.6 * u.rough_max, u.rough_max, s.rough);
    return float4(c.xy, edge * fcam * fdist * frough, length(c.xy - a.xy));
}
// half size: source pixel 2g; out RGBA32F (hit px x, y, confidence, distance px). q: [0] receivers [1] hits
// [2] confidence sum x 1024 (one atomic per simdgroup).
kernel void ssr_trace_cs(depth2d<float, access::read> z [[texture(0)]], texture2d<float, access::read> s0 [[texture(1)]],
                         texture2d<float, access::read> s1 [[texture(2)]], texture2d<float, access::write> out [[texture(3)]],
                         constant SsrParams &u [[buffer(0)]], device atomic_uint *q [[buffer(1)]],
                         uint2 g [[thread_position_in_grid]]) {
    bool in = g.x < out.get_width() && g.y < out.get_height();
    uint2 p = min(g * 2u, uint2(u.size) - 1u);
    float4 r = float4(0.0);
    bool ok = false;
    if (in) {
        SsrSurf s = ssr_surface(p, z, s0, s1, u);
        ok = s.ok;
        if (ok) r = ssr_march(s, p, z, s0, u);
        out.write(r, g);
    }
    uint n0 = simd_sum(ok ? 1u : 0u), n1 = simd_sum(r.z > 0.0 ? 1u : 0u), n2 = simd_sum(uint(r.z * 1024.0));
    if (simd_is_first()) {
        if (n0) atomic_fetch_add_explicit(&q[0], n0, memory_order_relaxed);
        if (n1) atomic_fetch_add_explicit(&q[1], n1, memory_order_relaxed);
        if (n2) atomic_fetch_add_explicit(&q[2], n2, memory_order_relaxed);
    }
}
struct SsrQuad { float4 pos [[position]]; };
vertex SsrQuad ssr_vs(uint vid [[vertex_id]]) {
    float2 c = float2(float((vid << 1) & 2u), float(vid & 2u));
    SsrQuad o;
    o.pos = float4(c * 2.0 - 1.0, 0.0, 1.0);
    return o;
}
// full size, into the scene back buffer: (hit colour, w) with blend src-alpha (destination alpha kept)
fragment float4 ssr_resolve_fs(SsrQuad i [[stage_in]], texture2d<float> src [[texture(0)]],
                               texture2d<float, access::read> tr [[texture(1)]], depth2d<float, access::read> z [[texture(2)]],
                               texture2d<float, access::read> s0 [[texture(3)]], texture2d<float, access::read> s1 [[texture(4)]],
                               constant SsrParams &u [[buffer(0)]], sampler sm [[sampler(0)]]) {
    uint2 q = min(uint2(i.pos.xy), uint2(u.size) - 1u);
    bool dbg = (u.flags & 48u) != 0u;
    SsrSurf s = ssr_surface(q, z, s0, s1, u);
    if (!s.ok) return dbg ? float4(0.0, 0.0, 0.0, 1.0) : float4(0.0);
    uint2 hs = uint2(tr.get_width(), tr.get_height());
    uint2 base = q / 2u;
    float2 bx = float2(float(q.x & 1u), float(q.y & 1u)) * 0.5;
    float3 col = float3(0.0);
    float wsum = 0.0, csum = 0.0;
    for (uint k = 0; k < 4u; ++k) {
        uint2 o = uint2(k & 1u, k >> 1);
        float bw = (o.x ? bx.x : 1.0 - bx.x) * (o.y ? bx.y : 1.0 - bx.y);
        if (bw <= 0.0) continue;
        uint2 c = min(base + o, hs - 1u);
        uint2 sp = min(c * 2u, uint2(u.size) - 1u);
        float zc = ssr_lin(z.read(sp), u);
        float w = bw * exp(-fabs(zc - s.pos.z) / (0.01 * s.pos.z));
        if (u.flags & 2u) {
            float3 nc = (float4(ssr_oct(s0.read(sp).xy), 0.0) * u.V).xyz;
            w *= pow(saturate(dot(normalize(nc), s.n)), 8.0);
        }
        float4 t = tr.read(c);
        if (t.z > 0.0) {
            float lod = clamp(log2(max(1.0, 2.0 * s.rough * s.rough * t.w)), 0.0, 4.0);
            col += src.sample(sm, t.xy / u.size, level(lod)).rgb * (w * t.z);
            csum += w * t.z;
        }
        wsum += w;
    }
    float conf = wsum > 1e-4 ? csum / wsum : 0.0;
    float3 hit = csum > 0.0 ? col / csum : float3(0.0);
    float nv = saturate(-dot(s.n, normalize(s.pos)));
    float k5 = pow(1.0 - nv, 5.0);
    float w = saturate(u.strength * conf * (s.f0 + (1.0 - s.f0) * k5) * s.cw);
    if (u.flags & 16u) return float4(float3(0.15 + 0.85 * conf), 1.0);
    if (u.flags & 32u) return float4(hit * w, 1.0);
    return float4(hit, w);
}
)MSL";
}

} // namespace remaster_ssr
