// host_d9.h - the Direct3D 9 device's GPU renderer, as the shim calls it.
//
// The shim keeps every resource's bytes in its own storage and decides what
// the GPU needs to hear. The host mirrors textures, surfaces and buffers under
// the shim's object ids and draws what it is told. A texture id names the
// whole texture; a surface of a texture is (texture id, face, level). A
// stand-alone surface - the back buffer, a depth-stencil surface - is a
// one-level texture under its own id.
//
// Weak no-op defaults live in host_d9_default.cpp; host_d9_active() returning
// zero there is what keeps the CPU renderer in charge.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { HOST_D9_TEX_2D = 0, HOST_D9_TEX_CUBE = 1, HOST_D9_TEX_VOLUME = 2 };
enum { HOST_D9_USAGE_RENDERTARGET = 1, HOST_D9_USAGE_DEPTH = 2 };

typedef struct HostD9TextureDesc {
    uint32_t id;
    uint32_t kind; // HOST_D9_TEX_*
    uint32_t width, height, levels;
    uint32_t format;  // D3DFORMAT
    uint32_t usage;   // HOST_D9_USAGE_*
    uint32_t samples; // multisampled render target or depth buffer: sample count; 0 or 1 for none
    uint32_t depth;   // volume depth; ignored for 2D/cube textures
    // Remaster A1: an opaque key a mod gave this texture (d3d9_texture_tag), or 0. The host uses it for
    // texture dumps and replacement; it never names a guest address.
    uint64_t key;
} HostD9TextureDesc;

// Non-zero when a GPU renderer is running and takes the device's draws.
int host_d9_active(void);

// (Re)creates the mirror; contents are undefined until uploaded or drawn.
void host_d9_texture_define(const HostD9TextureDesc *desc);
void host_d9_texture_drop(uint32_t id);
// Remaster A1: sets the key of a texture whose mirror is already defined (a texture tagged before its
// first draw carries the key in its HostD9TextureDesc instead).
void host_d9_texture_tag(uint32_t id, uint64_t key);
// One face/level from the shim's bytes, in the texture's D3DFORMAT.
// For a volume, face is the Z slice at this mip level. Upload completes before
// returning, so the caller may reuse the source bytes for the next slice.
void host_d9_texture_upload(uint32_t id, uint32_t face, uint32_t level, const uint8_t *bytes,
                            uint32_t pitch);
// Reads a colour face/level back as the texture's own format (32-bit formats
// only). Waits for the GPU. Non-zero on success.
int host_d9_texture_read(uint32_t id, uint32_t face, uint32_t level, uint8_t *bytes,
                         uint32_t pitch);
// Non-zero when host_d9_texture_read returns every face/level of the texture exactly as uploaded (its format is
// stored as is, at the game's size, in memory the CPU can read): the GPU's copy can be the only one.
int host_d9_texture_reads_back(uint32_t id);

// A bounded read for D3D9 GetRenderTargetData + LockRect(D3DLOCK_DONOTWAIT): queues a copy of a
// colour face/level (direct 32-bit formats) behind the work already handed to the renderer and
// returns at once. `ticket` (non-zero) names the request; a newer request with the same ticket
// replaces it. poll() never waits for the GPU or the render thread: 1 with the game-size image
// copied into `bytes` (w x h at `pitch`), 0 while the GPU has not run the copy yet, -1 when the
// ticket is unknown or the copy could not be made. forget() drops a ticket's request/result.
void host_d9_texture_read_async(uint32_t id, uint32_t face, uint32_t level, uint32_t ticket);
int host_d9_texture_read_poll(uint32_t ticket, uint8_t *bytes, uint32_t pitch, uint32_t w,
                              uint32_t h);
void host_d9_texture_read_forget(uint32_t ticket);

// Vertex and index buffers: `total` bytes, of which [offset, offset+size)
// changed.
void host_d9_buffer_upload(uint32_t id, uint32_t total, uint32_t offset, const uint8_t *bytes,
                           uint32_t size);
