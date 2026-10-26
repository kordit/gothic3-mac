// foundation.h - Remaster run 14 foundation F1/F3 (review/run14/foundation-design.md 1 and 3; the shared camera and
// motion contract for B1 MetalFX temporal, B2 SSR and B3 RT): the pure C++ parts, no Metal types. Included once, by
// d3d9_metal.mm; unit-tested in dx/tests/d3d9_tests.cpp (test_foundation).
//
// Conventions (OBS d3d9_shadergen.cpp fef_clip_shadow, the mod's clean_sun.h): a vertex shader writes oPos.k =
// dot(v, c[clip + k]), so the 4 constant registers hold the columns of the row-vector matrix the game built:
// clip = p x M with M[i][j] = c[clip + j][i] (from_const_block). Every matrix here is row-vector (p x A x B applies A
// first), in double. The camera's V_g (eView 1 camera +0x00..+0x3c) is row-vector world -> view, view space x right,
// y DOWN, z forward; P = V_g^-1 x VP_k is then the game's D3D projection (P[2][3] = 1, depth 0..1).
//
// Pixels: Metal pixel (i, j) (render pixels, the scene backing's size w x h) shows Direct3D NDC
//   nx = 2 (i + 0.5) / w - 1 - hx,   ny = 1 - 2 (j + 0.5) / h - hy
// where (hx, hy) is the draw's halfpix ((63/64) / w, -(63/64) / h, d3d9_metal.mm draw) plus the jitter, if any.
// Motion vectors are current -> previous in render pixels (x right, y down), from the UNJITTERED centre.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace remaster_fnd {

struct M4 {
    double m[4][4];
};
inline M4 identity() {
    M4 r{};
    for (int i = 0; i < 4; ++i)
        r.m[i][i] = 1.0;
    return r;
}
inline M4 mul(const M4 &a, const M4 &b) {   // p x a x b
    M4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k)
                s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}
// 16 floats, one row each 4 (the game's camera matrix in memory).
inline M4 from_rows(const float *f) {
    M4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = f[4 * i + j];
    return r;
}
// The 4 vertex constant registers c[k..k+3] as the shader uses them (oPos.j = dot(v, c[k + j])).
inline M4 from_const_block(const float *c) {
    M4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = c[4 * j + i];
    return r;
}
inline void xform(const double v[4], const M4 &a, double out[4]) {   // out = v x a
    for (int j = 0; j < 4; ++j)
        out[j] = v[0] * a.m[0][j] + v[1] * a.m[1][j] + v[2] * a.m[2][j] + v[3] * a.m[3][j];
}
// Gauss-Jordan with partial pivoting, scaled singularity test. False when (near) singular or not finite.
inline bool inverse(const M4 &a, M4 *out) {
    double m[4][8], scale = 1.0;
    for (int i = 0; i < 4; ++i) {
        double n = 0.0;
        for (int j = 0; j < 4; ++j) {
            m[i][j] = a.m[i][j];
            m[i][4 + j] = i == j ? 1.0 : 0.0;
            n += m[i][j] * m[i][j];
        }
        scale *= std::sqrt(n);
    }
    double det = 1.0;
    for (int c = 0; c < 4; ++c) {
        int piv = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(m[r][c]) > std::fabs(m[piv][c]))
                piv = r;
        if (!(std::fabs(m[piv][c]) > 0.0))
            return false;
        if (piv != c)
            for (int j = 0; j < 8; ++j)
                std::swap(m[c][j], m[piv][j]);
        det *= m[c][c];
        const double inv = 1.0 / m[c][c];
        for (int j = 0; j < 8; ++j)
            m[c][j] *= inv;
        for (int r = 0; r < 4; ++r)
            if (r != c && m[r][c] != 0.0) {
                const double f = m[r][c];
                for (int j = 0; j < 8; ++j)
                    m[r][j] -= f * m[c][j];
            }
    }
    if (!(scale > 0.0) || !(std::fabs(det) > 1e-12 * scale))
        return false;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            if (!std::isfinite(m[i][4 + j]))
                return false;
            out->m[i][j] = m[i][4 + j];
        }
    return true;
}
inline double max_abs_diff(const M4 &a, const M4 &b) {
    double d = 0.0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            d = std::max(d, std::fabs(a.m[i][j] - b.m[i][j]));
    return d;
}
inline uint64_t block_hash(const float *c16) {   // FNV-1a over the 64 bytes
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *p = (const uint8_t *)c16;
    for (int i = 0; i < 64; ++i)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h ? h : 1;
}

