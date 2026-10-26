// launcher_text.cpp - see launcher_text.h.
#include "launcher_text.h"

#include "launcher_mods.h" // ascii_text, for the bitmap fallback

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../../third_party/stb/stb_truetype.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace launcher {

uint32_t utf8_next(const std::string &s, size_t *i) {
    const auto byte = [&](size_t k) { return uint32_t(uint8_t(s[k])); };
    const uint32_t c = byte(*i);
    int extra = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : -1;
    if (extra == 0) {
        ++*i;
        return c;
    }
    if (extra < 0 || *i + size_t(extra) >= s.size()) {
        ++*i;
        return 0xFFFD;
    }
    uint32_t cp = c & (0x3Fu >> extra);
    for (int k = 1; k <= extra; ++k) {
        const uint32_t b = byte(*i + size_t(k));
        if ((b & 0xC0) != 0x80) {
            *i += size_t(k);
            return 0xFFFD;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    *i += size_t(extra) + 1;
    return cp;
}

size_t utf8_length(const std::string &s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++n)
        utf8_next(s, &i);
    return n;
}

struct Font::Face {
    std::vector<uint8_t> data;
    stbtt_fontinfo info{};
    int ascent = 0, descent = 0, gap = 0;
};

Font::Font() = default;
Font::~Font() = default;

bool Font::load(const std::string &spec, int index) {
    std::string path = spec;
    const size_t hash = spec.rfind('#');
    if (hash != std::string::npos && hash + 1 < spec.size() &&
        spec.find_first_not_of("0123456789", hash + 1) == std::string::npos) {
        path = spec.substr(0, hash);
        index = atoi(spec.c_str() + hash + 1);
    }
    face_.reset();
    cache_.clear();
    path_.clear();
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    auto face = std::make_unique<Face>();
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0 && size < (64l << 20)) {
        face->data.resize(size_t(size));
        if (fread(face->data.data(), 1, face->data.size(), f) != face->data.size())
            face->data.clear();
    }
    fclose(f);
    if (face->data.empty())
        return false;
    const int offset = stbtt_GetFontOffsetForIndex(face->data.data(), index);
    if (offset < 0 || !stbtt_InitFont(&face->info, face->data.data(), offset))
        return false;
    stbtt_GetFontVMetrics(&face->info, &face->ascent, &face->descent, &face->gap);
    face_ = std::move(face);
    path_ = spec;
    return true;
}

bool Font::ok() const {
    return face_ != nullptr;
}

bool Font::has_glyph(uint32_t cp) const {
    if (!face_ || std::find(deferred_.begin(), deferred_.end(), cp) != deferred_.end())
        return false;
    const int g = stbtt_FindGlyphIndex(&face_->info, int(cp));
    // A mapped but empty glyph (a game font's blank U+2026) counts as missing.
    return g != 0 && (cp == ' ' || cp == 0xA0 || !stbtt_IsGlyphEmpty(&face_->info, g));
}

const Font *Font::owner_of(uint32_t cp) const {
    if (has_glyph(cp))
        return this;
    if (fallback_ && fallback_->has_glyph(cp))
        return fallback_;
    return face_ ? this : fallback_;
}

float Font::scale(int px) const {
    return face_ ? stbtt_ScaleForMappingEmToPixels(&face_->info, float(px)) : 0.0f;
}

namespace {
// The bitmap font's pixel size for an em of `px`.
int bitmap_size(int px) {
    return std::max(1, (px + 4) / 9);
}
} // namespace

int Font::ascent(int px) const {
    if (!face_)
        return fallback_ && fallback_->ok() ? fallback_->ascent(px) : 7 * bitmap_size(px);
    return int(std::ceil(float(face_->ascent) * scale(px)));
}

int Font::descent(int px) const {
    if (!face_)
        return fallback_ && fallback_->ok() ? fallback_->descent(px) : bitmap_size(px);
    return int(std::ceil(float(-face_->descent) * scale(px)));
}

int Font::line_height(int px) const {
    if (!face_)
        return fallback_ && fallback_->ok() ? fallback_->line_height(px)
                                            : 8 * bitmap_size(px) + 3 * bitmap_size(px);
    return int(std::ceil(float(face_->ascent - face_->descent + face_->gap) * scale(px)));
}

float Font::advance(uint32_t cp, int px) const {
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&face_->info, int(cp), &adv, &lsb);
    return float(adv) * scale(px);
}

