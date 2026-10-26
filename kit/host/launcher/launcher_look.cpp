// launcher_look.cpp - see launcher_look.h.
#include "launcher_look.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace launcher {

namespace {
inline float smooth(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

// Bilinear sample of `src` at (x, y) in source pixels (centres at +0.5).
inline void sample(const Image &src, float x, float y, float out[4]) {
    x = std::clamp(x - 0.5f, 0.0f, float(src.w - 1));
    y = std::clamp(y - 0.5f, 0.0f, float(src.h - 1));
    const int x0 = int(x), y0 = int(y);
    const int x1 = std::min(x0 + 1, src.w - 1), y1 = std::min(y0 + 1, src.h - 1);
    const float fx = x - float(x0), fy = y - float(y0);
    const uint8_t *a = &src.rgba[(size_t(y0) * size_t(src.w) + size_t(x0)) * 4];
    const uint8_t *b = &src.rgba[(size_t(y0) * size_t(src.w) + size_t(x1)) * 4];
    const uint8_t *c = &src.rgba[(size_t(y1) * size_t(src.w) + size_t(x0)) * 4];
    const uint8_t *d = &src.rgba[(size_t(y1) * size_t(src.w) + size_t(x1)) * 4];
    for (int k = 0; k < 4; ++k) {
        const float top = float(a[k]) + (float(b[k]) - float(a[k])) * fx;
        const float bot = float(c[k]) + (float(d[k]) - float(c[k])) * fx;
        out[k] = top + (bot - top) * fy;
    }
}

// An integer box reduction by `f`.
Image reduce(const Image &src, int f) {
    Image out;
    out.w = std::max(1, src.w / f);
    out.h = std::max(1, src.h / f);
    out.rgba.assign(size_t(out.w) * size_t(out.h) * 4, 0);
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) {
            int sum[4] = {0, 0, 0, 0};
            for (int yy = 0; yy < f; ++yy) {
                const uint8_t *p = &src.rgba[(size_t(y * f + yy) * size_t(src.w) + size_t(x * f)) * 4];
                for (int xx = 0; xx < f; ++xx, p += 4)
                    for (int k = 0; k < 4; ++k)
                        sum[k] += p[k];
            }
            uint8_t *q = &out.rgba[(size_t(y) * size_t(out.w) + size_t(x)) * 4];
            for (int k = 0; k < 4; ++k)
                q[k] = uint8_t(sum[k] / (f * f));
        }
    return out;
}
} // namespace

Image resample(const Image &in, int w, int h) {
    Image out;
    if (in.w <= 0 || in.h <= 0 || w <= 0 || h <= 0)
        return out;
    const int f = std::min(in.w / w, in.h / h);
    const Image reduced = f >= 2 ? reduce(in, f) : Image{};
    const Image &src = f >= 2 ? reduced : in;
    out.w = w;
    out.h = h;
    out.rgba.resize(size_t(w) * size_t(h) * 4);
    const float sx = float(src.w) / float(w), sy = float(src.h) / float(h);
    float px[4];
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            sample(src, (float(x) + 0.5f) * sx, (float(y) + 0.5f) * sy, px);
            uint8_t *q = &out.rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
            for (int k = 0; k < 4; ++k)
                q[k] = uint8_t(std::clamp(px[k] + 0.5f, 0.0f, 255.0f));
        }
    return out;
}

Image cover_fit(const Image &in, int w, int h, float fx, float fy, float zoom) {
    if (in.w <= 0 || in.h <= 0 || w <= 0 || h <= 0)
        return {};
    const float s = std::max(float(w) / float(in.w), float(h) / float(in.h)) * std::max(1.0f, zoom);
    const float vw = float(w) / s, vh = float(h) / s;
    const float ox = std::clamp(fx * float(in.w) - vw / 2, 0.0f, float(in.w) - vw);
    const float oy = std::clamp(fy * float(in.h) - vh / 2, 0.0f, float(in.h) - vh);
    const Image region = crop(in, {int(ox), int(oy), int(std::ceil(vw)), int(std::ceil(vh))});
    return resample(region, w, h);
}

Image crop(const Image &src, Rect r) {
    r = r.intersect({0, 0, src.w, src.h});
    Image out;
    out.w = r.w;
    out.h = r.h;
    out.rgba.resize(size_t(r.w) * size_t(r.h) * 4);
    for (int y = 0; y < r.h; ++y)
        memcpy(&out.rgba[size_t(y) * size_t(r.w) * 4],
               &src.rgba[(size_t(r.y + y) * size_t(src.w) + size_t(r.x)) * 4], size_t(r.w) * 4);
    return out;
}

