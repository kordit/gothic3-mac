// launcher_canvas.cpp - see launcher_canvas.h.
#include "launcher_canvas.h"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" const uint8_t *mods_font6x8_glyph(char c); // mods/font6x8.cpp

namespace launcher {

const uint8_t *font_glyph(char c) {
    return mods_font6x8_glyph(c >= 32 && c < 127 ? c : '?');
}

Rect Rect::intersect(const Rect &o) const {
    const int x0 = std::max(x, o.x), y0 = std::max(y, o.y);
    const int x1 = std::min(x + w, o.x + o.w), y1 = std::min(y + h, o.y + o.h);
    return {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

Color fade(Color c, float k) {
    k = std::clamp(k, 0.0f, 1.0f);
    return {c.r, c.g, c.b, uint8_t(std::lround(float(c.a) * k))};
}

Color mix(Color a, Color b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto m = [t](uint8_t x, uint8_t y) {
        return uint8_t(std::lround(float(x) + (float(y) - float(x)) * t));
    };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a)};
}

float round_rect_distance(float px, float py, const Rect &r, float radius) {
    const float hw = float(r.w) * 0.5f, hh = float(r.h) * 0.5f;
    radius = std::min(radius, std::min(hw, hh));
    const float cx = float(r.x) + hw, cy = float(r.y) + hh;
    const float qx = std::fabs(px - cx) - (hw - radius);
    const float qy = std::fabs(py - cy) - (hh - radius);
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
}

namespace {
inline int coverage_of(float d) { // d: signed distance at the pixel centre
    const float c = std::clamp(0.5f - d, 0.0f, 1.0f);
    return int(c * 255.0f + 0.5f);
}
} // namespace

void Canvas::resize(int w, int h, bool bgra) {
    w_ = std::max(0, w);
    h_ = std::max(0, h);
    bgra_ = bgra;
    clip_ = {0, 0, w_, h_};
    px_.assign(size_t(w_) * size_t(h_) * 4, 0);
}

void Canvas::set_clip(Rect r) {
    clip_ = r.intersect({0, 0, w_, h_});
}

void Canvas::reset_clip() {
    clip_ = {0, 0, w_, h_};
}

void Canvas::blend(uint8_t *p, Color c, int coverage) {
    const int a = (int(c.a) * coverage + 127) / 255;
    if (a <= 0)
        return;
    const uint8_t r = bgra_ ? c.b : c.r, b = bgra_ ? c.r : c.b;
    if (a >= 255) {
        p[0] = r;
        p[1] = c.g;
        p[2] = b;
        p[3] = 255;
        return;
    }
    const int na = 255 - a;
    p[0] = uint8_t((r * a + p[0] * na + 127) / 255);
    p[1] = uint8_t((c.g * a + p[1] * na + 127) / 255);
    p[2] = uint8_t((b * a + p[2] * na + 127) / 255);
    p[3] = 255;
}

void Canvas::put(int x, int y, Color c) {
    if (!clip_.contains(x, y))
        return;
    blend(&px_[(size_t(y) * size_t(w_) + size_t(x)) * 4], c, 255);
}

void Canvas::span(int y, int x0, int x1, Color c) {
    if (y < clip_.y || y >= clip_.y + clip_.h)
        return;
    x0 = std::max(x0, clip_.x);
    x1 = std::min(x1, clip_.x + clip_.w);
    if (x0 >= x1 || c.a == 0)
        return;
    uint8_t *p = &px_[(size_t(y) * size_t(w_) + size_t(x0)) * 4];
    if (c.a == 255) {
        const uint8_t q[4] = {bgra_ ? c.b : c.r, c.g, bgra_ ? c.r : c.b, 255};
        for (int x = x0; x < x1; ++x, p += 4)
            memcpy(p, q, 4);
        return;
    }
    for (int x = x0; x < x1; ++x, p += 4)
        blend(p, c, 255);
}

void Canvas::clear(Color c) {
    const Rect saved = clip_;
    reset_clip();
    fill({0, 0, w_, h_}, {c.r, c.g, c.b, 255});
    clip_ = saved;
}

void Canvas::copy(const std::vector<uint8_t> &same_format) {
    if (same_format.size() == px_.size())
        memcpy(px_.data(), same_format.data(), px_.size());
}

void Canvas::fill(Rect r, Color c) {
    const Rect b = r.intersect(clip_);
    for (int y = b.y; y < b.y + b.h; ++y)
        span(y, b.x, b.x + b.w, c);
}

void Canvas::frame(Rect r, int t, Color c) {
    fill({r.x, r.y, r.w, t}, c);
    fill({r.x, r.y + r.h - t, r.w, t}, c);
    fill({r.x, r.y + t, t, r.h - 2 * t}, c);
    fill({r.x + r.w - t, r.y + t, t, r.h - 2 * t}, c);
}

void Canvas::fill_round(Rect r, int radius, Color c) {
    radius = std::clamp(radius, 0, std::min(r.w, r.h) / 2);
    if (radius == 0) {
        fill(r, c);
        return;
    }
    const Rect b = r.intersect(clip_);
    const float fr = float(radius);
    for (int y = b.y; y < b.y + b.h; ++y) {
        if (y >= r.y + radius && y < r.y + r.h - radius) {
            span(y, b.x, b.x + b.w, c);
            continue;
        }
        uint8_t *row = &px_[size_t(y) * size_t(w_) * 4];
        const int mid0 = r.x + radius, mid1 = r.x + r.w - radius;
        for (int x = b.x; x < b.x + b.w; ++x) {
            if (x >= mid0 && x < mid1) {
                span(y, x, std::min(mid1, b.x + b.w), c);
                x = mid1 - 1;
                continue;
            }
            const int cov = coverage_of(round_rect_distance(float(x) + 0.5f, float(y) + 0.5f, r, fr));
            if (cov)
                blend(row + size_t(x) * 4, c, cov);
        }
    }
}

void Canvas::stroke_round(Rect r, int radius, int t, Color c) {
    if (t <= 0 || r.empty())
        return;
    radius = std::clamp(radius, 0, std::min(r.w, r.h) / 2);
    const Rect in = r.inset(t);
    const float fr = float(radius), ir = float(std::max(0, radius - t));
    const Rect b = r.intersect(clip_);
    for (int y = b.y; y < b.y + b.h; ++y) {
        uint8_t *row = &px_[size_t(y) * size_t(w_) * 4];
        const bool band = y >= r.y + radius + t && y < r.y + r.h - radius - t;
        for (int x = b.x; x < b.x + b.w; ++x) {
            if (band && x >= r.x + t + 1 && x < r.x + r.w - t - 1) {
                x = r.x + r.w - t - 2;
                continue;
            }
            const float px = float(x) + 0.5f, py = float(y) + 0.5f;
            int cov = coverage_of(round_rect_distance(px, py, r, fr));
            if (!in.empty())
                cov -= coverage_of(round_rect_distance(px, py, in, ir));
            if (cov > 0)
                blend(row + size_t(x) * 4, c, cov);
        }
    }
}

void Canvas::shadow(Rect r, int radius, int blur, Color c) {
    if (blur <= 0 || r.empty())
        return;
    const Rect outer{r.x - blur, r.y - blur, r.w + 2 * blur, r.h + 2 * blur};
    const Rect b = outer.intersect(clip_);
    const float fr = float(std::clamp(radius, 0, std::min(r.w, r.h) / 2)), fb = float(blur);
    for (int y = b.y; y < b.y + b.h; ++y) {
        uint8_t *row = &px_[size_t(y) * size_t(w_) * 4];
        const bool beside = y >= r.y && y < r.y + r.h;
        for (int x = b.x; x < b.x + b.w; ++x) {
            if (beside && x >= r.x + radius && x < r.x + r.w - radius) {
                x = r.x + r.w - radius - 1;
                continue;
            }
            const float d = round_rect_distance(float(x) + 0.5f, float(y) + 0.5f, r, fr);
            if (d <= 0)
                continue;
            const float t = 1.0f - std::min(1.0f, d / fb);
            const int cov = int(t * t * 255.0f);
            if (cov > 0)
                blend(row + size_t(x) * 4, c, cov);
        }
    }
}

void Canvas::gradient(Rect r, Color a, Color b, bool horizontal) {
    const Rect k = r.intersect(clip_);
    if (k.empty())
        return;
    if (!horizontal) {
        for (int y = k.y; y < k.y + k.h; ++y)
            span(y, k.x, k.x + k.w,
                 mix(a, b, r.h > 1 ? float(y - r.y) / float(r.h - 1) : 0.0f));
        return;
    }
    for (int x = k.x; x < k.x + k.w; ++x) {
        const Color c = mix(a, b, r.w > 1 ? float(x - r.x) / float(r.w - 1) : 0.0f);
        for (int y = k.y; y < k.y + k.h; ++y)
            blend(&px_[(size_t(y) * size_t(w_) + size_t(x)) * 4], c, 255);
    }
}

void Canvas::line(float x0, float y0, float x1, float y1, float t, Color c) {
    const float h = t * 0.5f;
    const Rect box{int(std::floor(std::min(x0, x1) - h - 1)), int(std::floor(std::min(y0, y1) - h - 1)),
                   int(std::ceil(std::fabs(x1 - x0) + t + 3)), int(std::ceil(std::fabs(y1 - y0) + t + 3))};
    const Rect b = box.intersect(clip_);
    const float dx = x1 - x0, dy = y1 - y0, len2 = std::max(1e-6f, dx * dx + dy * dy);
    for (int y = b.y; y < b.y + b.h; ++y)
        for (int x = b.x; x < b.x + b.w; ++x) {
            const float px = float(x) + 0.5f - x0, py = float(y) + 0.5f - y0;
            const float k = std::clamp((px * dx + py * dy) / len2, 0.0f, 1.0f);
            const float ex = px - dx * k, ey = py - dy * k;
            const int cov = coverage_of(std::sqrt(ex * ex + ey * ey) - h);
            if (cov)
                blend(&px_[(size_t(y) * size_t(w_) + size_t(x)) * 4], c, cov);
        }
}

void Canvas::triangle(float x0, float y0, float x1, float y1, float x2, float y2, Color c) {
    const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (std::fabs(area) < 1e-3f)
        return;
    if (area < 0) {
        std::swap(x1, x2);
        std::swap(y1, y2);
    }
    const float xs[3] = {x0, x1, x2}, ys[3] = {y0, y1, y2};
    float nx[3], ny[3], off[3];
    for (int i = 0; i < 3; ++i) {
        const int j = (i + 1) % 3;
        const float ex = xs[j] - xs[i], ey = ys[j] - ys[i];
        const float len = std::sqrt(ex * ex + ey * ey);
        nx[i] = -ey / len; // inward normal for counter-clockwise in y-down
        ny[i] = ex / len;
        off[i] = -(nx[i] * xs[i] + ny[i] * ys[i]);
    }
    const Rect box{int(std::floor(std::min({x0, x1, x2}))) - 1, int(std::floor(std::min({y0, y1, y2}))) - 1,
                   int(std::ceil(std::max({x0, x1, x2}) - std::min({x0, x1, x2}))) + 3,
                   int(std::ceil(std::max({y0, y1, y2}) - std::min({y0, y1, y2}))) + 3};
    const Rect b = box.intersect(clip_);
    for (int y = b.y; y < b.y + b.h; ++y)
        for (int x = b.x; x < b.x + b.w; ++x) {
            const float px = float(x) + 0.5f, py = float(y) + 0.5f;
            float d = 1e9f;
            for (int i = 0; i < 3; ++i)
                d = std::min(d, nx[i] * px + ny[i] * py + off[i]);
            const int cov = coverage_of(-d);
            if (cov)
                blend(&px_[(size_t(y) * size_t(w_) + size_t(x)) * 4], c, cov);
        }
}

void Canvas::disc(float cx, float cy, float radius, Color c) {
    const Rect box{int(std::floor(cx - radius - 1)), int(std::floor(cy - radius - 1)),
                   int(std::ceil(2 * radius + 3)), int(std::ceil(2 * radius + 3))};
    const Rect b = box.intersect(clip_);
    for (int y = b.y; y < b.y + b.h; ++y)
        for (int x = b.x; x < b.x + b.w; ++x) {
            const float px = float(x) + 0.5f - cx, py = float(y) + 0.5f - cy;
            const int cov = coverage_of(std::sqrt(px * px + py * py) - radius);
            if (cov)
                blend(&px_[(size_t(y) * size_t(w_) + size_t(x)) * 4], c, cov);
        }
}

void Canvas::mask(int x, int y, int w, int h, const uint8_t *coverage, Color c) {
    if (!coverage)
        return;
    const Rect b = Rect{x, y, w, h}.intersect(clip_);
    for (int yy = b.y; yy < b.y + b.h; ++yy) {
        const uint8_t *src = coverage + size_t(yy - y) * size_t(w);
        uint8_t *dst = &px_[size_t(yy) * size_t(w_) * 4];
        for (int xx = b.x; xx < b.x + b.w; ++xx)
            if (const uint8_t cov = src[xx - x])
                blend(dst + size_t(xx) * 4, c, cov);
    }
}

int Canvas::text(int x, int y, const std::string &s, int size, Color c) {
    size = std::max(1, size);
    for (size_t i = 0; i < s.size(); ++i) {
        const uint8_t *glyph = font_glyph(s[i]);
        const int gx = x + int(i) * 6 * size;
        if (gx >= w_)
            break;
        for (int row = 0; row < 8; ++row)
            for (int col = 0; col < 6; ++col)
                if (glyph[row] & (1 << (5 - col)))
                    fill({gx + col * size, y + row * size, size, size}, c);
    }
    return text_width(s, size);
}

std::vector<std::string> Canvas::wrap(const std::string &s, int width, int size) {
    const size_t per_line = size_t(std::max(1, width / (6 * std::max(1, size))));
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= s.size()) {
        size_t newline = s.find('\n', start);
        std::string para =
            s.substr(start, newline == std::string::npos ? std::string::npos : newline - start);
        while (para.size() > per_line) {
            size_t cut = para.rfind(' ', per_line);
            if (cut == std::string::npos || cut == 0) {
                // A path: after a slash rather than inside a name.
                const size_t slash = para.rfind('/', per_line - 1);
                cut = slash != std::string::npos && slash > 0 ? slash + 1 : per_line;
            }
            lines.push_back(para.substr(0, cut));
            para = para.substr(cut + (para[cut] == ' ' ? 1 : 0));
        }
        lines.push_back(para);
        if (newline == std::string::npos)
            break;
        start = newline + 1;
    }
    return lines;
}

