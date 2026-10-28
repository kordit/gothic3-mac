// d3d9_backend.h - what a GPU backend implements to render the Direct3D 9
// device (dx/host_d9.h).
//
// d3d9_host.cpp owns the host_d9_* entry points: it queues the device's work
// for a render thread and calls one D9Backend from that thread, or from the
// game's thread while the render thread is idle. A backend therefore sees one
// caller at a time and needs no locking of its own for these calls.
//
// A backend is made from the host's gpu::Device by the factory for its API
// (Metal, Vulkan, WebGPU); the first factory that recognises the device wins.
#pragma once
#include "../../dx/host_d9.h"
#include "gpu.h"

#include <cstdint>

class D9Backend {
  public:
    virtual ~D9Backend() = default;
    virtual const char *name() const = 0;

    virtual void define(const HostD9TextureDesc &d) = 0;
    virtual void drop(uint32_t id) = 0;
    // Remaster A1: a defined texture's key changed (see host_d9_texture_tag).
    virtual void tag(uint32_t id, uint64_t key) { (void)id; (void)key; }
    virtual void upload(uint32_t id, uint32_t face, uint32_t level, const uint8_t *bytes,
                        uint32_t pitch) = 0;
    virtual bool read(uint32_t id, uint32_t face, uint32_t level, uint8_t *bytes,
                      uint32_t pitch) = 0;
    // Whether read() gives back every face/level of this texture as the bytes upload() was given, so the GPU's copy
    // can be the only one (Gothic 3 session 22: a managed texture's levels keep no copy in the shim).
    virtual bool reads_back(uint32_t id) { (void)id; return false; }

    // Queues a copy of a colour face/level (direct 32-bit formats) behind the work already
    // encoded, without waiting. When the GPU has run it, `deliver` is called - from any thread -
    // with the image at the game's size (one sample per guest pixel), or with null bytes when the
    // copy cannot be made. A backend without the copy says so at once.
    using ReadDeliver = void (*)(uint32_t ticket, uint64_t request, const uint8_t *bytes,
                                 uint32_t w, uint32_t h);
    virtual void read_async(uint32_t id, uint32_t face, uint32_t level, uint32_t ticket,
                            uint64_t request, ReadDeliver deliver) {
        (void)id, (void)face, (void)level;
        deliver(ticket, request, nullptr, 0, 0);
    }

    virtual void buffer_upload(uint32_t id, uint32_t total, uint32_t offset, const uint8_t *bytes,
                               uint32_t size) = 0;
    virtual void buffer_drop(uint32_t id) = 0;

    virtual void draw(const HostD9Draw &d) = 0;
    virtual void admission_accounting(const HostD9Accounting &) {}
    virtual void rejected_draw(const HostD9Draw &) {} // diagnostic metadata only
    virtual void clear(const HostD9Target &target, const int32_t vp[4], uint32_t count,
                       const int32_t *rects, uint32_t flags, uint32_t color, float z,
                       uint32_t stencil) = 0;
    virtual void stretch(HostD9Surface src, const int32_t sr[4], HostD9Surface dst,
                         const int32_t dr[4], uint32_t filter) = 0;
    virtual void present(uint32_t backbuffer, uint32_t w, uint32_t h) = 0;
    virtual bool read_presented(uint8_t *rgb, uint32_t cap, uint32_t *w, uint32_t *h) = 0;

    virtual void query_begin(uint32_t id) = 0;
    virtual void query_end(uint32_t id) = 0;
    // 1 with *count set, 0 while its frame is still running, -1 for no result.
    // `flush` sends a command buffer still holding the query to the GPU.
    virtual int query_result(uint32_t id, uint32_t *count, bool flush) = 0;
    virtual void query_drop(uint32_t id) = 0;
    // A number that changes whenever more query results may have become available (Metal: the
    // last command buffer the GPU finished); kProgressUnknown has every ended query asked again on
    // each poll (d3d9_host.cpp publishes counts for host_d9_query_peek).
    static constexpr uint64_t kProgressUnknown = ~0ull;
    virtual uint64_t query_progress() const { return kProgressUnknown; }

    virtual void probe_next(const char *tag) = 0;

    // Remaster run 13 MetalFX upscaling: host_d9_scene_split, in queue order. No-op unless the backend upscales.
    virtual void scene_split(uint32_t phase, uint32_t bb, uint32_t ds) { (void)phase, (void)bb, (void)ds; }
    // Remaster run 14 foundation F1: host_d9_scene_camera (16 + 16 words), in queue order. No-op by default.
    virtual void scene_camera(const float *view, const float *info) { (void)view, (void)info; }
    // Remaster run 14 B1 height fog: host_d9_atmosphere (16 + 16 floats), in queue order. No-op by default.
    virtual void atmosphere(const float *a, const float *b) { (void)a, (void)b; }
};

// Each returns nullptr when `device` is not its API's, or when the renderer
// could not start. Defined by the backends this platform builds.
#ifdef __APPLE__
D9Backend *d9_metal_create(gpu::Device *device);
#endif
#ifdef RECOMP_D9_VULKAN
D9Backend *d9_vulkan_create(gpu::Device *device);
#endif
#ifdef __EMSCRIPTEN__
D9Backend *d9_webgpu_create(gpu::Device *device);
// The web: make the backend on the main thread before the game starts, then
// drain the game's queued work from the main loop once per frame. Work handed
// over between frames is drained as soon as the main thread is free; the main
// loop brackets its frame with host_d9_frame(true/false) so that a drain never
// starts inside it (a main thread waiting on a lock runs queued calls).
bool host_d9_web_start(gpu::Device *device);
void host_d9_pump(void);
void host_d9_frame(bool inside);
#endif
