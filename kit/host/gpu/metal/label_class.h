// R3 run 31 O6 (NFSMW review/run29/opt-design.md; A/B switch labelcache, kill RECOMP_OPT_LABELCACHE=0): the effect
// label prefixes the Metal draw path asks about, read in one pass per draw instead of a strncmp per question. Exact
// by construction: each bit answers the same prefix the strncmp it replaces asked (host_tests checks every prefix
// against strncmp, also on near misses). Computed from the string each draw, never cached by pointer, so a label
// address reused by a later effect cannot carry an old answer.
#pragma once

#include <cstdint>
#include <cstring>

namespace label_class {
enum : uint32_t {
    WORLD = 1u,          // "IDI_WORLD"
    WORLD_FX = 2u,       // "IDI_WORLD_FX/"
    WORLDNORMALMAP = 4u, // "IDI_WORLDNORMALMAP_FX/"
    PARTICLES = 8u,      // "IDI_PARTICLES_FX/"
    GLOSSYWINDOW = 16u,  // "IDI_GLOSSYWINDOW_FX/"
    SKYBOX = 32u,        // "IDI_SKYBOX_FX/"
    RAIN_DROP = 64u,     // "IDI_RAIN_DROP_FX/"
    CAR = 128u,          // "IDI_CAR_FX/"
};
inline uint32_t of(const char *l) {
    if (!l || strncmp(l, "IDI_", 4))
        return 0;
    const char *r = l + 4;
    switch (r[0]) {
    case 'W': {
        if (strncmp(r, "WORLD", 5))
            return 0;
        if (!strncmp(r + 5, "_FX/", 4))
            return WORLD | WORLD_FX;
        if (!strncmp(r + 5, "NORMALMAP_FX/", 13))
            return WORLD | WORLDNORMALMAP;
        return WORLD;
    }
    case 'P':
        return strncmp(r, "PARTICLES_FX/", 13) ? 0 : PARTICLES;
    case 'G':
        return strncmp(r, "GLOSSYWINDOW_FX/", 16) ? 0 : GLOSSYWINDOW;
    case 'S':
        return strncmp(r, "SKYBOX_FX/", 10) ? 0 : SKYBOX;
    case 'R':
        return strncmp(r, "RAIN_DROP_FX/", 13) ? 0 : RAIN_DROP;
    case 'C':
        return strncmp(r, "CAR_FX/", 7) ? 0 : CAR;
    default:
        return 0;
    }
}
// The prefix each bit stands for, for the OLD path and the test.
inline const char *prefix(uint32_t bit) {
    switch (bit) {
    case WORLD: return "IDI_WORLD";
    case WORLD_FX: return "IDI_WORLD_FX/";
    case WORLDNORMALMAP: return "IDI_WORLDNORMALMAP_FX/";
    case PARTICLES: return "IDI_PARTICLES_FX/";
    case GLOSSYWINDOW: return "IDI_GLOSSYWINDOW_FX/";
    case SKYBOX: return "IDI_SKYBOX_FX/";
    case RAIN_DROP: return "IDI_RAIN_DROP_FX/";
    case CAR: return "IDI_CAR_FX/";
    default: return nullptr;
    }
}
} // namespace label_class
