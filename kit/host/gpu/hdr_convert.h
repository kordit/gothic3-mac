#pragma once
// hdr_convert.h - PLAN_R2 item 10 real HDR (review/run14/hdr-design.md s2.5, s7.9). The renderer's back buffer is
// RGBA16Float while the frame is float; whatever reads it as the guest's 8-bit format (LockRect/GetRenderTargetData,
// the frame dumps, the kept presented frame) gets these bytes: each channel clamped to [0,1] and rounded as the unorm
// store rounds, in BGRA order (D3DFMT_A8R8G8B8 / X8R8G8B8 in memory). No sRGB step: the values are gamma-encoded
// already (the game shades in gamma space).
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hdrconv {

inline float half_to_float(uint16_t h) {
    const uint32_t s = (uint32_t)(h & 0x8000u) << 16, e = (h >> 10) & 0x1fu, m = h & 0x3ffu;
    uint32_t bits;
    if (e == 0) {
        if (!m) {
            bits = s;
        } else {   // subnormal: normalise
            uint32_t mm = m, ee = 113;
            while (!(mm & 0x400u)) {
                mm <<= 1;
                --ee;
            }
            bits = s | ee << 23 | (mm & 0x3ffu) << 13;
        }
    } else if (e == 31) {
        bits = s | 0x7f800000u | m << 13;   // inf / nan
    } else {
        bits = s | (e + 112) << 23 | m << 13;
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

inline uint8_t unorm8(float x) {
    if (!(x > 0.0f))   // also NaN
        return 0;
    if (x >= 1.0f)
        return 255;
    return (uint8_t)std::lround(x * 255.0f);
}

// n RGBA16F pixels (8 bytes each) -> n BGRA8 pixels. Returns how many pixels had a colour channel above 1.0.
inline size_t rgba16f_to_bgra8(const void *src, uint8_t *dst, size_t n) {
    const uint16_t *h = (const uint16_t *)src;
    size_t over = 0;
    for (size_t i = 0; i < n; ++i, h += 4, dst += 4) {
        const float r = half_to_float(h[0]), g = half_to_float(h[1]), b = half_to_float(h[2]), a = half_to_float(h[3]);
        over += (r > 1.0f || g > 1.0f || b > 1.0f) ? 1 : 0;
        dst[0] = unorm8(b);
        dst[1] = unorm8(g);
        dst[2] = unorm8(r);
        dst[3] = unorm8(a);
    }
    return over;
}

// The presenter's mode-3 shoulder (shaders_msl.h hdr_shoulder), for the kit test and the Python twin's cross-check:
// identity up to 1, then f(m) = 1 + (H-1) u/(1+u), u = (m-1)/(H-1), on the max channel, hue kept, hot cores whitened.
inline void shoulder(float lin[3], float H, float D) {
    const float m = std::fmax(std::fmax(lin[0], lin[1]), lin[2]);
    if (m <= 1.0f || H <= 1.001f) {
        for (int k = 0; k < 3; ++k)
            lin[k] = std::fmin(lin[k], 1.0f);
        return;
    }
    const float u = (m - 1.0f) / (H - 1.0f), f = 1.0f + (H - 1.0f) * u / (1.0f + u), w = D * (f - 1.0f) / (H - 1.0f);
    for (int k = 0; k < 3; ++k)
        lin[k] = lin[k] * (f / m) * (1.0f - w) + f * w;
}

} // namespace hdrconv