void host_d9_buffer_drop(uint32_t id);

typedef struct HostD9Surface {
    uint32_t id, face, level; // id 0: none
} HostD9Surface;

typedef struct HostD9Target {
    HostD9Surface color[4];
    HostD9Surface depth;
} HostD9Target;

typedef struct HostD9Stream {
    uint32_t buffer; // buffer id, or 0 for the draw's inline vertices
    uint32_t offset, stride;
    // SetStreamSourceFreq's setting: 0 or 1 an ordinary stream; D3DSTREAMSOURCE_INDEXEDDATA (0x40000000) | n
    // draws the indexed geometry n times; D3DSTREAMSOURCE_INSTANCEDATA (0x80000000) | d steps once every d
    // instances. Instancing takes an indexed draw from a buffer, as in Direct3D 9.
    uint32_t freq;
} HostD9Stream;

// Opt-in guest draw provenance. Addresses are guest values, never host pointers.
// api is the IDirect3DDevice9 vtable slot (81..84). reason: 0 accepted,
// 1 missing shaders, 2 missing declaration, 3 missing IB, 4 invalid inline range.
typedef struct HostD9Admission {
    uint64_t serial;
    uint32_t api, caller, device, reason;
    uint32_t inline_vertices, inline_indices; // guest addresses, UP calls only
    uint32_t resource_identity[9];            // streams 0..7, then index buffer COM identity
} HostD9Admission;
int host_d9_admission_enabled(void);
void host_d9_admission_entry(uint64_t serial);
// Cumulative totals; sampled only after Present has drained the host queue.
// Disjoint producer/consumer fields are read together only at that barrier.
typedef struct HostD9Accounting {
    uint64_t entries, forwarded, rejected, received, received_rejected, unforwarded;
    uint64_t entry_serial_sum, forwarded_serial_sum, received_serial_sum;
    uint64_t last_entry, last_received;
    uint32_t enabled, drained;
} HostD9Accounting;

typedef struct HostD9Draw {
    HostD9Admission admission;
    HostD9Target target;
    int32_t viewport[4]; // x, y, w, h
    float depth_range[2];
    int32_t scissor[4]; // left, top, right, bottom; used when D3DRS_SCISSORTESTENABLE

    const uint8_t *vs;
    uint32_t vs_size;
    const uint8_t *ps;
    uint32_t ps_size;
    uint64_t vs_key, ps_key; // d9sh::code_key of the two programs
    const float *vconst;     // 4 floats each
    uint32_t vconst_count;
    const float *pconst;
    uint32_t pconst_count;
    // Inline so the render queue retains values from this draw, not a later
    // mutation of the device's constants. Nonzero is true, as with D3D BOOL.
    int32_t vbool[16], pbool[16];

    const uint8_t *decl; // D3DVERTEXELEMENT9 records, ending with the 0xff stream
    uint32_t decl_size;
    uint32_t decl_id; // stable key for `decl`
    HostD9Stream stream[8];
    const uint8_t *inline_vertices;
    uint32_t inline_bytes;

    uint32_t index_buffer; // 0: not indexed, unless inline_indices
    const uint8_t *inline_indices;
    uint32_t inline_index_bytes;
    uint32_t index_size; // 2 or 4

    uint32_t primitive; // D3DPRIMITIVETYPE
    uint32_t primitive_count;
    uint32_t start; // first vertex, or first index when indexed
    int32_t base_vertex;

    uint32_t sampler_texture[16];    // texture ids; 0: none
    const uint32_t *sampler_state;   // 16 x 14, indexed [stage * 14 + D3DSAMPLERSTATETYPE]
    const uint32_t *render_state;    // 256, D3DRENDERSTATETYPE
    const uint8_t *render_state_set; // 256 flags: the game set it
    uint32_t projected_mask;         // stages with D3DTTFF_PROJECTED
    // Nonzero: the three state tables above are unchanged for as long as this
    // is, so a host may reuse its copy from an earlier draw with the same value.
    uint64_t state_version;
    const char *label; // what bound the shaders, for diagnostics; may be null
    // Test133: nonzero when stage 0 samples the texture a mod named in private render state 251 as part of the
    // scene (NFSMW: the rear-view mirror target): after the HDR scene marker it keeps the HDR highlight expansion.
    uint32_t hdr_scene_texture;
    // Remaster C2: nonzero when the caller already resolved both programs in the shared decode cache (the shim's
    // gpu_draw does), so the queue skips its second lookup under the cache mutex. Zero keeps the full lookup.
    uint32_t programs_ready;
    // Remaster run 10 fused pool shader: nonzero = draw with that pool variant (d9msl::PixelVariant::pool: 1
    // normal-mapped, 2 diffuse-only; lamp light added in this pass);
    // pool_block = 20 float4 (CleanPools[16], CleanPoolCount, TextureOffset, CleanPoolParams, LocalEyePos), copied
    // into the queue's payload. Zero for every other draw.
    uint32_t pool_fuse;
    const float *pool_block;
    // Remaster run 11 (review/run11/design-A.md): nonzero = this plain draw is the base pass of a two-pass pool draw
    // whose (programs, form) is not known to fuse yet; the renderer builds that form's fused shaders and pipeline for
    // it (drawing it plain) and records the outcome with host_d9_pool_mark. The value is the form.
    uint32_t pool_probe;
} HostD9Draw;

