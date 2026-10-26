// Run 15 Wave B WB1b: the surface attachments (review/run14/foundation-design.md 2, review/run15/waveb-plan.md WB1b).
// Pure C++ (no Metal): the class priors, the per-draw object -> world normal rows from the clip constants, the
// fragment parameter block, and the MSL text patch the shader translator applies to a pixel program
// (PixelVariant surf / reactive). The Metal device (d3d9_metal.mm surf_*) attaches S0/S1/S2 at colour 4/5/6 on the
// in-window single-sample scene passes only; with every env unset nothing here runs and no source changes.
//
// Attachments (FndFrame s0..s2; the contract in foundation.h):
//   colour 4 S0 RGBA16Float: xy oct-encoded WORLD normal, z roughness 0..1, w linear view depth (1 / position.w)
//   colour 5 S1 RGBA8Unorm : r MatClass / 255, g F0, b SurfFlag bits / 255, a 0
//   colour 6 S2 R8Unorm    : reactive 0..1, blend MAX
// Cleared at the first surface pass of a window (0 = UNKNOWN, normal (0,0) = +z, depth 0), loaded after.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace remaster_surf {

constexpr int kAttS0 = 4, kAttS1 = 5, kAttS2 = 6;
// SurfFlag bit claimed by WB1b (foundation.h leaves 64 and 128 free): the normal is the depth-derivative facet
// normal (no usable vertex normal: no free varying, no NORMAL0 input, a non-rigid world matrix or no camera yet).
constexpr uint8_t SURF_DERIVED_N = 64;

// MatClass values (foundation.h remaster_fnd::MatClass; repeated so this header stays standalone).
enum : uint8_t { C_UNKNOWN = 0, C_ROAD, C_CAR_PAINT, C_CAR_CHROME, C_CAR_OTHER, C_GLASS, C_WATER, C_WORLD, C_WORLD_NM,
                 C_FOLIAGE, C_SKY, C_COUNT };

struct Prior {
    float rough, f0;
};
// Roughness (linear r, alpha = r^2) and F0 priors per class (foundation-design 2 table; owner-tunable later).
inline Prior prior(uint8_t cls) {
    switch (cls) {
    case C_ROAD: return {0.74f, 0.04f};        // sqrt(0.55): the road sheen's dry alpha0
    case C_CAR_PAINT: return {0.15f, 0.05f};   // F0 replaced per draw by the car's environment strength
    case C_CAR_CHROME: return {0.05f, 0.90f};
    case C_CAR_OTHER: return {0.60f, 0.04f};
    case C_GLASS: return {0.03f, 0.04f};
    case C_WATER: return {0.02f, 0.02f};
    case C_WORLD:
    case C_WORLD_NM: return {0.80f, 0.04f};
    case C_FOLIAGE: return {1.00f, 0.04f};
    case C_SKY: return {1.00f, 0.00f};
    default: return {1.00f, 0.00f};
    }
}
// Car sub-class from the environment strength f = sat(EnvmapMin + EnvmapRange) (vertex c[22].x, c[23].x).
inline uint8_t car_class(float f) {
    return f >= 0.6f ? C_CAR_CHROME : f < 0.2f ? C_CAR_OTHER : C_CAR_PAINT;
}
inline float car_f0(float envmin, float envrange) {
    float f = envmin + envrange;
    return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}

// Fragment buffer(6), five float4: tag = (class / 255, F0, flags / 255, roughness); vt0..vt2 = the rows of V3 (world ->
// view rotation of the game camera, row vectors): the derived view normal goes to world as n_w[i] = dot(n_v, vt_i);
// proj = (1 / P00, 1 / P11, 2 / pass width, 2 / pass height) for the view position of the pixel.
struct D9SurfParams {
    float tag[4];
    float vt0[4], vt1[4], vt2[4];
    float proj[4];
};
static_assert(sizeof(D9SurfParams) == 80, "D9SurfParams is five float4");

