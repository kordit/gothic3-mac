// Text for guest canvases: a TrueType face the program registered when its font
// names one, the bundled stand-in when it names a Windows interface face, the
// host's installed font file of that family, else fixed 8x16 bitmap cells. No
// host font services (only font files) or guest
// pointers escape this file; callbacks receive temporary guest-heap records.
#include "gdi32_internal.h"
#include <cstdlib>
#include "../platform/os.h"
#include "gdi32_font8x16.h"
#include "gdi32_truetype.h"
#include "memory.h"
#include "native_seam.h"
#include "win32.h"
#include <algorithm>
#include <cstring>
#include <climits>
#include <cstdio>
#include <vector>
using namespace gdi;
namespace {
uint32_t font_word(const Object &font, int offset) {
    uint32_t v;
    memcpy(&v, font.logfont.data() + offset, 4);
    return v;
}
int32_t font_scale(uint32_t dc) {
    auto *d = dc_of(dc);
    auto it = objects().find(d ? d->font : 0);
    int64_t height = it == objects().end() ? 16 : int32_t(font_word(it->second, 0));
    return int32_t(std::max<int64_t>(1, std::abs(height) / 16));
}
void create_font(X86 *c) {
    uint32_t p = arg(c, 0);
    if (!p || !gm_valid(p, 92)) {
        set_eax(c, 0);
        return;
    }
    Object font;
    font.kind = Object::Font;
    memcpy(font.logfont.data(), g_mem + p, 92);
    set_eax(c, make_object(font));
}
// LOGFONTA is LOGFONTW with a byte face name: 28 bytes of fields, then 32
// characters. The font keeps the wide record.
void create_font_a(X86 *c) {
    uint32_t p = arg(c, 0);
    if (!p || !gm_valid(p, 60)) {
        set_eax(c, 0);
        return;
    }
    Object font;
    font.kind = Object::Font;
    memset(font.logfont.data(), 0, 92);
    memcpy(font.logfont.data(), g_mem + p, 28);
    for (uint32_t i = 0; i < 31 && rd8(p + 28 + i); ++i) {
        uint16_t ch = rd8(p + 28 + i);
        memcpy(font.logfont.data() + 28 + 2 * i, &ch, 2);
    }
    set_eax(c, make_object(font));
}
// TEXTMETRICW is 60 bytes on x86: eleven DWORDs, four UTF-16
// characters, five BYTE fields and three bytes of trailing padding.
void metrics(uint32_t out, int32_t scale, uint32_t weight) {
    memset(g_mem + out, 0, 60);
    wr32(out, 16u * scale);
    wr32(out + 4, 13u * scale);
    wr32(out + 8, 3u * scale);
    wr32(out + 20, 8u * scale);
    wr32(out + 24, 8u * scale);
    wr32(out + 28, weight);
    wr32(out + 36, 96);
    wr32(out + 40, 96);
    wr16(out + 44, 32);
    wr16(out + 46, 127);
    wr16(out + 48, '?');
    wr16(out + 50, ' ');
    wr8(out + 55, 0x30);
    wr8(out + 56, 0);
}
// All wide text entry points share these bitmap glyphs and DC colour/clip rules.
void glyph(uint32_t hdc, int64_t x, int64_t y, uint32_t ch, bool underline = false) {
    auto *dc = dc_of(hdc);
    if (ch < 32 || ch > 127)
        ch = '?';
    int64_t scale = font_scale(hdc), width = 8 * scale, height = 16 * scale;
    Rect clip = clip_box(hdc);
    uint32_t fg = argb(dc->text_color), bg = argb(dc->bk_color);
    for (int64_t yy = std::max<int64_t>(y, clip.t); yy < std::min<int64_t>(y + height, clip.b);
         ++yy)
        for (int64_t xx = std::max<int64_t>(x, clip.l); xx < std::min<int64_t>(x + width, clip.r);
             ++xx) {
            uint8_t row = recomp_font::font8x16[ch - 32][size_t((yy - y) / scale)];
            if ((row & (0x80 >> ((xx - x) / scale))) || (underline && yy - y >= 14 * scale))
                write_pixel(hdc, xx, yy, fg);
            else if (dc->bk_mode == 2)
                write_pixel(hdc, xx, yy, bg);
        }
}
// How one DC's text is measured and drawn: from the registered TrueType face
// its font names, or from the bitmap cells.
struct Pen {
    const TrueTypeFace *face = nullptr;
    double scale = 0;
    int64_t cells = 1;
    TrueTypeMetrics metrics;
    int64_t advance(uint32_t ch) const {
        return face ? truetype_advance(face, scale, ch) : 8 * cells;
    }
    int64_t height() const {
        return face ? metrics.height : 16 * cells;
    }
    void draw(uint32_t hdc, int64_t x, int64_t y, uint32_t ch, bool underline) const {
        if (!face) {
            glyph(hdc, x, y, ch, underline);
            return;
        }
        auto *dc = dc_of(hdc);
        if (!dc)
            return;
        const uint32_t fg = argb(dc->text_color) & 0xffffffu;
        const int64_t width = advance(ch);
        if (dc->bk_mode == 2)
            fill(hdc, {int32_t(x), int32_t(y), int32_t(x + width), int32_t(y + metrics.height)},
                 argb(dc->bk_color));
        const TrueTypeGlyph &g = truetype_glyph(face, scale, ch);
        const int64_t baseline = y + metrics.ascent;
        for (int32_t j = 0; j < g.h; ++j)
            for (int32_t i = 0; i < g.w; ++i)
                if (uint8_t cover = g.coverage[size_t(j) * size_t(g.w) + size_t(i)])
                    write_pixel(hdc, x + g.x + i, baseline + g.y + j, (uint32_t(cover) << 24) | fg,
                                true);
        if (underline)
            for (int64_t yy = baseline + 1;
                 yy <= baseline + std::max<int64_t>(1, metrics.height / 16); ++yy)
                for (int64_t xx = x; xx < x + width; ++xx)
                    write_pixel(hdc, xx, yy, 0xff000000u | fg);
    }
};
Pen pen_of(uint32_t hdc) {
    Pen pen;
    pen.cells = font_scale(hdc);
    auto *dc = dc_of(hdc);
    auto it = objects().find(dc ? dc->font : 0);
    if (it == objects().end() || it->second.kind != Object::Font)
        return pen;
    std::string family;
    for (size_t i = 0; i < 32; ++i) {
        uint16_t unit =
            uint16_t(it->second.logfont[28 + 2 * i] | (it->second.logfont[29 + 2 * i] << 8));
        if (!unit)
            break;
        family.push_back(unit < 128 ? char(unit) : '?');
    }
    if (family.empty())
        return pen;
    pen.face = truetype_find(family);
    if (!pen.face) {
        // A face every Windows has and the program never supplied: what it
        // gets with no font of its own set, from the VCL's default font.
        const int32_t weight = int32_t(font_word(it->second, 16));
        pen.face = truetype_windows_substitute(family, weight);
        if (pen.face)
            log_once(("gdi-font:" + family).c_str(),
                     "gdi: \"%s\" is a Windows font the program did not register; drawing it "
                     "with the bundled Open Sans",
                     family.c_str());
    }
    if (!pen.face) {
        // Any other face: the host's installed copy of it, if it has one.
        std::string path;
        pen.face = truetype_host_face(family, int32_t(font_word(it->second, 16)), &path);
        if (pen.face)
            log_once(("gdi-host-font:" + family).c_str(),
                     "gdi: \"%s\" is drawn with the host's %s", family.c_str(), path.c_str());
    }
    if (!pen.face)
        return pen;
    pen.scale = truetype_scale(pen.face, int32_t(font_word(it->second, 0)));
    pen.metrics = truetype_metrics(pen.face, pen.scale);
    return pen;
}
// TEXTMETRICW of the font selected into hdc, into the 60 bytes at out.
bool fill_metrics(uint32_t hdc, uint32_t out) {
    auto *dc = dc_of(hdc);
    if (!dc || !out || !gm_valid(out, 60))
        return false;
    auto font = objects().find(dc->font);
    uint32_t weight = font == objects().end() ? 400 : font_word(font->second, 16);
    if (!weight)
        weight = 400;
    metrics(out, font_scale(hdc), weight);
    const Pen pen = pen_of(hdc);
    if (pen.face) {
        wr32(out, uint32_t(pen.metrics.height));
        wr32(out + 4, uint32_t(pen.metrics.ascent));
        wr32(out + 8, uint32_t(pen.metrics.descent));
        wr32(out + 12, uint32_t(pen.metrics.internal));
        wr32(out + 20, uint32_t(pen.metrics.average));
        wr32(out + 24, uint32_t(pen.metrics.maximum));
    }
    return true;
}
void get_metrics(X86 *c) {
    set_eax(c, fill_metrics(arg(c, 0), arg(c, 1)) ? 1 : 0);
}
// TEXTMETRICA is the wide record with its four characters as bytes: eleven
// DWORDs, tmFirstChar..tmBreakChar, five BYTE fields, 56 bytes in all.
void get_metrics_a(X86 *c) {
    uint32_t out = arg(c, 1);
    uint32_t wide = out && gm_valid(out, 56) ? heap_alloc(60, true) : 0;
    bool ok = wide && fill_metrics(arg(c, 0), wide);
    if (ok) {
        memcpy(g_mem + out, g_mem + wide, 44);
        for (uint32_t i = 0; i < 4; ++i)
            wr8(out + 44 + i, uint8_t(std::min<uint32_t>(rd16(wide + 44 + 2 * i), 255)));
        memcpy(g_mem + out + 48, g_mem + wide + 52, 5);
        memset(g_mem + out + 53, 0, 3);
    }
    if (wide)
        heap_free(wide);
    set_eax(c, ok ? 1 : 0);
}
void extent(X86 *c) {
    uint32_t dc = arg(c, 0), text = arg(c, 1), n = arg(c, 2), out = arg(c, 3);
    if (!dc_of(dc) || !out || !gm_valid(out, 8) || n > GUEST_SIZE / 2 ||
        (n && (!text || !gm_valid(text, n * 2)))) {
        set_eax(c, 0);
        return;
    }
    const Pen pen = pen_of(dc);
    uint64_t width = 0;
    for (uint32_t i = 0; i < n; ++i)
        width += uint64_t(pen.advance(rd16(text + 2 * i)));
    if (width > INT_MAX) {
        set_eax(c, 0);
        return;
    }
    wr32(out, uint32_t(width));
    wr32(out + 4, uint32_t(pen.height()));
    set_eax(c, 1);
}
// GetCharacterPlacement for left-to-right text with no shaping: each
// character is its own glyph (its code, as the runtime draws it), in order,
// advanced by the DC's pen. GCP_RESULTS: lStructSize, lpOutString, lpOrder,
// lpDx, lpCaretPos, lpClass, lpGlyphs, nGlyphs, nMaxFit.
void character_placement(X86 *c, bool wide) {
    uint32_t hdc = arg(c, 0), text = arg(c, 1), n = arg(c, 2), max_extent = arg(c, 3),
             results = arg(c, 4), flags = arg(c, 5);
    uint32_t unit = wide ? 2 : 1;
    if (!dc_of(hdc) || n > GUEST_SIZE / 4 || (n && (!text || !gm_valid(text, n * unit))) ||
        (results && !gm_valid(results, 36))) {
        set_eax(c, 0);
        return;
    }
    const Pen pen = pen_of(hdc);
    auto ch = [&](uint32_t i) { return wide ? rd16(text + 2 * i) : uint32_t(rd8(text + i)); };
    uint64_t width = 0;
    uint32_t fit = n;
    std::vector<uint32_t> advance(n), caret(n);
    for (uint32_t i = 0; i < n; ++i) {
        advance[i] = uint32_t(pen.advance(ch(i)));
        caret[i] = uint32_t(width);
        if ((flags & 0x100000) && fit == n && width + advance[i] > max_extent) // GCP_MAXEXTENT
            fit = i;
        width += advance[i];
    }
    if (results) {
        uint32_t limit = rd32(results + 28); // nGlyphs in: the arrays' size
        uint32_t count = std::min(n, limit ? limit : n);
        uint32_t out = rd32(results + 4), order = rd32(results + 8), dx = rd32(results + 12),
                 carets = rd32(results + 16), klass = rd32(results + 20), glyphs = rd32(results + 24);
        for (uint32_t i = 0; i < count; ++i) {
            if (out)
                wide ? wr16(out + 2 * i, uint16_t(ch(i))) : wr8(out + i, uint8_t(ch(i)));
            if (order)
                wr32(order + 4 * i, i);
            if (dx)
                wr32(dx + 4 * i, advance[i]);
            if (carets)
                wr32(carets + 4 * i, caret[i]);
            if (klass)
                wr8(klass + i, 1); // GCPCLASS_LATIN
            if (glyphs)
                wr16(glyphs + 2 * i, uint16_t(ch(i)));
        }
        wr32(results + 28, count);
        wr32(results + 32, fit);
    }
    set_eax(c, (uint32_t(std::min<uint64_t>(width, 0xffff))) | (uint32_t(pen.height()) << 16));
}
// Character widths, for ID3DXFont's layout. A glyph index is the character
// code here (glyph_indices), so the ...I forms read the same table. With a
// TrueType face, A and C are the glyph's bearings; the bitmap cells have none.
struct Abc {
    int32_t a, b, c;
};
Abc abc_of(const Pen &pen, uint32_t ch) {
    const int32_t advance = int32_t(pen.advance(ch));
    if (!pen.face)
        return {0, advance, 0};
    const TrueTypeGlyph &g = truetype_glyph(pen.face, pen.scale, ch);
    if (g.w <= 0)
        return {0, std::max(advance, 0), 0};
    return {g.x, g.w, advance - g.x - g.w};
}
// (hdc, first, last, out): ABC records for first..last, or plain widths.
void char_abc_range(X86 *c, bool abc) {
    uint32_t hdc = arg(c, 0), first = arg(c, 1), last = arg(c, 2), out = arg(c, 3);
    uint32_t size = abc ? 12 : 4;
    if (!dc_of(hdc) || last < first || last - first > 0xffff || !out ||
        !gm_valid(out, (last - first + 1) * size)) {
        set_eax(c, 0);
        return;
    }
    const Pen pen = pen_of(hdc);
    for (uint32_t ch = first; ch <= last; ++ch, out += size) {
        Abc w = abc_of(pen, ch);
        if (abc) {
            wr32(out, uint32_t(w.a));
            wr32(out + 4, uint32_t(w.b));
            wr32(out + 8, uint32_t(w.c));
        } else {
            wr32(out, uint32_t(w.a + w.b + w.c));
        }
    }
    set_eax(c, 1);
}
void char_abc_widths(X86 *c) {
    char_abc_range(c, true);
}
void char_widths(X86 *c) {
    char_abc_range(c, false);
}
// (hdc, giFirst, cgi, pgi, out): the glyphs in pgi, or giFirst onwards when it is null.
void char_abc_indices(X86 *c, bool abc) {
    uint32_t hdc = arg(c, 0), first = arg(c, 1), n = arg(c, 2), pgi = arg(c, 3), out = arg(c, 4);
    uint32_t size = abc ? 12 : 4;
    if (!dc_of(hdc) || n > 0x10000 || !out || !gm_valid(out, n * size) || (pgi && !gm_valid(pgi, n * 2))) {
        set_eax(c, 0);
        return;
    }
    const Pen pen = pen_of(hdc);
    for (uint32_t i = 0; i < n; ++i, out += size) {
        Abc w = abc_of(pen, pgi ? rd16(pgi + 2 * i) : first + i);
        if (abc) {
            wr32(out, uint32_t(w.a));
            wr32(out + 4, uint32_t(w.b));
            wr32(out + 8, uint32_t(w.c));
        } else {
            wr32(out, uint32_t(w.a + w.b + w.c));
        }
    }
    set_eax(c, 1);
}
void char_abc_widths_i(X86 *c) {
    char_abc_indices(c, true);
}
void char_widths_i(X86 *c) {
    char_abc_indices(c, false);
}
// GLYPHSET: cbThis, flAccel, cGlyphsSupported, cRanges, then {WCHAR low; USHORT count}.
// One range, the Basic Multilingual Plane from U+0001.
void font_unicode_ranges(X86 *c) {
    uint32_t out = arg(c, 1);
    const uint32_t size = 20;
    if (!dc_of(arg(c, 0)) || (out && !gm_valid(out, size))) {
        set_eax(c, 0);
        return;
    }
    if (out) {
        wr32(out, size);
        wr32(out + 4, 0);
        wr32(out + 8, 0xffff);
        wr32(out + 12, 1);
        wr16(out + 16, 1);
        wr16(out + 18, 0xffff);
    }
    set_eax(c, size);
}
void character_placement_w(X86 *c) {
    character_placement(c, true);
}
void character_placement_a(X86 *c) {
    character_placement(c, false);
}
// A glyph index is the character code, as character_placement reports it.
void glyph_indices(X86 *c, bool wide) {
    uint32_t text = arg(c, 1), n = arg(c, 2), out = arg(c, 3);
    if (!out || n > GUEST_SIZE / 4 || !gm_valid(out, n * 2) || (n && !text)) {
        set_eax(c, 0xffffffffu); // GDI_ERROR
        return;
    }
    for (uint32_t i = 0; i < n; ++i)
        wr16(out + 2 * i, wide ? rd16(text + 2 * i) : uint16_t(rd8(text + i)));
    set_eax(c, n);
}
void glyph_indices_w(X86 *c) {
    glyph_indices(c, true);
}
void glyph_indices_a(X86 *c) {
    glyph_indices(c, false);
}
// No font here needs reordering, glyph substitution or kerning.
void font_language_info(X86 *c) {
    set_eax(c, 0);
}
// MM_TEXT is the only mapping mode drawing honours; another is remembered by
// no one and logged once.
void set_map_mode(X86 *c) {
    if (arg(c, 1) != 1)
        log_once("SetMapMode", "SetMapMode(%u): only MM_TEXT is drawn", arg(c, 1));
    set_eax(c, 1); // the previous mode, MM_TEXT
}
void text_out(X86 *c) {
    uint32_t hdc = arg(c, 0), flags = arg(c, 3), rp = arg(c, 4), text = arg(c, 5), n = arg(c, 6),
             dx = arg(c, 7);
    // With no rectangle, ETO_OPAQUE and ETO_CLIPPED have nothing to act on and
    // are ignored, as Windows does. D3DX9's ID3DXFont draws every glyph so,
    // and refusing the call left Gothic 3 with an invisible font.
    if (!rp)
        flags &= ~6u;
    auto *dc = dc_of(hdc);
    if (recomp_env("TRACE_GDI") && text && n && n < 256 && gm_valid(text, n * 2)) {
        std::string chars;
        for (uint32_t i = 0; i < n; ++i) {
            const uint16_t ch = rd16(text + 2 * i);
            chars += ch >= 32 && ch < 127 ? (char)ch : '?';
        }
        LOGW("gdi: ExtTextOutW hdc=%08x at %d,%d flags=%08x font=%08x \"%s\"", hdc, (int32_t)arg(c, 1),
             (int32_t)arg(c, 2), flags, dc ? dc->font : 0, chars.c_str());
    }
    int w, h;
    if (!dc || !dc_size(hdc, &w, &h) || n > GUEST_SIZE / 4 ||
        (n && (!text || !gm_valid(text, n * 2))) || (dx && !gm_valid(dx, n * 4)) ||
        ((flags & 6) && (!rp || !gm_valid(rp, 16)))) {
        if (recomp_env("TRACE_GDI"))
            LOGW("gdi: ExtTextOutW refused (dc %d, size %d, rect %08x)", dc != nullptr, dc && dc_size(hdc, &w, &h), rp);
        set_eax(c, 0);
        return;
    }
    bool old_clipped = dc->clipped;
    std::vector<Rect> old_clip = dc->clip;
    Rect rect{};
    if (flags & 6)
        rect = {int32_t(rd32(rp)), int32_t(rd32(rp + 4)), int32_t(rd32(rp + 8)),
                int32_t(rd32(rp + 12))};
    if (flags & 4) {
        Rect cut = to_device(hdc, rect);
        std::vector<Rect> source = dc->clipped ? dc->clip : std::vector<Rect>{{0, 0, w, h}};
        dc->clip.clear();
        dc->clipped = true;
        for (Rect r : source) {
            Rect hit{std::max(r.l, cut.l), std::max(r.t, cut.t), std::min(r.r, cut.r),
                     std::min(r.b, cut.b)};
            if (hit.l < hit.r && hit.t < hit.b)
                dc->clip.push_back(hit);
        }
    }
    uint32_t bg = argb(dc->bk_color);
    if (flags & 2)
        fill(hdc, rect, bg);
    const Pen pen = pen_of(hdc);
    int64_t x = int32_t(arg(c, 1)), y = int32_t(arg(c, 2));
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t unit = rd16(text + 2 * i);
        pen.draw(hdc, x, y, unit, false);
        x += dx ? int32_t(rd32(dx + 4 * i)) : pen.advance(unit);
    }
    dc->clip = std::move(old_clip);
    dc->clipped = old_clipped;
    if (recomp_env("TRACE_GDI")) {
        uint32_t lit = 0, sample = 0;
        for (int yy = 0; yy < h; ++yy)
            for (int xx = 0; xx < w; ++xx) {
                uint32_t px = 0;
                if (read_pixel(hdc, xx, yy, &px, true) && (px & 0xffffffu)) {
                    ++lit;
                    sample = px;
                }
            }
        LOGW("gdi: ExtTextOutW left %u of %dx%d pixels lit (one: %08x), face %s", lit, w, h, sample,
             pen.face ? "TrueType" : "bitmap cells");
    }
    set_eax(c, 1);
}
// The fonts are kept, so a DC whose font names one of them draws with it. Data
// the rasterizer cannot read still gets a handle, as it always has here.
void memory_font(X86 *c) {
    uint32_t data = arg(c, 0), size = arg(c, 1), count = arg(c, 3);
    if (count && !gm_valid(count, 4)) {
        set_eax(c, 0);
        return;
    }
    uint32_t fonts =
        data && size && gm_valid(data, size) ? truetype_add_memory(g_mem + data, size) : 0;
    Object font;
    font.kind = Object::Font;
    uint32_t handle = make_object(font);
    if (count)
        wr32(count, std::max<uint32_t>(fonts, 1));
    set_eax(c, handle);
}
// AddFontResourceEx{A,W}(name, flags, reserved) and AddFontResourceA(name): a
// font file the program ships (Gothic 3's Gothic3.ttf), read through the
// game's file tiers and registered as AddFontMemResourceEx would. Returns the
// number of fonts added, 0 when the file is not there or is no font.
static uint32_t add_font_file(const std::string &name) {
    char path[1024];
    if (name.empty() || !recomp_readable_path(name.c_str(), path, sizeof path))
        return 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    std::vector<uint8_t> data;
    uint8_t block[65536];
    size_t n;
    while ((n = fread(block, 1, sizeof block, f)) > 0)
        data.insert(data.end(), block, block + n);
    fclose(f);
    uint32_t added = truetype_add_memory(data.data(), data.size());
    LOGV("gdi: font file %s -> %u fonts", name.c_str(), added);
    return added;
}
void file_font_a(X86 *c) {
    set_eax(c, add_font_file(gm_str(arg(c, 0))));
}
void file_font_w(X86 *c) {
    set_eax(c, add_font_file(gm_wstr(arg(c, 0))));
}
// The faces stay registered: nothing here unloads a font a DC may still use.
void remove_font_file(X86 *c) {
    set_eax(c, 1);
}
void enumerate(X86 *c) {
    uint32_t cb = arg(c, 2), param = arg(c, 3);
    if (!dc_of(arg(c, 0)) || !cb) {
        set_eax(c, 0);
        return;
    }
    uint32_t data = heap_alloc(92 + 60, true);
    if (!data) {
        set_eax(c, 0);
        return;
    }
    wr32(data, 16);
    wr32(data + 4, 8);
    wr32(data + 16, 400);
    wr8(data + 27, 0x30);
    gm_put_wstr(data + 28, "recomp", 32);
    metrics(data + 92, 1, 400);
    uint32_t result = guest_call(c, cb, data, data + 92, 1, param);
    heap_free(data);
    set_eax(c, result);
}
void unsupported(X86 *c) {
    set_eax(c, 0);
}
} // namespace
namespace gdi {
// DrawText's basic VCL layout uses the same fixed font as ExtTextOutW.
// Measurement must use that font too; CALCRECT never mutates canvas pixels.
uint32_t draw_text(uint32_t hdc, uint32_t text, uint32_t count, uint32_t rp, uint32_t flags) {
    auto *dc = dc_of(hdc);
    if (recomp_env("TRACE_GDI")) {
        int dw = -1, dh = -1;
        dc_size(hdc, &dw, &dh);
        LOGW("gdi: DrawText hdc=%08x(%dx%d)%s flags=%08x \"%s\"", hdc, dw, dh,
             dc ? "" : " NO SUCH DC", flags, gm_wstr(text, 64).c_str());
    }
    if (!dc || !rp || !gm_valid(rp, 16) || !text)
        return 0;
    if (count == UINT32_MAX) {
        count = 0;
        while (uint64_t(text) + 2ull * count + 2 <= GUEST_SIZE && gm_valid(text + count * 2, 2) &&
               rd16(text + count * 2))
            ++count;
    }
    if (!count || count > GUEST_SIZE / 2 || !gm_valid(text, count * 2))
        return 0;
    Rect rect{int32_t(rd32(rp)), int32_t(rd32(rp + 4)), int32_t(rd32(rp + 8)),
              int32_t(rd32(rp + 12))};
    const Pen pen = pen_of(hdc);
    const int64_t ch = pen.height();
    struct Cell {
        uint16_t ch;
        bool underline;
    };
    std::vector<std::vector<Cell>> lines(1);
    auto width_of = [&](const std::vector<Cell> &line) {
        int64_t width = 0;
        for (Cell cell : line)
            width += pen.advance(cell.ch);
        return width;
    };
    bool prefix = false;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t unit = rd16(text + 2 * i);
        if (!(flags & 0x800) && unit == '&') { // DT_NOPREFIX
            if (i + 1 < count && rd16(text + 2 * (i + 1)) == '&')
                ++i;
            else {
                prefix = true;
                continue;
            }
        }
        if (unit == '\r' || unit == '\n') {
            if (unit == '\r' && i + 1 < count && rd16(text + 2 * (i + 1)) == '\n')
                ++i;
            if (!(flags & 0x20)) {
                lines.emplace_back();
                prefix = false;
                continue;
            }
            unit = ' ';
        }
        if (unit == '\t' && (flags & 0x40)) {
            size_t spaces = 8 - lines.back().size() % 8;
            lines.back().insert(lines.back().end(), spaces, Cell{' ', false});
        } else
            lines.back().push_back({unit, prefix && !(flags & 0x100000)});
        prefix = false;
    }
    if ((flags & 0x10) && !(flags & 0x20) && rect.r > rect.l) { // DT_WORDBREAK
        const int64_t limit = int64_t(rect.r) - rect.l;
        for (size_t i = 0; i < lines.size(); ++i) {
            auto &line = lines[i];
            size_t cols = 0; // how many characters fit the width
            for (int64_t used = 0; cols < line.size(); ++cols) {
                used += pen.advance(line[cols].ch);
                if (used > limit)
                    break;
            }
            if (!cols || cols >= line.size())
                continue;
            size_t split = std::min(cols, line.size() - 1);
            while (split && line[split].ch != ' ')
                --split;
            if (!split) { // A long word is never split in the middle.
                split = cols;
                while (split < line.size() && line[split].ch != ' ')
                    ++split;
            }
            if (split < line.size()) {
                std::vector<Cell> tail(line.begin() + split + 1, line.end());
                line.resize(split);
                lines.insert(lines.begin() + i + 1, std::move(tail));
            }
        }
    }
    int64_t height = ch * lines.size(), maxwidth = 0;
    for (const auto &line : lines)
        maxwidth = std::max(maxwidth, width_of(line));
    if (height > INT_MAX || maxwidth > INT_MAX)
        return 0;
    if (flags & 0x400) {
        wr32(rp + 8, uint32_t(int64_t(rect.l) + maxwidth));
        wr32(rp + 12, uint32_t(int64_t(rect.t) + height));
        return uint32_t(height);
    }
    int w, h;
    if (!dc_size(hdc, &w, &h))
        return 0;
    bool old_clipped = dc->clipped;
    std::vector<Rect> old_clip = dc->clip;
    if (!(flags & 0x100)) { // DT_NOCLIP
        Rect cut = to_device(hdc, rect);
        std::vector<Rect> source = dc->clipped ? dc->clip : std::vector<Rect>{{0, 0, w, h}};
        dc->clip.clear();
        dc->clipped = true;
        for (Rect r : source) {
            Rect hit{std::max(r.l, cut.l), std::max(r.t, cut.t), std::min(r.r, cut.r),
                     std::min(r.b, cut.b)};
            if (hit.l < hit.r && hit.t < hit.b)
                dc->clip.push_back(hit);
        }
    }
    int64_t y = rect.t;
    if (flags & 0x20) {
        if (flags & 4)
            y += (int64_t(rect.b) - rect.t - height) / 2;
        else if (flags & 8)
            y = int64_t(rect.b) - height;
    }
    for (const auto &line : lines) {
        int64_t x = rect.l, width = width_of(line);
        if (flags & 1)
            x += (int64_t(rect.r) - rect.l - width) / 2;
        else if (flags & 2)
            x = int64_t(rect.r) - width;
        for (Cell cell : line) {
            pen.draw(hdc, x, y, cell.ch, cell.underline);
            x += pen.advance(cell.ch);
        }
        y += ch;
    }
    dc->clip = std::move(old_clip);
    dc->clipped = old_clipped;
    return uint32_t(y - rect.t);
}
void register_text() {
#define G(n, a, f)                                                                                 \
    {                                                                                              \
        "GDI32.dll", n, a, f                                                                       \
    }
    static const ImportShim shims[] = {G("CreateFontIndirectW", 1, create_font),
                                       G("ExtTextOutW", 8, text_out),
                                       G("GetTextExtentPoint32W", 4, extent),
                                       G("GetTextExtentPointW", 4, extent),
                                       G("GetCharacterPlacementW", 6, character_placement_w),
                                       G("GetCharacterPlacementA", 6, character_placement_a),
                                       G("GetGlyphIndicesW", 5, glyph_indices_w),
                                       G("GetGlyphIndicesA", 5, glyph_indices_a),
                                       G("GetFontLanguageInfo", 1, font_language_info),
                                       G("SetMapMode", 2, set_map_mode),
                                       G("GetTextMetricsW", 2, get_metrics),
                                       G("GetTextMetricsA", 2, get_metrics_a),
                                       G("GetCharABCWidthsW", 4, char_abc_widths),
                                       G("GetCharABCWidthsA", 4, char_abc_widths),
                                       G("GetCharWidth32W", 4, char_widths),
                                       G("GetCharWidth32A", 4, char_widths),
                                       G("GetCharWidthW", 4, char_widths),
                                       G("GetCharWidthA", 4, char_widths),
                                       G("GetCharABCWidthsI", 5, char_abc_widths_i),
                                       G("GetCharWidthI", 5, char_widths_i),
                                       G("GetFontUnicodeRanges", 2, font_unicode_ranges),
                                       G("CreateFontIndirectA", 1, create_font_a),
                                       G("AddFontResourceExA", 3, file_font_a),
                                       G("AddFontResourceExW", 3, file_font_w),
                                       G("AddFontResourceA", 1, file_font_a),
                                       G("RemoveFontResourceExA", 3, remove_font_file),
                                       G("RemoveFontResourceExW", 3, remove_font_file),
                                       G("AddFontMemResourceEx", 4, memory_font),
                                       G("EnumFontsW", 4, enumerate),
                                       G("EnumFontFamiliesExW", 5, enumerate),
                                       G("CreateEnhMetaFileW", 4, unsupported),
                                       G("CreateEnhMetaFileA", 4, unsupported),
                                       G("CloseEnhMetaFile", 1, unsupported),
                                       G("DeleteEnhMetaFile", 1, unsupported),
                                       G("GetEnhMetaFileW", 1, unsupported),
                                       G("GetEnhMetaFileA", 1, unsupported),
                                       G("GetEnhMetaFileBits", 3, unsupported),
                                       G("GetEnhMetaFileHeader", 3, unsupported),
                                       G("GetEnhMetaFileDescriptionW", 3, unsupported),
                                       G("GetEnhMetaFileDescriptionA", 3, unsupported),
                                       G("GetEnhMetaFilePaletteEntries", 3, unsupported),
                                       G("GetEnhMetaFilePixelFormat", 3, unsupported),
                                       G("SetEnhMetaFileBits", 2, unsupported),
                                       G("SetWinMetaFileBits", 4, unsupported),
                                       G("GetWinMetaFileBits", 5, unsupported),
                                       G("PlayEnhMetaFile", 3, unsupported),
                                       G("CopyEnhMetaFileW", 2, unsupported),
                                       G("CreateDCW", 4, unsupported),
                                       G("CreateICW", 4, unsupported),
                                       G("StartDocW", 2, unsupported),
                                       G("EndDoc", 1, unsupported),
                                       G("StartPage", 1, unsupported),
                                       G("EndPage", 1, unsupported),
                                       G("AbortDoc", 1, unsupported),
                                       G("SetAbortProc", 2, unsupported)};
#undef G
    imports_register(shims, sizeof(shims) / sizeof(shims[0]));
}
} // namespace gdi