void host_d9_draw(const HostD9Draw *draw);
// Diagnostic only: queue in draw order, never render or upload its geometry.
void host_d9_rejected_draw(const HostD9Draw *draw);

// D3DCLEAR_TARGET 1, _ZBUFFER 2, _STENCIL 4. Rects are D3DRECTs.
void host_d9_clear(const HostD9Target *target, const int32_t viewport[4], uint32_t count,
                   const int32_t *rects, uint32_t flags, uint32_t color, float z, uint32_t stencil);

// StretchRect: rects are left, top, right, bottom. filter is D3DTEXTUREFILTERTYPE.
void host_d9_stretch(HostD9Surface src, const int32_t src_rect[4], HostD9Surface dst,
                     const int32_t dst_rect[4], uint32_t filter);

// The back buffer is finished: send it to the presenter. Commits the frame's
// GPU work. With RECOMP_D3D9_PRESENT_ASYNC=<n> this happens on the render
// thread, at most n presents behind the game (see host_d9_present_async).
void host_d9_present(uint32_t backbuffer, uint32_t width, uint32_t height);
// Live RECOMP_D3D9_PRESENT_ASYNC (PRESENT_ASYNC in the trace control file):
// presents allowed to wait for the render thread, 0 in step, -1 the environment's.
void host_d9_present_async(int frames);

// The last presented frame as tightly packed RGB, for dumps. Waits for the
// GPU. Writes nothing and returns zero when no frame was presented or `cap`
// is too small; *w and *h are set either way when a frame exists.
int host_d9_read_presented(uint8_t *rgb, uint32_t cap, uint32_t *w, uint32_t *h);

// Occlusion queries: the draws between begin and end count the samples that
// pass. result() is 1 with the count once the GPU has run them, 0 while it
// has not, -1 for a query it never saw. With `flush` a result still waiting
// in the open command buffer has it sent to the GPU (D3DGETDATA_FLUSH).
void host_d9_query_begin(uint32_t id);
void host_d9_query_end(uint32_t id);
int host_d9_query_result(uint32_t id, uint32_t *count, int flush);
void host_d9_query_drop(uint32_t id);
// The count of the id's latest issue if the render thread has published it,
// without waiting for that thread: 1 with *count, -1 for no result (as
// result()), 0 not yet. -2 when the issue is not tracked (it ended before the
// first peek, or the renderer cannot say): ask result() instead. The first
// call turns tracking on.
int host_d9_query_peek(uint32_t id, uint32_t *count);

