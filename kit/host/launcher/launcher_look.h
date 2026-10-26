// launcher_look.h - what the launcher looks like, apart from what it says:
// the palette, the fonts and pictures it loads (the game's own font from the
// player's game folder, a system serif, the game's art from the app's
// Resources/launcher or a frame of the game's intro), the backdrop prepared
// once per window size, framed buttons (a 9-slice picture or drawn), and
// vector icons. Internal to the launcher (launcher_ui*.cpp).
#pragma once
#include "launcher_canvas.h"
#include "launcher_mods.h"
#include "launcher_text.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace launcher {

namespace pal {
constexpr Color bg0 = hex(0x0f0d0b), bg1 = hex(0x1a1612);
constexpr Color panel = hex(0x221d18, 217), panel_solid = hex(0x221d18), panel_hi = hex(0x2d261f, 230);
constexpr Color border = hex(0x3a3128), border_hi = hex(0x5a4b38);
constexpr Color text = hex(0xe8dcc4), dim = hex(0xa89a82), faint = hex(0x7a6e5c);
constexpr Color gold = hex(0xc9a24a), gold_hot = hex(0xe0bb5f), gold_deep = hex(0x8a6a2a);
constexpr Color red = hex(0xb5483a), green = hex(0x7fa36b), amber = hex(0xd09a4a);
constexpr Color red_text = hex(0xe07a68); // red words on a dark panel, readable
constexpr Color ink = hex(0x15110d);
} // namespace pal

// Pictures (RGBA8 Image from launcher_mods.h).
// `src` scaled to w x h, bilinear after a box reduction for large shrinks.
Image resample(const Image &src, int w, int h);
// `src` filling w x h, scaled by `zoom` beyond cover, the crop centred on
// (focus_x, focus_y) of the source (0..1) as far as the edges allow.
Image cover_fit(const Image &src, int w, int h, float focus_x, float focus_y, float zoom = 1.0f);
// A picture on black (a logo, an ornament) as one with alpha: the black goes
// transparent, the colours keep their look over a dark background.
Image alpha_from_black(const Image &src, float gain = 1.0f);
// The bounding box of pixels with alpha above `threshold`.
Rect opaque_bounds(const Image &img, int threshold = 16);
Image crop(const Image &src, Rect r);
// A 9-slice: the source's corners (insets l, t, r, b) kept at the
// destination's corner sizes, edges and middle stretched.
Image nine_slice(const Image &src, const int src_inset[4], int w, int h, const int dst_inset[4]);
void box_blur(Image *img, int radius, int passes = 3);

enum class Icon { Play, Puzzle, Gear, Folder, Document, Database, Exit, Search, Check, Back, Cross };
// `size` is the icon's box in pixels, centred on (cx, cy).
void draw_icon(Canvas &c, Icon icon, float cx, float cy, float size, Color color);

// Polish counts: 1 "mod", 2-4 "mody", 5+ "modów" (12-14 "modów").
std::string mods_word(size_t n);

// What the launcher loads once and keeps.
struct Look {
    Font display, body, strong;
    std::string fonts_for; // the game folder the fonts were looked up for
    bool fonts_done = false;
    bool art_done = false;
    Image art_backdrop, logo, frame, rosette; // from the game's art; empty when absent
    Image video_frame;                        // a frame of the intro when there is no art backdrop
    std::string video_for;
    bool video_done = false;
    // The backdrop at the canvas size: [0] the main screen, [1] the mod screen.
    std::vector<uint8_t> bg[2];
    int bg_w = 0, bg_h = 0;
    bool bg_bgra = false;
    const Image *bg_source = nullptr;
    // Scaled pictures by key ("frame:w:h", "logo:w", ...), rebuilt on a new size.
    std::map<std::string, Image> cache;
    const Image &cached(const std::string &key, const std::function<Image()> &make);
    // The backdrop for `screen` (0 main, 1 mods) at the canvas size.
    void backdrop(Canvas &c, int screen);
};

} // namespace launcher
