// launcher_text.h - TrueType text for the launcher: stb_truetype glyphs
// rasterised with anti-aliasing at the screen's pixel size and kept in a
// cache, kerning, a fallback face for glyphs a font lacks (a game font
// without U+2026), and word wrapping of UTF-8 by measured width. Without any
// font file the kit's 6x8 bitmap font stands in, so the launcher always draws.
#pragma once
#include "launcher_canvas.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace launcher {

// The next code point of UTF-8 at *i (advanced); malformed bytes read as U+FFFD.
uint32_t utf8_next(const std::string &s, size_t *i);
size_t utf8_length(const std::string &s);

class Font {
  public:
    Font();
    ~Font();
    Font(const Font &) = delete;
    Font &operator=(const Font &) = delete;

    // A .ttf, or one face of a .ttc. "path#index" names the face too.
    bool load(const std::string &path, int index = 0);
    bool ok() const;
    const std::string &path() const {
        return path_;
    }
    // Glyphs this font lacks come from `f` (not owned).
    void set_fallback(const Font *f) {
        fallback_ = f;
    }
    // `px` is the em size in pixels.
    int ascent(int px) const;
    int descent(int px) const; // positive, below the baseline
    int line_height(int px) const;
    int measure(const std::string &utf8, int px, float tracking = 0) const;
    // Draws with the baseline at `baseline`; returns the width.
    int draw(Canvas &c, int x, int baseline, const std::string &utf8, int px, Color color,
             float tracking = 0) const;
    // Rasterised glyphs kept (each code point, size and subpixel phase once).
    size_t cached_glyphs() const {
        return cache_.size();
    }
    bool has_glyph(uint32_t cp) const;
    // Take `cp` from the fallback even though this font maps it (a game font
    // whose U+2026 draws nothing visible).
    void defer(uint32_t cp) {
        deferred_.push_back(cp);
    }

  private:
    struct Glyph {
        int x0 = 0, y0 = 0, w = 0, h = 0;
        std::vector<uint8_t> bits;
    };
    struct Face;
    const Font *owner_of(uint32_t cp) const; // this font or the fallback holding cp
    float scale(int px) const;
    float advance(uint32_t cp, int px) const;
    float kern(uint32_t a, uint32_t b, int px) const;
    const Glyph &glyph(uint32_t cp, int px, int phase) const;
    std::string path_;
    std::unique_ptr<Face> face_;
    const Font *fallback_ = nullptr;
    std::vector<uint32_t> deferred_;
    mutable std::map<uint64_t, Glyph> cache_;
};

// How wide a string is, for wrapping without a canvas (and in the tests).
using Measure = std::function<int(const std::string &)>;

// UTF-8 broken into lines no wider than `width` at spaces; a word wider than
// a line is split between code points (never inside one). '\n' starts a line.
std::vector<std::string> wrap_text(const std::string &utf8, int width, const Measure &measure);
// At most `max_lines`; when the text needs more, the last line ends in "…".
// `whole_words` (titles): a word wider than a line is never split; it is cut
// with "…" and ends the text there.
std::vector<std::string> wrap_lines(const std::string &utf8, int width, int max_lines,
                                    const Measure &measure, bool whole_words = false);
// `utf8` cut to `width` with "…" at the end, or in the middle (paths).
std::string ellipsize(const std::string &utf8, int width, const Measure &measure);
std::string ellipsize_middle(const std::string &utf8, int width, const Measure &measure);

} // namespace launcher