// ---- F1 K: the plurality constant block of the frame's WORLD draws in the scene window -------------------------
// add() per WORLD draw whose vertex program writes oPos as 4 dp4 with one constant block (vertex_clip_regs);
// world() per WORLD draw (analysed or not). pick(): the block with the most draws; a tie goes to the block seen first
// (counted). Valid with >= min_draws draws and >= min_share of the WORLD draws.
struct Plurality {
    struct Entry {
        uint64_t hash;
        uint32_t count, first;
        float block[16];
    };
    std::vector<Entry> e;
    std::unordered_map<uint64_t, uint32_t> at;
    uint32_t world_draws = 0, captured = 0;
    void reset() {
        e.clear();
        at.clear();
        world_draws = captured = 0;
    }
    void world() { ++world_draws; }
    void add(const float *block) {
        const uint64_t h = block_hash(block);
        auto it = at.find(h);
        if (it == at.end()) {
            Entry n;
            n.hash = h;
            n.count = 0;
            n.first = captured;
            memcpy(n.block, block, sizeof n.block);
            it = at.emplace(h, (uint32_t)e.size()).first;
            e.push_back(n);
        }
        ++e[it->second].count;
        ++captured;
    }
    struct Result {
        bool valid = false, tie = false;
        const Entry *best = nullptr;
        uint32_t count = 0, world = 0, distinct = 0;
        double share = 0.0;
    };
    Result pick(uint32_t min_draws = 8, double min_share = 0.25) const {
        Result r;
        r.world = world_draws;
        r.distinct = (uint32_t)e.size();
        for (const Entry &x : e)
            if (!r.best || x.count > r.best->count || (x.count == r.best->count && x.first < r.best->first))
                r.best = &x;
        if (!r.best)
            return r;
        for (const Entry &x : e)
            r.tie = r.tie || (&x != r.best && x.count == r.best->count);
        r.count = r.best->count;
        r.share = world_draws ? (double)r.count / world_draws : 0.0;
        r.valid = r.count >= min_draws && r.share >= min_share;
        return r;
    }
    // WORLD draws whose object transform W = M x VP^-1 is the identity within tol (design 1: world scenery, matrix 0).
    // CONF14B: an absolute tol on W fails on rounding alone far from the origin (W's row 3 carries |t| x 1e-7); the kit
    // now counts same_block() members of the picked cluster instead (Pick::members). Kept for the tests.
    uint32_t identity_draws(const M4 &vp_inv, double tol) const {
        uint32_t n = 0;
        const M4 I = identity();
        for (const Entry &x : e)
            if (max_abs_diff(mul(from_const_block(x.block), vp_inv), I) < tol)
                n += x.count;
        return n;
    }
};
// Two constant blocks are the same matrix within float rounding (CONF14B fix: the game builds WVP per object, so
// world-space scenery gives VP with last-bit differences that an exact hash splits into many "distinct" blocks).
// Per row i of the row-vector matrix (rows 0-2 rotation x P, row 3 the translation x P, up to |t| x |P| large):
// |a - b| <= rel x the row's largest |element|. A 5 m instance offset changes row 3 by ~5 x |P|: far above 1e-5 x |t P|
// for any |t| < 100 km.
inline bool same_block(const float *a, const float *b, double rel = 1e-5) {
    for (int i = 0; i < 4; ++i) {
        double sc = 0.0;
        for (int j = 0; j < 4; ++j)
            sc = std::max({sc, std::fabs((double)a[4 * j + i]), std::fabs((double)b[4 * j + i])});
        const double tol = rel * sc + 1e-12;
        for (int j = 0; j < 4; ++j)
            if (std::fabs((double)a[4 * j + i] - (double)b[4 * j + i]) > tol)
                return false;
    }
    return true;
}