float Font::kern(uint32_t a, uint32_t b, int px) const {
    return float(stbtt_GetCodepointKernAdvance(&face_->info, int(a), int(b))) * scale(px);
}

const Font::Glyph &Font::glyph(uint32_t cp, int px, int phase) const {
    const uint64_t key = uint64_t(cp) | (uint64_t(px) << 21) | (uint64_t(phase) << 40);
    auto it = cache_.find(key);
    if (it != cache_.end())
        return it->second;
    Glyph g;
    const float s = scale(px), shift = float(phase) * 0.25f;
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBoxSubpixel(&face_->info, int(cp), s, s, shift, 0, &x0, &y0, &x1, &y1);
    g.x0 = x0;
    g.y0 = y0;
    g.w = std::max(0, x1 - x0);
    g.h = std::max(0, y1 - y0);
    if (g.w > 0 && g.h > 0) {
        g.bits.assign(size_t(g.w) * size_t(g.h), 0);
        stbtt_MakeCodepointBitmapSubpixel(&face_->info, g.bits.data(), g.w, g.h, g.w, s, s, shift, 0,
                                          int(cp));
    }
    return cache_.emplace(key, std::move(g)).first->second;
}

int Font::measure(const std::string &s, int px, float tracking) const {
    if (!face_) {
        if (fallback_ && fallback_->ok())
            return fallback_->measure(s, px, tracking);
        return Canvas::text_width(ascii_text(s), bitmap_size(px));
    }
    float pen = 0;
    uint32_t prev = 0;
    const Font *prev_owner = nullptr;
    size_t count = 0;
    for (size_t i = 0; i < s.size();) {
        const uint32_t cp = utf8_next(s, &i);
        const Font *f = owner_of(cp);
        if (prev && f == prev_owner)
            pen += f->kern(prev, cp, px);
        pen += f->advance(cp, px);
        prev = cp;
        prev_owner = f;
        ++count;
    }
    if (count > 1)
        pen += tracking * float(count - 1);
    return int(std::ceil(pen));
}

int Font::draw(Canvas &c, int x, int baseline, const std::string &s, int px, Color color,
               float tracking) const {
    if (!face_) {
        if (fallback_ && fallback_->ok())
            return fallback_->draw(c, x, baseline, s, px, color, tracking);
        const int k = bitmap_size(px);
        return c.text(x, baseline - 7 * k, ascii_text(s), k, color);
    }
    float pen = float(x);
    uint32_t prev = 0;
    const Font *prev_owner = nullptr;
    const Rect clip = c.clip();
    for (size_t i = 0; i < s.size();) {
        const uint32_t cp = utf8_next(s, &i);
        const Font *f = owner_of(cp);
        if (prev && f == prev_owner)
            pen += f->kern(prev, cp, px);
        if (pen > float(clip.x + clip.w))
            break;
        const int ix = int(std::floor(pen));
        const int phase = std::min(3, int((pen - float(ix)) * 4.0f));
        if (cp != ' ') {
            const Glyph &g = f->glyph(cp, px, phase);
            if (!g.bits.empty())
                c.mask(ix + g.x0, baseline + g.y0, g.w, g.h, g.bits.data(), color);
        }
        pen += f->advance(cp, px) + tracking;
        prev = cp;
        prev_owner = f;
    }
    return int(std::ceil(pen - tracking)) - x;
}

// ---------------------------------------------------------------------------
// Wrapping
// ---------------------------------------------------------------------------
namespace {
// `word` split so each piece fits `width`, between code points.
std::vector<std::string> split_word(const std::string &word, int width, const Measure &measure) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < word.size();) {
        const size_t start = i;
        utf8_next(word, &i);
        const std::string next = cur + word.substr(start, i - start);
        if (!cur.empty() && measure(next) > width) {
            out.push_back(cur);
            cur = word.substr(start, i - start);
        } else
            cur = next;
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}
} // namespace