Image alpha_from_black(const Image &src, float gain) {
    Image out = src;
    for (size_t i = 0; i + 3 < out.rgba.size(); i += 4) {
        uint8_t *p = &out.rgba[i];
        const int m = std::max(p[0], std::max(p[1], p[2]));
        // A little black is noise from the compression: cut it.
        const float a = std::clamp((float(m) - 10.0f) / 245.0f * gain, 0.0f, 1.0f);
        if (a <= 0.0f) {
            p[0] = p[1] = p[2] = p[3] = 0;
            continue;
        }
        for (int k = 0; k < 3; ++k)
            p[k] = uint8_t(std::clamp(float(p[k]) / a, 0.0f, 255.0f));
        p[3] = uint8_t(a * 255.0f + 0.5f);
    }
    return out;
}

Rect opaque_bounds(const Image &img, int threshold) {
    int x0 = img.w, y0 = img.h, x1 = -1, y1 = -1;
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x)
            if (img.rgba[(size_t(y) * size_t(img.w) + size_t(x)) * 4 + 3] > threshold) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
    if (x1 < 0)
        return {0, 0, img.w, img.h};
    return {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

Image nine_slice(const Image &in, const int si[4], int w, int h, const int di[4]) {
    Image out;
    if (in.w <= 0 || w <= 0 || h <= 0)
        return out;
    // Scale the source so its corners come out about 1:1, then stretch the rest.
    const float k = std::min(1.0f, float(di[0]) / float(std::max(1, si[0])));
    const Image scaled = k < 0.95f ? resample(in, std::max(1, int(float(in.w) * k)),
                                              std::max(1, int(float(in.h) * k)))
                                   : in;
    const float kx = float(scaled.w) / float(in.w), ky = float(scaled.h) / float(in.h);
    const float sl = float(si[0]) * kx, st = float(si[1]) * ky, sr = float(si[2]) * kx,
                sb = float(si[3]) * ky;
    const float dl = float(di[0]), dt = float(di[1]), dr = float(di[2]), db = float(di[3]);
    auto map = [](float d, float size, float a0, float a1, float src_size, float s0, float s1) {
        if (d < a0)
            return d * s0 / a0;
        if (d >= size - a1)
            return src_size - s1 + (d - (size - a1)) * s1 / a1;
        const float mid = std::max(1.0f, size - a0 - a1);
        return s0 + (d - a0) * (src_size - s0 - s1) / mid;
    };
    out.w = w;
    out.h = h;
    out.rgba.resize(size_t(w) * size_t(h) * 4);
    float px[4];
    for (int y = 0; y < h; ++y) {
        const float sy = map(float(y) + 0.5f, float(h), dt, db, float(scaled.h), st, sb);
        for (int x = 0; x < w; ++x) {
            const float sx = map(float(x) + 0.5f, float(w), dl, dr, float(scaled.w), sl, sr);
            sample(scaled, sx, sy, px);
            uint8_t *q = &out.rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
            for (int c = 0; c < 4; ++c)
                q[c] = uint8_t(std::clamp(px[c] + 0.5f, 0.0f, 255.0f));
        }
    }
    return out;
}

void box_blur(Image *img, int r, int passes) {
    if (!img || r <= 0 || img->w <= 0)
        return;
    std::vector<uint8_t> tmp(img->rgba.size());
    const int w = img->w, h = img->h;
    for (int pass = 0; pass < passes; ++pass)
        for (int dir = 0; dir < 2; ++dir) {
            const int n = dir ? h : w, lines = dir ? w : h;
            for (int l = 0; l < lines; ++l) {
                auto at = [&](int i) -> size_t {
                    i = std::clamp(i, 0, n - 1);
                    return dir ? (size_t(i) * size_t(w) + size_t(l)) * 4
                               : (size_t(l) * size_t(w) + size_t(i)) * 4;
                };
                int sum[4] = {0, 0, 0, 0};
                for (int i = -r; i <= r; ++i)
                    for (int k = 0; k < 4; ++k)
                        sum[k] += img->rgba[at(i) + size_t(k)];
                for (int i = 0; i < n; ++i) {
                    for (int k = 0; k < 4; ++k)
                        tmp[at(i) + size_t(k)] = uint8_t(sum[k] / (2 * r + 1));
                    for (int k = 0; k < 4; ++k)
                        sum[k] += int(img->rgba[at(i + r + 1) + size_t(k)]) -
                                  int(img->rgba[at(i - r) + size_t(k)]);
                }
            }
            img->rgba.swap(tmp);
        }
}

// ---------------------------------------------------------------------------
// Icons
// ---------------------------------------------------------------------------
void draw_icon(Canvas &c, Icon icon, float cx, float cy, float s, Color col) {
    const float t = std::max(1.5f, s * 0.085f);
    const float h = s * 0.5f;
    auto L = [&](float x0, float y0, float x1, float y1, float th = 0) {
        c.line(cx + x0 * s, cy + y0 * s, cx + x1 * s, cy + y1 * s, th > 0 ? th : t, col);
    };
    auto ring = [&](float x, float y, float r, float th = 0) {
        const int R = int(std::lround(r * s));
        c.stroke_round({int(std::lround(cx + x * s)) - R, int(std::lround(cy + y * s)) - R, 2 * R, 2 * R},
                       R, std::max(1, int(std::lround(th > 0 ? th : t))), col);
    };
    switch (icon) {
    case Icon::Play:
        c.triangle(cx - 0.28f * s, cy - 0.36f * s, cx + 0.38f * s, cy, cx - 0.28f * s, cy + 0.36f * s, col);
        break;
    case Icon::Puzzle: {
        const int x0 = int(cx - 0.34f * s), y0 = int(cy - 0.26f * s), side = int(0.6f * s);
        c.fill_round({x0, y0, side, side}, int(0.08f * s), col);
        c.disc(float(x0) + float(side) * 0.5f, float(y0) - 0.06f * s, 0.13f * s, col);
        c.disc(float(x0 + side) + 0.06f * s, float(y0) + float(side) * 0.5f, 0.13f * s, col);
        break;
    }
    case Icon::Gear: {
        for (int k = 0; k < 8; ++k) {
            const float a = float(k) * 3.14159265f / 4.0f;
            L(0.2f * std::cos(a), 0.2f * std::sin(a), 0.42f * std::cos(a), 0.42f * std::sin(a), 0.16f * s);
        }
        ring(0, 0, 0.27f, 0.16f * s);
        break;
    }
    case Icon::Folder: {
        const int x0 = int(cx - 0.42f * s), y0 = int(cy - 0.24f * s);
        c.stroke_round({x0, y0, int(0.84f * s), int(0.6f * s)}, int(0.06f * s), int(t), col);
        L(-0.42f, -0.24f, -0.42f, -0.34f);
        L(-0.42f, -0.34f, -0.1f, -0.34f);
        L(-0.1f, -0.34f, 0.0f, -0.24f);
        break;
    }
    case Icon::Document: {
        c.stroke_round({int(cx - 0.3f * s), int(cy - 0.4f * s), int(0.6f * s), int(0.8f * s)}, int(0.05f * s),
                       int(t), col);
        L(-0.15f, -0.15f, 0.15f, -0.15f);
        L(-0.15f, 0.02f, 0.15f, 0.02f);
        L(-0.15f, 0.19f, 0.08f, 0.19f);
        break;
    }
    case Icon::Database:
        for (int k = 0; k < 3; ++k) {
            const float y = -0.3f + 0.3f * float(k);
            c.stroke_round({int(cx - 0.36f * s), int(cy + (y - 0.11f) * s), int(0.72f * s), int(0.22f * s)},
                           int(0.11f * s), int(t), col);
        }
        break;
    case Icon::Exit:
        L(0.05f, -0.38f, -0.32f, -0.38f);
        L(-0.32f, -0.38f, -0.32f, 0.38f);
        L(-0.32f, 0.38f, 0.05f, 0.38f);
        L(-0.08f, 0.0f, 0.4f, 0.0f);
        L(0.4f, 0.0f, 0.24f, -0.16f);
        L(0.4f, 0.0f, 0.24f, 0.16f);
        break;
    case Icon::Search:
        ring(-0.08f, -0.08f, 0.26f);
        L(0.12f, 0.12f, 0.36f, 0.36f, t * 1.2f);
        break;
    case Icon::Check:
        L(-0.3f, 0.02f, -0.08f, 0.24f, t * 1.4f);
        L(-0.08f, 0.24f, 0.32f, -0.22f, t * 1.4f);
        break;
    case Icon::Back:
        L(-0.34f, 0, 0.34f, 0);
        L(-0.34f, 0, -0.12f, -0.22f);
        L(-0.34f, 0, -0.12f, 0.22f);
        break;
    case Icon::Cross:
        L(-0.24f, -0.24f, 0.24f, 0.24f, t * 1.2f);
        L(-0.24f, 0.24f, 0.24f, -0.24f, t * 1.2f);
        break;
    }
    (void)h;
}

// ---------------------------------------------------------------------------
// The backdrop
// ---------------------------------------------------------------------------
const Image &Look::cached(const std::string &key, const std::function<Image()> &make) {
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;
    if (cache.size() > 64)
        cache.clear();
    return cache.emplace(key, make()).first->second;
}

namespace {
// A small steady noise (film grain) so a dark gradient does not band.
inline float grain(int x, int y) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float(h & 0xFF) / 255.0f - 0.5f;
}
} // namespace