// ---- F1 G/K cross-check: P = V_g^-1 x VP_k has the D3D projection form --------------------------------------------
// fov: the camera's full vertical FOV (radians); aspect: scene width / height; near/far: camera +0xbc/+0xc0; trans:
// |camera position| (metres) - the float constants' rounding grows with it, so the off-diagonal tolerance does too
// (DER: 4 float ulps of the largest product). rel: the agreement tolerance (design: 1e-4).
struct ProjCheck {
    bool form = false, fov_ok = false, aspect_ok = false, near_ok = false, far_ok = false, reversed = false;
    bool y_down = false;   // P[1][1] < 0: view y down -> clip y up (the game's camera, clean_sun.h P0)
    double max_off = 0.0, tol_off = 0.0, xs = 0.0, ys = 0.0, near = 0.0, far = 0.0, fov = 0.0;
    // CONF14B (lights, 2208 of 2208 checked frames): P had the projection form to 2.3e-4 of the tolerance, near 0.5,
    // but |P11| = 1.2122 / tan(fov_u16 / 2), P00 = |P11| / 1.6926 and far ~14980: the core mod's widescreen fix scales
    // the game's projection (video_modes.h apply_resolution: g_hor = (4/3) / aspect / 1.047, g_vert 1.21 or 1.27) and the
    // projection's far plane is not camera +0xc0. So FOV, aspect and far are information (logged with the measured
    // ratios); agreement = the same camera: the form (16 constraints: a VP_k of another view or an instanced block
    // fails it), near and the depth direction. P itself (the GPU's) is what the consumers get.
    bool agree() const { return form && near_ok && !reversed; }
};
inline ProjCheck check_projection(const M4 &P, double fov, double aspect, double near_g, double far_g, double trans,
                                  double rel = 1e-4) {
    ProjCheck c;
    const double pmax = std::max({std::fabs(P.m[0][0]), std::fabs(P.m[1][1]), std::fabs(P.m[2][2]), 1.0});
    const double eps = 4.0 * 1.1920929e-7 * pmax;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            if (i == j && i < 3)
                continue;
            if ((i == 2 && j == 3) || (i == 3 && j == 2))
                continue;
            const double tol = 1e-5 + eps * (i == 3 ? 1.0 + trans : 1.0);
            const double d = std::fabs(P.m[i][j]) / tol;
            if (d > c.max_off) {
                c.max_off = d;
                c.tol_off = tol;
            }
        }
    c.form = c.max_off <= 1.0 && std::fabs(P.m[2][3] - 1.0) < 1e-5 + eps;
    c.xs = P.m[0][0];
    c.ys = P.m[1][1];
    c.y_down = c.ys < 0.0;
    c.reversed = !(P.m[2][2] > 0.0);
    const double ay = std::fabs(c.ys);
    c.fov = ay > 0.0 ? 2.0 * std::atan(1.0 / ay) : 0.0;
    const double want = fov > 0.0 ? 1.0 / std::tan(0.5 * fov) : 0.0;
    c.fov_ok = want > 0.0 && std::fabs(ay - want) <= rel * want;
    c.aspect_ok = aspect > 0.0 && std::fabs(std::fabs(c.xs) - ay / aspect) <= rel * ay / aspect;
    if (P.m[2][2] != 0.0 && P.m[2][2] != 1.0) {
        c.near = -P.m[3][2] / P.m[2][2];
        c.far = P.m[3][2] / (1.0 - P.m[2][2]);
    }
    // near from P[3][2]/P[2][2] is well conditioned; far from 1 - P[2][2] (about -5e-5 at 0.5/10000) loses about
    // 1e-3 relative to the float rounding of P[2][2] (DER), so far gets 2e-2.
    c.near_ok = near_g > 0.0 && std::fabs(c.near - near_g) <= 1e-3 * near_g;
    c.far_ok = far_g > 0.0 && std::fabs(c.far - far_g) <= 2e-2 * far_g;
    return c;
}
// ---- F1 K with G: the pick over clusters of equal blocks ----------------------------------------------------------
// Clusters (same_block) of the most-drawn distinct blocks (at most 64 entries looked at, by count). With the camera
// (vinv_g = V_g^-1), the largest cluster with >= min_draws draws whose P = V_g^-1 x block has the projection form is
// VP_k (the camera's own matrix, whatever share the instanced draws have: CONF14B Rosewood had 0 valid frames under the
// share rule). Without the camera, or when no cluster has the form: the largest cluster, valid with >= min_share of
// the WORLD draws (the design's plurality rule).
struct Pick {
    bool valid = false, tie = false, by_camera = false, form_any = false;
    const float *block = nullptr;
    uint32_t count = 0, world = 0, distinct = 0, clusters = 0, rank = 0;   // rank: 0 = the largest cluster
    uint32_t top[3] = {0, 0, 0};
    double share = 0.0;
};
inline Pick pick_clusters(const Plurality &pl, uint32_t min_draws, double min_share, const M4 *vinv_g, double trans) {
    Pick r;
    r.world = pl.world_draws;
    r.distinct = (uint32_t)pl.e.size();
    std::vector<const Plurality::Entry *> order;
    order.reserve(pl.e.size());
    for (const Plurality::Entry &x : pl.e)
        order.push_back(&x);
    std::sort(order.begin(), order.end(), [](const Plurality::Entry *a, const Plurality::Entry *b) {
        return a->count != b->count ? a->count > b->count : a->first < b->first;
    });
    if (order.size() > 64)
        order.resize(64);
    struct C {
        const Plurality::Entry *rep;
        uint32_t count, first;
    };
    std::vector<C> cl;
    for (const Plurality::Entry *x : order) {
        bool joined = false;
        for (C &c : cl)
            if (same_block(c.rep->block, x->block)) {
                c.count += x->count;
                c.first = std::min(c.first, x->first);
                joined = true;
                break;
            }
        if (!joined)
            cl.push_back({x, x->count, x->first});
    }
    std::stable_sort(cl.begin(), cl.end(), [](const C &a, const C &b) {
        return a.count != b.count ? a.count > b.count : a.first < b.first;
    });
    r.clusters = (uint32_t)cl.size();
    for (size_t i = 0; i < cl.size() && i < 3; ++i)
        r.top[i] = cl[i].count;
    if (cl.empty())
        return r;
    r.tie = cl.size() > 1 && cl[1].count == cl[0].count;
    size_t pickd = 0;
    if (vinv_g)
        for (size_t i = 0; i < cl.size(); ++i) {
            if (cl[i].count < min_draws)
                break;
            if (check_projection(mul(*vinv_g, from_const_block(cl[i].rep->block)), 0, 0, 0, 0, trans).form) {
                r.by_camera = r.form_any = true;
                pickd = i;
                break;
            }
        }
    r.rank = (uint32_t)pickd;
    r.block = cl[pickd].rep->block;
    r.count = cl[pickd].count;
    r.share = r.world ? (double)r.count / r.world : 0.0;
    r.valid = r.by_camera || (r.count >= min_draws && r.share >= min_share);
    return r;
}
// Camera position of a row-vector world -> view matrix (rigid): c x R + t = 0 -> c = -t x R^T.
inline void camera_position(const M4 &V, double out[3]) {
    for (int k = 0; k < 3; ++k)
        out[k] = -(V.m[3][0] * V.m[k][0] + V.m[3][1] * V.m[k][1] + V.m[3][2] * V.m[k][2]);
}
// The same view with the translation dropped (the sky: rotation-only reprojection).
inline M4 rotation_only(const M4 &V) {
    M4 r = V;
    r.m[3][0] = r.m[3][1] = r.m[3][2] = 0.0;
    r.m[3][3] = 1.0;
    return r;
}