std::vector<std::string> wrap_text(const std::string &text, int width, const Measure &measure) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (true) {
        const size_t nl = text.find('\n', start);
        const std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        std::string cur;
        size_t i = 0;
        bool any = false;
        while (i < para.size()) {
            while (i < para.size() && para[i] == ' ')
                ++i;
            if (i >= para.size())
                break;
            size_t j = para.find(' ', i);
            if (j == std::string::npos)
                j = para.size();
            const std::string word = para.substr(i, j - i);
            i = j;
            any = true;
            const std::string joined = cur.empty() ? word : cur + " " + word;
            if (measure(joined) <= width) {
                cur = joined;
                continue;
            }
            if (!cur.empty())
                lines.push_back(cur);
            cur.clear();
            if (measure(word) <= width) {
                cur = word;
                continue;
            }
            std::vector<std::string> pieces = split_word(word, width, measure);
            for (size_t k = 0; k + 1 < pieces.size(); ++k)
                lines.push_back(pieces[k]);
            cur = pieces.empty() ? std::string() : pieces.back();
        }
        if (!cur.empty() || !any)
            lines.push_back(cur);
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    return lines;
}

std::string ellipsize(const std::string &s, int width, const Measure &measure) {
    if (measure(s) <= width)
        return s;
    static const std::string dots = "\xE2\x80\xA6";
    std::string cut = s;
    // Drop code points from the end until the rest and "…" fit.
    while (!cut.empty()) {
        size_t k = cut.size() - 1;
        while (k > 0 && (uint8_t(cut[k]) & 0xC0) == 0x80)
            --k;
        cut.erase(k);
        while (!cut.empty() && cut.back() == ' ')
            cut.pop_back();
        if (measure(cut + dots) <= width)
            return cut + dots;
    }
    return measure(dots) <= width ? dots : std::string();
}

std::string ellipsize_middle(const std::string &s, int width, const Measure &measure) {
    if (measure(s) <= width)
        return s;
    static const std::string dots = "\xE2\x80\xA6";
    std::vector<size_t> starts; // code point starts
    for (size_t i = 0; i < s.size();) {
        starts.push_back(i);
        utf8_next(s, &i);
    }
    // Keep the end longer than the start: a path's last folders say most.
    for (size_t keep = starts.size(); keep-- > 1;) {
        const size_t head = keep / 3, tail = keep - head;
        const std::string t = s.substr(0, starts[head]) + dots + s.substr(starts[starts.size() - tail]);
        if (measure(t) <= width)
            return t;
    }
    return ellipsize(s, width, measure);
}

std::vector<std::string> wrap_lines(const std::string &text, int width, int max_lines,
                                    const Measure &measure, bool whole_words) {
    if (whole_words) {
        // Lines of whole words; a word too wide for any line is cut and ends the text.
        std::vector<std::string> out;
        std::string cur;
        size_t i = 0;
        bool cut = false;
        while (i < text.size() && !cut) {
            while (i < text.size() && (text[i] == ' ' || text[i] == '\n'))
                ++i;
            if (i >= text.size())
                break;
            size_t j = text.find_first_of(" \n", i);
            if (j == std::string::npos)
                j = text.size();
            const std::string word = text.substr(i, j - i);
            i = j;
            const std::string joined = cur.empty() ? word : cur + " " + word;
            if (measure(joined) <= width) {
                cur = joined;
                continue;
            }
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
            if (measure(word) <= width)
                cur = word;
            else {
                out.push_back(ellipsize(word, width, measure));
                cut = true;
            }
        }
        if (!cur.empty())
            out.push_back(cur);
        if (max_lines > 0 && int(out.size()) > max_lines) {
            std::string rest = out[size_t(max_lines - 1)];
            for (size_t k = size_t(max_lines); k < out.size(); ++k)
                rest += " " + out[k];
            out.resize(size_t(max_lines));
            out.back() = ellipsize(rest, width, measure);
        }
        return out;
    }
    std::vector<std::string> lines = wrap_text(text, width, measure);
    if (max_lines <= 0 || int(lines.size()) <= max_lines)
        return lines;
    // The last kept line takes the rest of the text, cut with "…".
    std::string rest = lines[size_t(max_lines - 1)];
    for (size_t k = size_t(max_lines); k < lines.size(); ++k)
        rest += " " + lines[k];
    lines.resize(size_t(max_lines));
    std::string cut = ellipsize(rest, width, measure);
    // Prefer ending on a whole word when one fits.
    static const std::string dots = "\xE2\x80\xA6";
    const size_t space = cut.rfind(' ');
    if (space != std::string::npos && space > 0 && measure(cut.substr(0, space)) >= width / 2) {
        const std::string word_cut = cut.substr(0, space) + dots;
        if (measure(word_cut) <= width)
            cut = word_cut;
    }
    lines.back() = cut;
    return lines;
}

} // namespace launcher
