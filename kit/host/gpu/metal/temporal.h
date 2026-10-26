// Remaster run 15 Wave B WB2: MetalFX temporal upscaling, the pure C++ part (policy only; no Metal here, so
// kit/dx/tests/d3d9_tests.cpp test_temporal checks it). The Metal side is d3d9_metal.mm's "---- Run 15 Wave B WB2"
// block (temporal_encode / temporal_scaler / temporal_reset), fed by the foundation's FndFrame (foundation.h).
//
// Conventions, measured with harness/tools/mfx_bench.mm `temporal-sign` (M5 Pro, 9 Oct 2026; the synthetic scene drawn
// exactly as the kit jitters it, error vs the ideal picture):
//   - jitterOffsetX/Y = +jx / +jy, the foundation's jitter in render pixels (x right, y down: pixel p shows the scene at
//     p + 0.5 - j). Error 0.0188 vs 0.115 / 0.112 with the x sign flipped (0.0305 with only y flipped; 0.0557 unjittered).
//   - motion = previous - current in render pixels, y down (the foundation kernel's convention), motionVectorScale +1.
//     Error 0.026 vs 0.331 flipped (x), 0.028 vs 0.333 (y).
//   - `temporal-formats`: BGRA8Unorm and RGBA16Float colour, Depth32Float_Stencil8 (the kit's scene depth) and
//     Depth32Float, RG16Float motion, R8Unorm reactive, auto exposure on/off: all create and encode; every input needs
//     only ShaderRead; the output needs ShaderRead | ShaderWrite | RenderTarget. Input content scale 1..3.
#pragma once
#include <cstdint>
#include "foundation.h"

namespace remaster_tmp {

constexpr float kJitterSignX = 1.0f, kJitterSignY = 1.0f;   // jitterOffset = sign x the foundation's jitter
constexpr float kMotionScale = 1.0f;                        // motionVectorScaleX/Y (vectors already in render px)

// The history resets that also reset MetalFX's history. RESET_PAUSE is left out on purpose: a paused game still draws
// the same picture with a new jitter phase every frame, which is exactly what the history should accumulate (a reset
// per frame would show the raw jitter as shimmer in the pause menu).
constexpr uint32_t kResetMask = ~(uint32_t)remaster_fnd::RESET_PAUSE;

// Why a frame went to the spatial scaler instead (counted per reason in the report line).
enum Refuse : uint32_t {
    REFUSE_NONE = 0,
    REFUSE_UNSUPPORTED,   // no MTLFXTemporalScalerDescriptor class / device not supported
    REFUSE_SCALE,         // the upscale factor is outside the device's supported input content scale
    REFUSE_NO_FRAME,      // no foundation window closed this scene (or FndFrame not valid this frame)
    REFUSE_NO_MOTION,     // no motion vectors this frame (no VP, first frame after a gap)
    REFUSE_NO_DEPTH,      // the scene has no depth
    REFUSE_SIZE,          // motion / depth / colour sizes differ
    REFUSE_MSAA,          // a multisampled colour or depth (temporal needs single-sample inputs)
    REFUSE_USAGE,         // a texture lacks a usage the scaler needs
    REFUSE_CREATE,        // the scaler or its output could not be created
    REFUSE_AB_OLD,        // the A/B switch `uptemp` is in its OLD phase (spatial on the same jittered backing)
    REFUSE_COUNT
};
inline const char *refuse_name(uint32_t r) {
    static const char *const n[REFUSE_COUNT] = {"none", "unsupported", "scale", "no_frame", "no_motion", "no_depth",
                                                "size", "msaa", "usage", "create", "ab_old"};
    return r < REFUSE_COUNT ? n[r] : "?";
}

// The inputs of one frame as the Metal side sees them (sizes 0 = the texture is absent).
struct Inputs {
    bool supported = false;       // the class exists and the device supports it
    double factor = 0.0, min_scale = 1.0, max_scale = 1.0;
    bool frame = false;           // a foundation window closed this scene with a usable FndFrame
    uint32_t cw = 0, ch = 0, cs = 1;    // colour (scene backing) size and samples
    uint32_t zw = 0, zh = 0, zs = 1;    // depth
    uint32_t mw = 0, mh = 0;            // motion
    bool usage_ok = true;
    bool ab_old = false;
};
inline Refuse check(const Inputs &in) {
    if (!in.supported)
        return REFUSE_UNSUPPORTED;
    if (in.factor < in.min_scale - 1e-3 || in.factor > in.max_scale + 1e-3)
        return REFUSE_SCALE;
    if (in.ab_old)
        return REFUSE_AB_OLD;
    if (!in.frame)
        return REFUSE_NO_FRAME;
    if (!in.mw || !in.mh)
        return REFUSE_NO_MOTION;
    if (!in.zw || !in.zh)
        return REFUSE_NO_DEPTH;
    if (in.cs > 1 || in.zs > 1)
        return REFUSE_MSAA;
    if (in.mw != in.cw || in.mh != in.ch || in.zw != in.cw || in.zh != in.ch)
        return REFUSE_SIZE;
    if (!in.usage_ok)
        return REFUSE_USAGE;
    return REFUSE_NONE;
}

// The scaler's identity: any change rebuilds it (and so resets the history).
struct Key {
    uint32_t iw = 0, ih = 0, ow = 0, oh = 0;
    uint64_t color = 0, depth = 0, reactive = 0;   // MTLPixelFormat values (reactive 0: no mask)
    bool auto_exposure = false;
    bool operator==(const Key &o) const {
        return iw == o.iw && ih == o.ih && ow == o.ow && oh == o.oh && color == o.color && depth == o.depth &&
               reactive == o.reactive && auto_exposure == o.auto_exposure;
    }
    bool operator!=(const Key &o) const { return !(*this == o); }
};

// History reset reasons of the MetalFX history (bits; 0 = keep the history).
enum : uint32_t {
    TRESET_FOUNDATION = 1,   // a foundation reset bit in kResetMask (cut, jump, FOV, VP, disagree, gap, resize, camera)
    TRESET_NEW_SCALER = 2,   // the scaler was (re)built
    TRESET_GAP = 4,          // the previous present was not a temporal frame (spatial fallback, A/B OLD, front end)
    TRESET_COUNT = 3
};
// `prev_frame`: the present serial of the last temporal frame (0: none); `frame`: this present's serial.
inline uint32_t reset_reasons(uint32_t fnd_reset, bool new_scaler, uint64_t prev_frame, uint64_t frame) {
    uint32_t r = 0;
    if (fnd_reset & kResetMask)
        r |= TRESET_FOUNDATION;
    if (new_scaler)
        r |= TRESET_NEW_SCALER;
    if (!prev_frame || frame != prev_frame + 1)
        r |= TRESET_GAP;
    return r;
}

// Auto exposure for float (HDR) scene colour only: the 8-bit scene is already display-referred 0..1.
inline bool want_auto_exposure(bool float_color, int env) { return env < 0 ? float_color : env > 0; }

}  // namespace remaster_tmp