// ---- F3 reprojection ---------------------------------------------------------------------------------------------
// R maps the current clip position (nx, ny, depth, 1) to the previous frame's clip position: world = clip x VP_k^-1,
// prev = world x VP_prev, so R = VP_k^-1 x VP_prev (row-vector; the design's "VP_prev x VP_k^-1" in column order).
// Formed in double on the CPU; on the GPU only R (no world coordinates) is used.
inline bool reprojection(const M4 &vp_cur, const M4 &vp_prev, M4 *R) {
    M4 inv;
    if (!inverse(vp_cur, &inv))
        return false;
    *R = mul(inv, vp_prev);
    return true;
}
// Self test (design 1): R with VP_prev := VP must map the 4 NDC corners at depths 0.1/0.5/0.99 to themselves.
// Returns the largest NDC error (inf when VP is singular).
inline double self_test(const M4 &vp) {
    M4 R;
    if (!reprojection(vp, vp, &R))
        return INFINITY;
    double e = 0.0;
    for (double z : {0.1, 0.5, 0.99})
        for (int k = 0; k < 4; ++k) {
            const double v[4] = {k & 1 ? 1.0 : -1.0, k & 2 ? 1.0 : -1.0, z, 1.0};
            double o[4];
            xform(v, R, o);
            if (!(std::fabs(o[3]) > 0.0))
                return INFINITY;
            for (int a = 0; a < 3; ++a)
                e = std::max(e, std::fabs(o[a] / o[3] - v[a]));
        }
    return e;
}
// The NDC a Metal pixel centre shows (the header's mapping; hx/hy the unjittered halfpix).
inline void pixel_ndc(double i, double j, uint32_t w, uint32_t h, double hx, double hy, double *nx, double *ny) {
    *nx = 2.0 * (i + 0.5) / w - 1.0 - hx;
    *ny = 1.0 - 2.0 * (j + 0.5) / h - hy;
}
// The CPU reference of the motion kernel (fnd_motion_cs in d3d9_metal.mm): current -> previous in render pixels.
// False when the previous position is behind the camera (w <= 0): the kernel writes 0 and counts it.
inline bool motion(const M4 &R, double i, double j, double depth, uint32_t w, uint32_t h, double hx, double hy,
                   double *mvx, double *mvy, double *prev_depth = nullptr) {
    double nx, ny;
    pixel_ndc(i, j, w, h, hx, hy, &nx, &ny);
    const double v[4] = {nx, ny, depth, 1.0};
    double o[4];
    xform(v, R, o);
    if (!(o[3] > 1e-12)) {
        *mvx = *mvy = 0.0;
        return false;
    }
    *mvx = (o[0] / o[3] - nx) * 0.5 * w;
    *mvy = -(o[1] / o[3] - ny) * 0.5 * h;
    if (prev_depth)
        *prev_depth = o[2] / o[3];
    return true;
}
// Linear view depth of a stored depth (0..1, not reversed) for near/far n, f.
inline double linear_depth(double d, double n, double f) {
    return n * f / (f - d * (f - n));
}