// Object -> world normal rows T (row vectors, n_w = n_o . T) from the draw's clip constants (c[a..a+3] = the columns of
// WVP = W x V x P, D3D row-vector convention) and the camera's world -> view V. For a D3D perspective P (x scale sx > 0,
// y scale sy of either sign, w = view z) the upper 3x3 of W x V has columns parallel to c[a].xyz, c[a+1].xyz and
// c[a+3].xyz: normalised, and the y column's sign chosen so det matches det(V3) (W assumed unmirrored), that is
// R = (W x V)3 / scale. Then T = R x V3^T. No P is needed. Returns false (derived normal) when the columns are not
// orthogonal to 2e-3 (non-uniform scale, a projection with off-centre terms) or degenerate. *ortho gets the worst
// |cos| between the columns.
inline bool normal_rows(const float *c, const double V[4][4], float out[12], double *ortho = nullptr) {
    double col[3][3];
    const int src[3] = {0, 1, 3};
    for (int j = 0; j < 3; ++j) {
        const float *q = c + 4 * src[j];
        double l = std::sqrt((double)q[0] * q[0] + (double)q[1] * q[1] + (double)q[2] * q[2]);
        if (!(l > 1e-12) || !std::isfinite(l))
            return false;
        for (int k = 0; k < 3; ++k)
            col[j][k] = q[k] / l;
    }
    double worst = 0.0;
    for (int a = 0; a < 3; ++a)
        for (int b = a + 1; b < 3; ++b) {
            double d = std::fabs(col[a][0] * col[b][0] + col[a][1] * col[b][1] + col[a][2] * col[b][2]);
            worst = d > worst ? d : worst;
        }
    if (ortho)
        *ortho = worst;
    if (worst > 2e-3)
        return false;
    auto det3 = [](const double m[3][3]) {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    };
    double R[3][3], V3[3][3];   // R[k][j] = column j, row k
    for (int k = 0; k < 3; ++k)
        for (int j = 0; j < 3; ++j) {
            R[k][j] = col[j][k];
            V3[k][j] = V[k][j];
        }
    const double dv = det3(V3);
    if (!(std::fabs(dv) > 1e-6))
        return false;
    if ((det3(R) < 0.0) != (dv < 0.0))
        for (int k = 0; k < 3; ++k)
            R[k][1] = -R[k][1];
    for (int k = 0; k < 3; ++k)
        for (int i = 0; i < 3; ++i) {
            double s = 0.0;
            for (int j = 0; j < 3; ++j)
                s += R[k][j] * V3[i][j];   // T[k][i] = sum_j R[k][j] V3^T[j][i]
            out[4 * k + i] = (float)s;
        }
    out[3] = out[7] = out[11] = 0.0f;
    return true;
}

// The MSL helpers (inserted once before the fragment function of a surface variant).
static const char kSurfMsl[] =
    "struct D9SurfParams { float4 tag; float4 vt0; float4 vt1; float4 vt2; float4 proj; };\n"
    "static float2 d9s_oct(float3 n) {   // octahedral encode (remaster_fnd::oct_encode)\n"
    "    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1.0e-8);\n"
    "    float2 e = n.xy;\n"
    "    if (n.z < 0.0)\n"
    "        e = (1.0 - abs(n.yx)) * select(float2(-1.0), float2(1.0), n.xy >= 0.0);\n"
    "    return e;\n"
    "}\n";

