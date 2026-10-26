// R3 run 22 (TEST RECOMP_REMASTER_UPSCALE_TEMPORAL_SHARPEN=<stops>; review/run22/sharpen-design.md): the sharpen on
// the MetalFX temporal output, our own MSL of the published RCAS idea (FSR2 docs super-resolution-temporal.md
// 806-840, MIT; no code copied). MetalFX temporal super-resolves but its resolve is low-pass (run 22 bench: error vs
// the ideal 0.0157 against a single-frame bicubic's 0.0482, energy 0.654 of the ideal; this sharpen at 2 stops: error
// 0.0090, energy 0.91). One string, shared by d3d9_metal.mm's utility library (the compose pass) and
// harness/tools/mfx_bench.mm (temporal-sharpen), so the bench scores the kit's own kernel.
// Cross 5-tap: the negative lobe is the largest that keeps the result inside the taps' min / max in every channel (one
// lobe for rgb: channel ratios kept, no fringes), capped at -0.1875 and scaled by 2^-stops; DC gain 1. HDR (float
// scene): the taps are tonemapped c / (1 + c) for the limiter and the result mapped back (monotone, so containment
// survives); the 8-bit scene is display-referred already.
#pragma once

#define REMASTER_SHARPEN_MSL R"MSL(
static float3 rs_tm(float3 c, bool hdr) { return hdr ? c / (1.0 + max(c, 0.0)) : c; }
static float3 rs_itm(float3 c, bool hdr) { return hdr ? c / max(1.0 - c, 1.0e-4) : c; }
static float4 rcas5(texture2d<float> t, uint2 p, float stops, bool hdr) {
    const uint2 m = uint2(t.get_width() - 1, t.get_height() - 1);
    const float4 e4 = t.read(p);
    const float3 e = rs_tm(e4.rgb, hdr);
    const float3 n = rs_tm(t.read(uint2(p.x, max(p.y, 1u) - 1u)).rgb, hdr);
    const float3 s = rs_tm(t.read(uint2(p.x, min(p.y + 1u, m.y))).rgb, hdr);
    const float3 w = rs_tm(t.read(uint2(max(p.x, 1u) - 1u, p.y)).rgb, hdr);
    const float3 a = rs_tm(t.read(uint2(min(p.x + 1u, m.x), p.y)).rgb, hdr);
    const float3 mn = min(min(min(n, s), min(w, a)), e), mx = max(max(max(n, s), max(w, a)), e);
    const float3 hit_min = mn / max(4.0 * mx, 1.0e-6);
    const float3 hit_max = (1.0 - mx) / min(4.0 * mn - 4.0, -1.0e-6);
    float lobe = max(max(-hit_min.r, max(-hit_min.g, -hit_min.b)), max(hit_max.r, max(hit_max.g, hit_max.b)));
    lobe = clamp(lobe, -0.1875, 0.0) * exp2(-stops);
    const float3 r = (lobe * (n + s + w + a) + e) / (4.0 * lobe + 1.0);
    return float4(rs_itm(r, hdr), e4.a);
}
struct SharpParams { float stops; float hdr; float pad0; float pad1; };
)MSL"