// ---- F3 jitter: Halton(2,3) ----------------------------------------------------------------------------------------
inline double halton(uint32_t i, uint32_t b) {
    double f = 1.0, r = 0.0;
    while (i) {
        f /= b;
        r += f * (i % b);
        i /= b;
    }
    return r;
}
// 8 x f^2 phases (f = the upscale factor, 1 for AA only), 8..128.
inline uint32_t jitter_phases(double factor) {
    const double f = factor >= 1.0 ? factor : 1.0;
    return (uint32_t)std::min(128.0, std::max(8.0, std::round(8.0 * f * f)));
}
// Phase k (0-based) in render pixels, -0.5..0.5 (index k + 1: Halton index 0 is the origin).
inline void jitter(uint32_t k, double *jx, double *jy) {
    *jx = halton(k + 1, 2) - 0.5;
    *jy = halton(k + 1, 3) - 0.5;
}
// The halfpix addition for a jitter of (jx, jy) render pixels (x right, y down) in a viewport of vw x vh render pixels:
// pos.xy += halfpix x w, and one render pixel is 2 / vw in NDC (y up in NDC, so y down is negative).
inline void jitter_halfpix(double jx, double jy, double vw, double vh, float *dx, float *dy) {
    *dx = vw > 0.0 ? (float)(2.0 * jx / vw) : 0.0f;
    *dy = vh > 0.0 ? (float)(-2.0 * jy / vh) : 0.0f;
}

