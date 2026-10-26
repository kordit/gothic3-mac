// d3d9_msaa.h - the multisample sample count, pure logic (no Metal, no guest memory), so tests can reach it.
//
// A game asks for multisampling in D3D9 terms (D3DMULTISAMPLE_TYPE and a quality level); the renderer draws a
// sample count. Remaster (owner 8 Oct, "off, 2x or 4x and let the user decide"): the game's own FSAA value only says
// on or off (NONMASKABLE quality 1, drawn as 4x), so the count a player picked travels beside it as a cap,
// RECOMP_D3D9_MSAA_SAMPLES=2, set by the core mod before the device is created or reset. Unset (or 4): every count
// exactly as before, so 4x renders as the old 4x.
#pragma once
#include <cstdint>
#include <cstdlib>

namespace d9msaa {

// D3DMULTISAMPLE_TYPE and quality as a sample count: 2x and 4x, which every Apple GPU renders. NONMASKABLE (1)
// offers them as quality levels 0 and 1. 0: not multisampled.
inline uint32_t count(uint32_t type, uint32_t quality) {
    if (type == 1)
        return quality == 0 ? 2 : 4;
    if (type >= 2)
        return type >= 4 ? 4 : 2;
    return 0;
}

// The cap's text: "2" caps to 2 samples; anything else (unset, "4", junk) is no cap (0).
inline uint32_t parse_cap(const char *s) {
    return s && s[0] == '2' && s[1] == '\0' ? 2u : 0u;
}

// A requested count under the cap (0 = none). A single-sampled request stays single-sampled.
inline uint32_t capped(uint32_t n, uint32_t cap) {
    return cap && n > cap ? cap : n;
}

// count() under the cap the environment carries (read at device create / reset / surface creation only).
inline uint32_t count_env(uint32_t type, uint32_t quality) {
    return capped(count(type, quality), parse_cap(std::getenv("RECOMP_D3D9_MSAA_SAMPLES")));
}

// The count a renderer gives a request: 4 for 4 or more, else 2, then down by halves to the first the device
// supports (`supports(n)` -> bool); 1 when none is (drawn single-sampled).
template <class Supports>
inline uint32_t fit(uint32_t requested, Supports supports) {
    if (requested < 2)
        return 1;
    uint32_t n = requested >= 4 ? 4u : 2u;
    while (n > 1 && !supports(n))
        n /= 2;
    return n;
}

} // namespace d9msaa