void Canvas::image(int x, int y, int w, int h, const uint8_t *rgba) {
    if (!rgba)
        return;
    const Rect b = Rect{x, y, w, h}.intersect(clip_);
    for (int row = b.y; row < b.y + b.h; ++row) {
        const uint8_t *src = rgba + (size_t(row - y) * size_t(w) + size_t(b.x - x)) * 4;
        uint8_t *q = &px_[(size_t(row) * size_t(w_) + size_t(b.x)) * 4];
        for (int col = 0; col < b.w; ++col, src += 4, q += 4) {
            q[0] = bgra_ ? src[2] : src[0];
            q[1] = src[1];
            q[2] = bgra_ ? src[0] : src[2];
            q[3] = 255;
        }
    }
}

void Canvas::image(int x, int y, int w, int h, const uint8_t *rgba, const ImageStyle &st) {
    if (!rgba)
        return;
    const Rect r{x, y, w, h};
    const Rect b = r.intersect(clip_);
    const int radius = std::clamp(st.radius, 0, std::min(w, h) / 2);
    const int alpha = int(std::clamp(st.opacity, 0.0f, 1.0f) * 255.0f + 0.5f);
    const bool plain = st.saturation == 1.0f && st.brightness == 1.0f;
    for (int row = b.y; row < b.y + b.h; ++row) {
        const uint8_t *src = rgba + (size_t(row - y) * size_t(w) + size_t(b.x - x)) * 4;
        uint8_t *q = &px_[(size_t(row) * size_t(w_) + size_t(b.x)) * 4];
        const bool corner_row = row < y + radius || row >= y + h - radius;
        for (int col = b.x; col < b.x + b.w; ++col, src += 4, q += 4) {
            int cov = alpha * src[3] / 255;
            if (corner_row && (col < x + radius || col >= x + w - radius))
                cov = cov * coverage_of(round_rect_distance(float(col) + 0.5f, float(row) + 0.5f, r,
                                                            float(radius))) / 255;
            if (cov <= 0)
                continue;
            Color c{src[0], src[1], src[2], 255};
            if (!plain) {
                const float l = 0.299f * src[0] + 0.587f * src[1] + 0.114f * src[2];
                auto adj = [&](uint8_t v) {
                    const float s = l + (float(v) - l) * st.saturation;
                    return uint8_t(std::clamp(s * st.brightness, 0.0f, 255.0f));
                };
                c = {adj(src[0]), adj(src[1]), adj(src[2]), 255};
            }
            blend(q, c, cov);
        }
    }
}

int Canvas::paragraph(int x, int y, int width, const std::string &s, int size, Color c) {
    const std::vector<std::string> lines = wrap(s, width, size);
    const int line_h = text_height(size) + size * 3;
    for (size_t i = 0; i < lines.size(); ++i)
        text(x, y + int(i) * line_h, lines[i], size, c);
    return int(lines.size()) * line_h;
}

} // namespace launcher