// ---- F3 history resets (design 3) -----------------------------------------------------------------------------------
enum : uint32_t {
    RESET_CUT = 1,         // the cut-scene camera flag (008f9b28 low byte)
    RESET_JUMP = 2,        // camera moved > 10 m in a frame
    RESET_FOV = 4,         // vertical FOV changed > 2 degrees
    RESET_VP_INVALID = 8,  // no valid VP_k this frame
    RESET_DISAGREE = 16,   // G and K disagree (check_projection)
    RESET_GAP = 32,        // the previous frame had no scene window (first frame, front end <-> world, loads)
    RESET_PAUSE = 64,      // the game frame did not advance
    RESET_RESIZE = 128,    // Reset / the back buffer or scene depth redefined
    RESET_NO_CAMERA = 256, // no camera from the mod this frame
    RESET_COUNT = 9
};
struct Camera {
    bool have = false;
    M4 V{};
    double fov = 0.0, near = 0.0, far = 0.0;
    uint32_t flags = 0, mover = 0, frame = 0;
};
inline uint32_t camera_resets(const Camera &prev, const Camera &cur) {
    uint32_t r = 0;
    if (!cur.have)
        return RESET_NO_CAMERA;
    if (cur.flags & 1u)
        r |= RESET_CUT;
    if (prev.have) {
        double a[3], b[3];
        camera_position(prev.V, a);
        camera_position(cur.V, b);
        const double d = std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) +
                                   (a[2] - b[2]) * (a[2] - b[2]));
        if (!(d <= 10.0))
            r |= RESET_JUMP;
        if (!(std::fabs(cur.fov - prev.fov) <= 2.0 * 3.14159265358979323846 / 180.0))
            r |= RESET_FOV;
        if (cur.frame == prev.frame)
            r |= RESET_PAUSE;
    }
    return r;
}
// Run 15 Wave B WB1a: a still camera (the parked gate's per-frame lines `reset 0x0 still 1`): the game frame advanced
// and the camera moved < 1 mm and turned < 1e-5 (rotation entries). Without both cameras: not still.
inline bool camera_still(const Camera &prev, const Camera &cur) {
    if (!prev.have || !cur.have || cur.frame == prev.frame)
        return false;
    double a[3], b[3];
    camera_position(prev.V, a);
    camera_position(cur.V, b);
    const double d = std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
    double rot = 0.0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            rot = std::max(rot, std::fabs(cur.V.m[i][j] - prev.V.m[i][j]));
    return d < 0.001 && rot < 1e-5;
}
// Run 15 Wave B WB1a H-lag (TEST counters, review/run15/waveb-plan.md WB1a 2): the three candidate reprojections the
// motion kernel scores against the previous depth: R = VP_k^-1 x VP_prev (the vector source), R2 = VP_k^-1 x VP_(n-2)
// (the depth or VP one frame off) and RG = (V_g P)^-1 x (V_g,prev P_prev) with one P for both frames (the caller passes
// P_prev twice; this frame's own P = V_g^-1 x VP_k would make RG = R) (the mod camera instead of the picked block:
// a stale plurality block). Returns which of them were formed (bits 1 R, 2 R2, 4 RG).
struct LagInputs {
    M4 vp{}, vp_prev{}, vp_prev2{}, V{}, P{}, V_prev{}, P_prev{};
    bool prev = false, prev2 = false, cam = false;   // vp_prev / vp_prev2 valid; V/P and V_prev/P_prev valid
};
inline uint32_t lag_candidates(const LagInputs &in, M4 *R, M4 *R2, M4 *RG) {
    uint32_t got = 0;
    if (in.prev && reprojection(in.vp, in.vp_prev, R))
        got |= 1u;
    if (in.prev2 && reprojection(in.vp, in.vp_prev2, R2))
        got |= 2u;
    if (in.cam && reprojection(mul(in.V, in.P), mul(in.V_prev, in.P_prev), RG))
        got |= 4u;
    return got;
}