void Look::backdrop(Canvas &c, int screen) {
    const Image *src = !art_backdrop.rgba.empty() ? &art_backdrop
                       : !video_frame.rgba.empty() ? &video_frame
                                                   : nullptr;
    const int w = c.width(), h = c.height();
    if (w <= 0 || h <= 0)
        return;
    if (bg_w != w || bg_h != h || bg_bgra != c.bgra() || bg_source != src || bg[0].empty()) {
        bg_w = w;
        bg_h = h;
        bg_bgra = c.bgra();
        bg_source = src;
        cache.clear();
        for (int s = 0; s < 2; ++s) {
            Image pic;
            if (src) {
                if (s == 0)
                    pic = cover_fit(*src, w, h, src == &art_backdrop ? 0.6f : 0.0f, 0.5f,
                                    src == &art_backdrop ? 1.0f : 1.25f);
                else {
                    Image small = cover_fit(*src, std::max(1, w / 10), std::max(1, h / 10), 0.6f, 0.5f);
                    box_blur(&small, 3, 3);
                    pic = resample(small, w, h);
                }
            }
            std::vector<uint8_t> &out = bg[s];
            out.resize(size_t(w) * size_t(h) * 4);
            for (int y = 0; y < h; ++y) {
                const float fy = float(y) / float(h);
                for (int x = 0; x < w; ++x) {
                    const float fx = float(x) / float(w);
                    float r, g, b;
                    if (src) {
                        const uint8_t *p = &pic.rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
                        r = p[0];
                        g = p[1];
                        b = p[2];
                    } else {
                        // A warm glow behind the title on the left, dark elsewhere.
                        const float dx = fx - 0.3f, dy = (fy - 0.4f) * 0.8f;
                        const float glow = std::exp(-(dx * dx + dy * dy) * 4.0f);
                        r = 15 + 34 * glow;
                        g = 13 + 25 * glow;
                        b = 11 + 15 * glow;
                    }
                    float k;
                    if (s == 0) {
                        // Darker on the left, where the text sits, and at the bottom bar.
                        k = 0.30f + 0.70f * smooth(0.05f, 0.62f, fx);
                        k *= 1.0f - 0.45f * smooth(0.78f, 1.0f, fy);
                    } else
                        k = 0.34f;
                    const float vx = fx - 0.55f, vy = fy - 0.5f;
                    k *= 1.0f - 0.55f * std::min(1.0f, (vx * vx + vy * vy) * 1.6f);
                    if (!src)
                        k = std::max(k, 0.8f);
                    const float n = grain(x, y) * (src ? 3.0f : 4.0f);
                    const uint8_t R = uint8_t(std::clamp(r * k + n, 0.0f, 255.0f));
                    const uint8_t G = uint8_t(std::clamp(g * k + n, 0.0f, 255.0f));
                    const uint8_t B = uint8_t(std::clamp(b * k * 0.96f + n, 0.0f, 255.0f));
                    uint8_t *q = &out[(size_t(y) * size_t(w) + size_t(x)) * 4];
                    q[0] = bg_bgra ? B : R;
                    q[1] = G;
                    q[2] = bg_bgra ? R : B;
                    q[3] = 255;
                }
            }
        }
    }
    c.copy(bg[std::clamp(screen, 0, 1)]);
}

} // namespace launcher