// The pixel-program patch: `surf` 0 = no S0/S1, 1 = S0/S1 with the derived (depth-derivative) normal, 4..7 = S0/S1
// with the interpolated world normal in t<surf> (the VertexVariant surf slot); `reactive` 0 = no S2, 1 = S2 = output
// alpha, 2 = max(rgb), 3 = max(rgb) x alpha (additive with SRCALPHA). Applied last (after every other patch and the
// HDR clamp) to the final source: the output struct gains s0 [[color(4)]] / s1 [[color(5)]] / s2 [[color(6)]], the
// signature gains the D9SurfParams block at buffer(6) (surf only), and the writes go right before `return o;`.
// False (the caller then drops the variant) if the source does not have the expected shape.
inline bool surf_ps_patch(std::string *s, uint8_t surf, uint8_t reactive) {
    if (!surf && !reactive)
        return true;
    if ((surf > 1 && (surf < 4 || surf > 7)) || reactive > 3)
        return false;
    const std::string kStruct = "struct D9PSOut {\n", kFn = "fragment D9PSOut ps_main(", kBody = ") {\n    float4 r[32] = {};\n",
                      kRet = "    return o;\n}\n";
    const size_t st = s->find(kStruct), fn = s->find(kFn);
    if (st == std::string::npos || fn == std::string::npos || s->find(kStruct, st + 1) != std::string::npos ||
        s->find(kFn, fn + 1) != std::string::npos || s->find("[[color(4)]]") != std::string::npos ||
        s->find("buffer(6)") != std::string::npos)
        return false;
    const size_t body = s->find(kBody, fn), ret = s->rfind(kRet);
    if (body == std::string::npos || ret == std::string::npos || ret < body || s->find(kRet, body) != ret)
        return false;
    std::string w;
    if (surf) {
        w += "    {   // Wave B WB1b surface: S0 (world normal oct, roughness, linear depth), S1 (tag, F0, flags)\n"
             "        float d9s_z = 1.0 / vin.pos.w;\n";
        if (surf == 1)
            w += "        float3 d9s_p = float3((vin.pos.x * SFP.proj.z - 1.0) * SFP.proj.x,\n"
                 "                              (1.0 - vin.pos.y * SFP.proj.w) * SFP.proj.y, 1.0) * d9s_z;\n"
                 "        float3 d9s_v = cross(dfdx(d9s_p), dfdy(d9s_p));\n"
                 "        if (dot(d9s_v, d9s_p) > 0.0)\n"
                 "            d9s_v = -d9s_v;\n"
                 "        float3 d9s_n = float3(dot(d9s_v, SFP.vt0.xyz), dot(d9s_v, SFP.vt1.xyz), dot(d9s_v, SFP.vt2.xyz));\n";
        else
            w += "        float3 d9s_n = vin.t" + std::to_string(surf) + ".xyz;\n";
        // TEST ONLY (run 21, the W1-stale fail-on-purpose control): RECOMP_REMASTER_FOUNDATION_STALE_TEST=1 writes S0.w
        // 1 % long, so every surface pixel must read stale in views 5 and 9.
        static const bool stale_test = [] { const char *e = getenv("RECOMP_REMASTER_FOUNDATION_STALE_TEST"); return e && *e == '1'; }();
        w += "        d9s_n *= rsqrt(max(dot(d9s_n, d9s_n), 1.0e-20));\n";
        w += stale_test ? "        o.s0 = float4(d9s_oct(d9s_n), SFP.tag.w, d9s_z * 1.01);\n"
                        : "        o.s0 = float4(d9s_oct(d9s_n), SFP.tag.w, d9s_z);\n";
        w += "        o.s1 = float4(SFP.tag.xyz, 0.0);\n"
             "    }\n";
    }
    if (reactive)
        w += std::string("    o.s2 = float4(saturate(") +
             (reactive == 1   ? "o.c0.a"
              : reactive == 2 ? "max(o.c0.r, max(o.c0.g, o.c0.b))"
                              : "max(o.c0.r, max(o.c0.g, o.c0.b)) * o.c0.a") +
             "));   // Wave B WB1b reactive (S2, blend max)\n";
    s->insert(ret, w);
    if (surf)
        s->insert(body, ",\n                         constant D9SurfParams &SFP [[buffer(6)]]");
    std::string fields;
    if (surf)
        fields += "    float4 s0 [[color(4)]];\n    float4 s1 [[color(5)]];\n";
    if (reactive)
        fields += "    float4 s2 [[color(6)]];\n";
    s->insert(st + kStruct.size(), fields);
    if (surf)
        s->insert(s->find(kFn), kSurfMsl);
    return true;
}

} // namespace remaster_surf