// ---- Run 15 Wave B contract (review/run15/waveb-plan.md P0) -------------------------------------------------------
// FndFrame: what the foundation hands its consumers (WB2 MetalFX temporal, WB3 SSR, later RT) for the scene window
// that just closed. WB1a fills it at the end of fnd_end (d3d9_metal.mm); consumers only read it, between fnd_end and
// upscale_end (scene_split's END order: fnd_end(); ssr_apply(); upscale_end(true);). valid == false: skip and count.
// Texture ids are non-owning (__bridge) pointers to the id<MTLTexture> the Metal device holds in fnd_tex_; nullptr
// when that attachment does not exist this frame (surface off, MSAA on, ...).
enum MatClass : uint8_t {   // S1.r tag (surface attachments F2, WB1b): the material class of the pixel's last write
    MAT_UNKNOWN = 0,
    MAT_ROAD = 1,
    MAT_CAR_PAINT = 2,
    MAT_CAR_CHROME = 3,
    MAT_CAR_OTHER = 4,   // tyres, lights, interior, undercarriage
    MAT_GLASS = 5,
    MAT_WATER = 6,
    MAT_WORLD = 7,       // world geometry with no normal map
    MAT_WORLD_NM = 8,    // world geometry with a normal map
    MAT_FOLIAGE = 9,
    MAT_SKY = 10,
    MAT_COUNT = 11
};
enum SurfFlag : uint8_t {   // S1.b flag bits
    SURF_DYNAMIC = 1,         // moving object (cars, traffic, props): its own motion is not in the camera reprojection
    SURF_ALPHA_TEST = 2,      // alpha-tested (foliage, fences): thin, unreliable history
    SURF_CAMERA_LOCKED = 4,   // the player's car under the chase camera (the q[16] class)
    SURF_INSTANCED_W = 8,     // world matrix not recoverable (instanced W, debug view 10)
    SURF_EMISSIVE = 16,       // lights, signs: no reflection receiver
    SURF_DECAL = 32,          // decal / overlay draw: the tag of the surface below
    // 64, 128 free (WB1b may claim them; state it in wb-wb1b.md)
};
// S0 RGBA16F: xy oct-encoded WORLD normal (oct_encode), z roughness 0..1, w linear view depth (the pass's own, for the
// stale test |S0.w - linear depth| < 1/512 rel). S1 RGBA8: r MatClass, g F0 (0..1), b SurfFlag bits, a reserved 0.
// S2 R8: reactive 0..1 (MetalFX reactive mask). motion RG16F: current -> previous, render pixels, x right y down.
struct FndFrame {
    bool valid = false;
    uint32_t reset = 0;          // RESET_* bits of this window (0: the history may be used)
    uint64_t frame = 0;          // the foundation's window serial (fnd_.windows)
    uint32_t w = 0, h = 0;       // render (scene backing) pixels
    double jx = 0.0, jy = 0.0;   // this window's jitter in render pixels (0 with jitter off)
    M4 vp{}, vp_inv{};           // VP_k (the picked world -> clip) and its inverse
    M4 V{}, P{};                 // V_g (world -> view, y down) and the D3D projection P = V_g^-1 x VP_k
    double zn = 0.0, zf = 0.0;   // P's near / far (view units)
    void *motion = nullptr;      // id<MTLTexture> RG16F
    void *s0 = nullptr;          // id<MTLTexture> RGBA16F normal oct / roughness / linear depth
    void *s1 = nullptr;          // id<MTLTexture> RGBA8 tag / F0 / flags
    void *s2 = nullptr;          // id<MTLTexture> R8 reactive
    void *scene = nullptr;       // id<MTLTexture> the single-sample scene colour (the window's back buffer backing)
    void *depth = nullptr;       // id<MTLTexture> the scene depth
};
// Octahedral normal encoding (C++ reference; WB1b ports it to MSL for S0.xy): unit n -> [-1, 1]^2 and back.
inline void oct_encode(const double n[3], double *u, double *v) {
    double s = std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]);
    if (s <= 0.0) {
        *u = *v = 0.0;
        return;
    }
    double x = n[0] / s, y = n[1] / s;
    if (n[2] < 0.0) {
        double ox = (1.0 - std::fabs(y)) * (x >= 0.0 ? 1.0 : -1.0);
        double oy = (1.0 - std::fabs(x)) * (y >= 0.0 ? 1.0 : -1.0);
        x = ox;
        y = oy;
    }
    *u = x;
    *v = y;
}
inline void oct_decode(double u, double v, double n[3]) {
    double x = u, y = v, z = 1.0 - std::fabs(u) - std::fabs(v);
    if (z < 0.0) {
        double t = -z;
        x += x >= 0.0 ? -t : t;
        y += y >= 0.0 ? -t : t;
    }
    double l = std::sqrt(x * x + y * y + z * z);
    n[0] = x / l;
    n[1] = y / l;
    n[2] = z / l;
}

} // namespace remaster_fnd
