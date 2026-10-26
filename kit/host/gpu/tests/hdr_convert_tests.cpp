// hdr_convert_tests.cpp - PLAN_R2 item 10 real HDR (review/run14/hdr-design.md s7 C5): the readback conversion of a
// promoted (RGBA16Float) back buffer to the guest's 8-bit bytes, the presenter's mode-3 shoulder, and the guest
// formats the promotion must leave alone (format_info 21/22 still BGRA8: GetDesc and the 8-bit path unchanged).
#include "../hdr_convert.h"
#include "../d3d9_common.h"
#include <cstdio>
#include <vector>

static int g_checks, g_fail;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        if (!(x)) {                                                                                                    \
            ++g_fail;                                                                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                                         \
        }                                                                                                              \
    } while (0)

static uint16_t half_of(float f) {   // exact for the values used here (normal, representable)
    uint32_t b;
    std::memcpy(&b, &f, 4);
    const uint32_t s = b >> 16 & 0x8000u, e = (b >> 23 & 0xffu) - 112, m = b >> 13 & 0x3ffu;
    return f == 0.0f ? (uint16_t)s : (uint16_t)(s | e << 10 | m);
}

int main() {
    using namespace hdrconv;
    CHECK(half_to_float(0x3c00) == 1.0f);
    CHECK(half_to_float(0x4000) == 2.0f);
    CHECK(half_to_float(0x3800) == 0.5f);
    CHECK(half_to_float(0xbc00) == -1.0f);
    CHECK(half_to_float(0x7bff) == 65504.0f);
    CHECK(half_to_float(0x0001) > 0.0f && half_to_float(0x0001) < 1e-7f);   // subnormal
    CHECK(half_to_float(0x7c00) > 1e30f);                                   // inf
    for (float f : {0.0f, 0.25f, 0.5f, 1.0f, 1.5f, 3.0f, 64.0f})
        CHECK(half_to_float(half_of(f)) == f);
    // Conversion: clamp, unorm rounding, BGRA order, count of pixels above 1.
    const float px[3][4] = {{1.0f, 0.5f, 0.0f, 1.0f}, {2.5f, 0.25f, -1.0f, 0.0f}, {0.2f, 0.4f, 0.6f, 0.75f}};
    std::vector<uint16_t> h;
    for (auto &p : px)
        for (float c : p)
            h.push_back(half_of(c));
    uint8_t out[12] = {};
    const size_t over = rgba16f_to_bgra8(h.data(), out, 3);
    CHECK(over == 1);
    CHECK(out[0] == 0 && out[1] == 128 && out[2] == 255 && out[3] == 255);   // b g r a
    CHECK(out[4] == 0 && out[5] == 64 && out[6] == 255 && out[7] == 0);      // r clamped at 1, b at 0
    CHECK(out[8] == unorm8(half_to_float(half_of(0.6f))) && out[10] == unorm8(half_to_float(half_of(0.2f))));
    // Every value <= 1 converts as the 8-bit store would round it (the identity rule of design s5).
    for (int i = 0; i <= 255; ++i)
        CHECK(unorm8(half_to_float(half_of(i / 255.0f))) == i);
    // Mode-3 shoulder: identity to 1, continuous slope 1 at 1, monotone, asymptote H, hue kept with D = 0.
    for (float H : {1.5f, 2.0f, 4.0f, 8.0f}) {
        float a[3] = {1.0f, 0.5f, 0.25f};
        shoulder(a, H, 0.3f);
        CHECK(a[0] == 1.0f && a[1] == 0.5f && a[2] == 0.25f);
        float prev = 1.0f;
        for (float m = 1.001f; m < 1000.0f; m *= 1.05f) {
            float b[3] = {m, 0.0f, 0.0f};
            shoulder(b, H, 0.0f);
            CHECK(b[0] >= prev && b[0] < H);
            prev = b[0];
        }
        float c[3] = {1.0f + 1e-3f, 0.0f, 0.0f};
        shoulder(c, H, 0.0f);
        CHECK(std::fabs((c[0] - 1.0f) / 1e-3f - 1.0f) < 0.01f);   // f'(1) = 1
        float d[3] = {1e6f, 0.0f, 0.0f};
        shoulder(d, H, 0.0f);
        CHECK(std::fabs(d[0] - H) < 1e-3f * H);
        float e[3] = {4.0f, 2.0f, 1.0f};
        shoulder(e, H, 0.0f);
        CHECK(std::fabs(e[1] / e[0] - 0.5f) < 1e-5f && std::fabs(e[2] / e[0] - 0.25f) < 1e-5f);
    }
    float k[3] = {2.0f, 1.0f, 0.5f};   // H = 4: m = 2 -> 1.75 (design s3 example)
    shoulder(k, 4.0f, 0.0f);
    CHECK(std::fabs(k[0] - 1.75f) < 1e-5f);
    float s[3] = {0.5f, 2.0f, 0.1f};   // no headroom: plain clip
    shoulder(s, 1.0f, 0.3f);
    CHECK(s[0] == 0.5f && s[1] == 1.0f && s[2] == 0.1f);
    // The guest's back-buffer formats are untouched by the promotion (it swaps the Metal backing only).
    CHECK(d9gpu::format_info(21, false).store == d9gpu::Store::BGRA8);
    CHECK(d9gpu::format_info(22, false).store == d9gpu::Store::BGRA8);
    CHECK(d9gpu::format_info(113, false).store == d9gpu::Store::RGBA16F);   // the chain's f16 targets
    CHECK(d9gpu::format_info(115, false).store == d9gpu::Store::RG32F);     // Gothic 3's G32R32F target
    CHECK(d9gpu::format_info(115, false).conv == d9gpu::Conv::Direct);
    CHECK(d9gpu::format_info(112, false).store == d9gpu::Store::RG16F);
    std::printf("hdr_convert_tests: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
