// launcher_canvas.h - a CPU canvas the launcher draws on: alpha-blended
// rectangles with rounded, anti-aliased corners, soft shadows, gradients,
// pictures, and coverage masks (the glyphs of launcher_text.h). The kit's 6x8
// bitmap font stays as the fallback when no TrueType font can be loaded.
// Pixels are BGRA or RGBA to match the drawable the canvas is copied to.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace launcher {

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
    bool empty() const {
        return w <= 0 || h <= 0;
    }
    Rect inset(int d) const {
        return {x + d, y + d, w - 2 * d, h - 2 * d};
    }
    Rect intersect(const Rect &o) const;
};

struct Color {
    uint8_t r, g, b, a;
};
// 0xRRGGBB with an alpha.
constexpr Color hex(uint32_t rgb, uint8_t a = 255) {
    return {uint8_t(rgb >> 16), uint8_t(rgb >> 8), uint8_t(rgb), a};
}
// `c` with its alpha scaled by `k` (0..1).
Color fade(Color c, float k);
// From `a` to `b`, t in 0..1 (alpha too).
Color mix(Color a, Color b, float t);

// How a picture is drawn: rounded corners, opacity, and a muted look (a mod
// the port cannot run yet).
struct ImageStyle {
    int radius = 0;
    float opacity = 1.0f;
    float saturation = 1.0f; // 0 = grey
    float brightness = 1.0f;
};

class Canvas {
  public:
    void resize(int w, int h, bool bgra);
    int width() const {
        return w_;
    }
    int height() const {
        return h_;
    }
    bool bgra() const {
        return bgra_;
    }
    const uint8_t *pixels() const {
        return px_.data();
    }
    uint8_t *pixels() {
        return px_.data();
    }
    // Drawing stays inside the clip (the whole canvas by default).
    void set_clip(Rect r);
    void reset_clip();
    Rect clip() const {
        return clip_;
    }

    void clear(Color c);
    void fill(Rect r, Color c);
    void frame(Rect r, int thickness, Color c);
    // Anti-aliased rounded rectangles; `radius` in pixels.
    void fill_round(Rect r, int radius, Color c);
    void stroke_round(Rect r, int radius, int thickness, Color c);
    // A soft shadow around `r` (not under it): `blur` pixels of falloff.
    void shadow(Rect r, int radius, int blur, Color c);
    // A gradient from `a` to `b`, top to bottom or left to right.
    void gradient(Rect r, Color a, Color b, bool horizontal = false);
    // An anti-aliased line with round ends.
    void line(float x0, float y0, float x1, float y1, float thickness, Color c);
    // An anti-aliased filled triangle (any winding).
    void triangle(float x0, float y0, float x1, float y1, float x2, float y2, Color c);
    // An anti-aliased disc.
    void disc(float cx, float cy, float radius, Color c);
    // An 8-bit coverage mask (a glyph) in colour `c`.
    void mask(int x, int y, int w, int h, const uint8_t *coverage, Color c);

    // Text in the 6x8 bitmap font at `size` pixels per font pixel; returns the
    // width drawn. Characters outside ASCII 32-126 draw as '?'.
    int text(int x, int y, const std::string &s, int size, Color c);
    static int text_width(const std::string &s, int size) {
        return int(s.size()) * 6 * size;
    }
    static int text_height(int size) {
        return 8 * size;
    }
    // Word-wrapped to `width`; returns the height used.
    int paragraph(int x, int y, int width, const std::string &s, int size, Color c);
    static std::vector<std::string> wrap(const std::string &s, int width, int size);
    // An RGBA8 picture (rows packed) at x, y, clipped, opaque. The styled
    // one blends with the picture's own alpha.
    void image(int x, int y, int w, int h, const uint8_t *rgba);
    void image(int x, int y, int w, int h, const uint8_t *rgba, const ImageStyle &style);
    // Copies a whole canvas-sized buffer in this canvas's pixel order.
    void copy(const std::vector<uint8_t> &same_format);

  private:
    void put(int x, int y, Color c);
    void blend(uint8_t *p, Color c, int coverage); // coverage 0..255
    void span(int y, int x0, int x1, Color c);
    int w_ = 0, h_ = 0;
    bool bgra_ = true;
    Rect clip_;
    std::vector<uint8_t> px_;
};

// A signed distance from (px, py) to a rounded rectangle's edge, negative
// inside. Shared by the canvas and the tests.
float round_rect_distance(float px, float py, const Rect &r, float radius);

// The glyph table the settings page uses (mods/font6x8.cpp).
const uint8_t *font_glyph(char c);

} // namespace launcher