// Diagnostics: log every draw of the next frame and dump its textures, with
// `tag` in the file names.
void host_d9_probe_next_frame(const char *tag);

// Remaster run 11 fused pools: readiness of a (vertex program, pixel program, pool form), so the shim fuses only
// what the renderer has already built (no per-draw round trip). Written by the render thread only, read by the game
// thread. States: 0 unknown (draw two-pass and probe), 1 ready (fuse), 2 blocked (always two-pass).
enum { HOST_D9_POOL_UNKNOWN = 0, HOST_D9_POOL_READY = 1, HOST_D9_POOL_BLOCKED = 2 };
static inline uint64_t host_d9_pool_key(uint64_t vs_key, uint64_t ps_key, uint32_t form) {
    uint64_t k = (vs_key ^ (ps_key * 0x9e3779b97f4a7c15ull)) * 0xff51afd7ed558ccdull + form;
    k ^= k >> 33;
    return k ? k : 1;
}
int host_d9_pool_state(uint64_t key);
void host_d9_pool_mark(uint64_t key, int state);

// Remaster run 13 MetalFX upscaling (review/run13/metalfx-design.md): the core mod marks a frame's 3D scene with
// private render states the device turns into events (never device state). HOST_D9_SCENE_BEGIN opens the scene:
// `bb` is the device's back buffer id, `ds` the depth-stencil surface bound with it then (0: none). HOST_D9_SCENE_END
// closes it (bb and ds 0). A renderer that upscales draws bb and ds at a lower resolution in between and scales the
// picture up into the native back buffer at the end; Present closes a scene left open. Any other renderer ignores
// both. Queued in order with the draws.
enum { HOST_D9_SCENE_END = 252, HOST_D9_SCENE_BEGIN = 253 };
void host_d9_scene_split(uint32_t phase, uint32_t bb, uint32_t ds);
// Remaster run 14 foundation F1 (review/run14/foundation-design.md 1, G): the core mod's camera for the scene window,
// sent only with RECOMP_REMASTER_FOUNDATION=1 as two private SetTransform states the device turns into one event (never
// device state; 0x200/0x201 are outside D3D's transform range). `view`: the camera's world -> view matrix, 16 floats,
// row-vector. `info` (16 words): [0] full vertical FOV (radians), [1] near, [2] far, then u32 bits: [3] flags (1 an
// in-engine cut-scene, 2 the world, 4 info), [4] the first camera mover's type, [5] the game frame, [6] the raw FOV u16. Queued in
// order with the draws; renderers without a foundation ignore it.
enum { HOST_D9_TS_SCENE_VIEW = 0x200, HOST_D9_TS_SCENE_INFO = 0x201 };
void host_d9_scene_camera(const float *view, const float *info);
// Remaster run 14 B1 height fog (review/run14/smoke-realism-design.md 8 (B)): the core mod's atmosphere packets, two
// private SetTransform states (0x202 then 0x203, 16 floats each; layout in host/gpu/metal/atmosphere.h Params) that
// the device turns into one event, never device state. Sent only while the setting `atmosphere` is on. Queued in
// order with the draws; renderers without the pass ignore it.
enum { HOST_D9_TS_ATMOS_A = 0x202, HOST_D9_TS_ATMOS_B = 0x203 };
void host_d9_atmosphere(const float *a, const float *b);

// PLAN_R2 item 10 real HDR (review/run14/hdr-design.md s2): 1 while the renderer's back buffer is float (RGBA16F, the
// guest still sees its own format). The renderer sets it at Present; GetRenderState(249), a private read-only state,
// returns it to the core mod (its post chain then runs in its HDR form). 0 with any renderer that has no float frame.
#define HOST_D9_RS_HDR_FLOAT 249
extern uint32_t host_d9_hdr_float;

#ifdef __cplusplus
}
#endif
