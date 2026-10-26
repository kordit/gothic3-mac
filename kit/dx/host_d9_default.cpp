// host_d9_default.cpp - weak no-op defaults for host_d9.h. A host that links a
// GPU renderer overrides every one of these.
#include "host_d9.h"

#define HOST_D9_DEFAULT __attribute__((weak))

// PLAN_R2 item 10 (host_d9.h): set by a renderer with a float frame, read by GetRenderState(249). Defined here, beside
// the defaults, so every host (and the tests) has it.
uint32_t host_d9_hdr_float = 0;

HOST_D9_DEFAULT int host_d9_active(void) {
    return 0;
}
HOST_D9_DEFAULT void host_d9_texture_define(const HostD9TextureDesc *) {}
HOST_D9_DEFAULT void host_d9_texture_drop(uint32_t) {}
HOST_D9_DEFAULT void host_d9_texture_tag(uint32_t, uint64_t) {}
HOST_D9_DEFAULT void host_d9_texture_upload(uint32_t, uint32_t, uint32_t, const uint8_t *,
                                            uint32_t) {}
HOST_D9_DEFAULT int host_d9_texture_read(uint32_t, uint32_t, uint32_t, uint8_t *, uint32_t) {
    return 0;
}
HOST_D9_DEFAULT void host_d9_texture_read_async(uint32_t, uint32_t, uint32_t, uint32_t) {}
HOST_D9_DEFAULT int host_d9_texture_read_poll(uint32_t, uint8_t *, uint32_t, uint32_t, uint32_t) {
    return -1;
}
HOST_D9_DEFAULT void host_d9_texture_read_forget(uint32_t) {}
HOST_D9_DEFAULT void host_d9_buffer_upload(uint32_t, uint32_t, uint32_t, const uint8_t *,
                                           uint32_t) {}
HOST_D9_DEFAULT void host_d9_buffer_drop(uint32_t) {}
HOST_D9_DEFAULT void host_d9_draw(const HostD9Draw *) {}
HOST_D9_DEFAULT int host_d9_admission_enabled(void) {
    return 0;
}
HOST_D9_DEFAULT void host_d9_admission_entry(uint64_t) {}
HOST_D9_DEFAULT void host_d9_rejected_draw(const HostD9Draw *) {}
HOST_D9_DEFAULT void host_d9_clear(const HostD9Target *, const int32_t[4], uint32_t,
                                   const int32_t *, uint32_t, uint32_t, float, uint32_t) {}
HOST_D9_DEFAULT void host_d9_stretch(HostD9Surface, const int32_t[4], HostD9Surface,
                                     const int32_t[4], uint32_t) {}
HOST_D9_DEFAULT void host_d9_present(uint32_t, uint32_t, uint32_t) {}
HOST_D9_DEFAULT int host_d9_read_presented(uint8_t *, uint32_t, uint32_t *, uint32_t *) {
    return 0;
}
HOST_D9_DEFAULT void host_d9_probe_next_frame(const char *) {}
HOST_D9_DEFAULT int host_d9_pool_state(uint64_t) {
    return HOST_D9_POOL_BLOCKED; // no renderer: never fuse
}
HOST_D9_DEFAULT void host_d9_pool_mark(uint64_t, int) {}
HOST_D9_DEFAULT void host_d9_query_begin(uint32_t) {}
HOST_D9_DEFAULT void host_d9_query_end(uint32_t) {}
HOST_D9_DEFAULT int host_d9_query_result(uint32_t, uint32_t *, int) {
    return -1;
}
HOST_D9_DEFAULT void host_d9_query_drop(uint32_t) {}
HOST_D9_DEFAULT int host_d9_query_peek(uint32_t, uint32_t *) {
    return -2;
}
HOST_D9_DEFAULT void host_d9_present_async(int) {}
HOST_D9_DEFAULT void host_d9_scene_split(uint32_t, uint32_t, uint32_t) {}
HOST_D9_DEFAULT void host_d9_scene_camera(const float *, const float *) {}
HOST_D9_DEFAULT void host_d9_atmosphere(const float *, const float *) {}
