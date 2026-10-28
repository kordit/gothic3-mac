// launcher_ui_mods.cpp - the launcher's "Mody (Sefaris)" screen: a sidebar
// (search, categories, presets), a scrolling grid of cards with covers, the
// selected mod's description, and active-mods.json at Play. The rules live in
// launcher_mods.cpp; this is what the player sees.
#include "launcher_ui.h"

#include "launcher_look.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace launcher {
namespace {

std::string home_relative(const std::string &path) {
    const char *home = getenv("HOME");
    if (home && strlen(home) > 1 && path.compare(0, strlen(home), home) == 0 &&
        (path.size() == strlen(home) || path[strlen(home)] == '/'))
        return "~" + path.substr(strlen(home));
    return path;
}

std::string lower(std::string s) {
    for (char &c : s)
        c = char(std::tolower((unsigned char)c));
    return s;
}

// A title as the player reads it: '_' as a space.
std::string readable(std::string t) {
    for (char &ch : t)
        if (ch == '_')
            ch = ' ';
    return t;
}

constexpr int kCategoryAll = 0, kCategoryEnabled = 1;

// The mod screen's layout in points.
constexpr float kTop = 76, kBottom = 92, kSide = 236, kGap = 22, kCardGap = 18, kCardPad = 10;
constexpr float kCardMin = 176, kCardMax = 232;
constexpr float kToggle = 26;

} // namespace

std::string Launcher::choice_file() const {
    return info_.profile_dir + "/launcher-mods.json";
}

void Launcher::load_mods(const std::string &root) {
    mods_ = load_mod_catalog(root);
    // Defaults the player never chose keep only what this catalog has.
    if (default_choice_)
        sel_.ids.erase(std::remove_if(sel_.ids.begin(), sel_.ids.end(),
                                      [&](const std::string &id) {
                                          return std::none_of(
                                              mods_.mods.begin(), mods_.mods.end(),
                                              [&](const ModInfo &m) { return m.id == id; });
                                      }),
                       sel_.ids.end());
    for (ModInfo &m : mods_.mods) {
        if (m.script_dll.empty())
            continue;
        const size_t slash = m.script_dll.find_last_of("/\\");
        const std::string base = lower(slash == std::string::npos ? m.script_dll : m.script_dll.substr(slash + 1));
        for (const std::string &n : spec_.native_dlls)
            m.builtin = m.builtin || lower(n) == base;
        for (const std::string &n : spec_.native_mod_dlls)
            m.native = m.native || lower(n) == base;
    }
    mods_loaded_ = true;
    mod_root_ = root;
    covers_.clear();
    cover_order_.clear();
    no_cover_.clear();
    detail_cover_for_ = -1;
    mod_selected_ = -1;
    readme_.clear();
    grid_scroll_ = grid_target_ = 0;
    detail_scroll_ = detail_target_ = 0;
    const std::vector<int> v = visible_mods();
    if (!v.empty()) {
        mod_selected_ = v.front();
        readme_ = load_readme(mods_.mods[size_t(mod_selected_)].readme);
    }
    mod_notice_.clear();
    if (!mods_.found)
        mod_notice_ = "Nie znaleziono " + home_relative(root) + "/Mods.";
}

void Launcher::select_tile(int idx) {
    if (idx < 0 || idx >= int(mods_.mods.size()))
        return;
    mod_selected_ = idx;
    readme_ = load_readme(mods_.mods[size_t(idx)].readme);
    detail_scroll_ = detail_target_ = 0;
    dirty_ = true;
}

bool Launcher::select_mod(const std::string &text) {
    if (!mods_loaded_)
        load_mods(mod_root_);
    const std::string want = lower(ascii_text(text));
    for (size_t i = 0; i < mods_.mods.size(); ++i)
        if (lower(ascii_text(mods_.mods[i].title)).find(want) != std::string::npos) {
            select_tile(int(i));
            reveal_selected_ = true;
            rebuild();
            for (size_t b = 0; b < buttons_.size(); ++b)
                if (buttons_[b].id == kModTile + int(i))
                    focus_ = int(b);
            return true;
        }
    return false;
}

void Launcher::open_mods() {
    if (!mods_loaded_)
        load_mods(mod_root_);
    screen_ = Screen::Mods;
    confirm_play_ = false;
    search_active_ = false;
    rebuild();
    // The focus starts on the selected card.
    focus_ = 0;
    for (size_t b = 0; b < buttons_.size(); ++b)
        if (buttons_[b].id == kModTile + mod_selected_)
            focus_ = int(b);
}

// The view of the chosen mods in the profile (tools/g3mods.py build), right
// after active-mods.json: the player starts the app alone, without Python.
void Launcher::build_view() {
    const std::string data = status_.root.empty() ? std::string() : find_name(status_.root, "Data");
    const ModView v = build_mod_view(info_.profile_dir, data.empty() ? "" : status_.root + "/" + data);
    fprintf(stderr, "[launcher] mods-view: %zu links, %zu files left out, %zu errors\n",
            v.links.size(), v.skipped.size(), v.errors.size());
    for (const std::string &e : v.errors)
        fprintf(stderr, "[launcher] mods-view: %s\n", e.c_str());
}

bool Launcher::write_mods_for_play() {
    if (spec_.mod_root.empty())
        return true;
    if (!mods_loaded_ && !have_choice_) {
        build_view(); // the player never chose: active-mods.json stays as it is
        return true;
    }
    if (!mods_loaded_)
        load_mods(mod_root_);
    if (screen_ == Screen::Mods && !confirm_play_) {
        const std::vector<ModProblem> problems = mod_problems(mods_, sel_);
        if (!problems.empty()) {
            confirm_play_ = true;
            mod_notice_ = "Zaznaczenie ma ostrzeżenia (" + std::to_string(problems.size()) +
                          "). Naciśnij „Graj mimo to”, żeby grać tak, jak jest.";
            return false;
        }
    }
    save_mod_choice(choice_file(), mod_root_, sel_);
    have_choice_ = true;
    if (!write_active_mods(info_.profile_dir + "/active-mods.json", mods_, sel_)) {
        mod_notice_ = "Nie udało się zapisać active-mods.json w " + info_.profile_dir;
        return screen_ != Screen::Mods;
    }
    fprintf(stderr, "[launcher] active-mods.json: %zu mods, preset %s\n",
            mod_active(mods_, sel_).size(), sel_.preset.empty() ? "none" : sel_.preset.c_str());
    build_view();
    return true;
}

std::vector<int> Launcher::visible_mods() const {
    std::vector<int> out;
    const std::vector<std::string> cats = mods_.categories();
    const std::string want = lower(ascii_text(search_));
    for (size_t i = 0; i < mods_.mods.size(); ++i) {
        const ModInfo &m = mods_.mods[i];
        if (!want.empty() && lower(ascii_text(readable(m.title))).find(want) == std::string::npos &&
            lower(ascii_text(m.title)).find(want) == std::string::npos)
            continue;
        if (mod_category_ == kCategoryEnabled) {
            if (sel_.enabled(m.id))
                out.push_back(int(i));
        } else if (mod_category_ >= 2) {
            const size_t c = size_t(mod_category_ - 2);
            const std::string &cat = c < cats.size() ? cats[c] : std::string();
            const std::string have = m.category.empty() ? "Uncategorized" : m.category;
            if (have == cat)
                out.push_back(int(i));
        } else
            out.push_back(int(i));
    }
    // Enabled ones in load order (the order the game gets them); the rest by title.
    if (mod_category_ != kCategoryEnabled)
        std::stable_sort(out.begin(), out.end(), [&](int a, int b) {
            return lower(mods_.mods[size_t(a)].title) < lower(mods_.mods[size_t(b)].title);
        });
    return out;
}

namespace {
// Sefaris's category names, as the player reads them here.
std::string category_name(const std::string &c) {
    static const char *const names[][2] = {
        {"Animations", "Animacje"},      {"Balance", "Balans"},          {"Content", "Zawartość"},
        {"Gameplay", "Rozgrywka"},       {"Interface", "Interfejs"},     {"Patches", "Poprawki"},
        {"Sounds", "Dźwięki"},           {"Story", "Fabuła"},            {"Texture packs", "Paczki tekstur"},
        {"Textures/Models", "Tekstury i modele"}, {"Uncategorized", "Bez kategorii"}};
    for (const auto &n : names)
        if (c == n[0])
            return n[1];
    return c;
}
} // namespace

void Launcher::mods_buttons() {
    auto add = [&](int id, const std::string &label, bool enabled, int kind, Icon icon = Icon(-1)) {
        Button b{id, label, enabled, {}};
        b.kind = kind;
        b.icon = int(icon);
        buttons_.push_back(b);
    };
    add(kModsSearch, search_, true, kKindField, Icon::Search);
    const std::vector<std::string> cats = mods_.categories();
    add(kModsCategory + kCategoryAll, "Wszystkie\t" + std::to_string(mods_.mods.size()), true, kKindList);
    add(kModsCategory + kCategoryEnabled, "Włączone\t" + std::to_string(mod_active(mods_, sel_).size()), true,
        kKindList);
    for (size_t i = 0; i < cats.size(); ++i) {
        int n = 0;
        for (const ModInfo &m : mods_.mods)
            n += (m.category.empty() ? std::string("Uncategorized") : m.category) == cats[i];
        add(kModsCategory + 2 + int(i), category_name(cats[i]) + "\t" + std::to_string(n), true, kKindList);
    }
    add(kModsPresetNone, "Bez presetu", true, kKindList);
    for (size_t i = 0; i < mods_.presets.size(); ++i)
        add(kModsPresetPick + int(i), readable(mods_.presets[i].name), true, kKindList);
    for (int idx : visible_mods())
        add(kModTile + idx, mods_.mods[size_t(idx)].title, true, kKindTile);
    if (mod_selected_ >= 0 && mod_selected_ < int(mods_.mods.size())) {
        const ModInfo &m = mods_.mods[size_t(mod_selected_)];
        if (sel_.enabled(m.id))
            add(kModToggle, "Wyłącz", mod_required_by(mods_, sel_, m.id).empty(), kKindList);
        else
            add(kModToggle, "Włącz", mod_blocked(mods_, sel_, m.id).empty(), kKindList);
    }
    if (!spec_.mods_url.empty())
        add(kModsGet, "Skąd wziąć mody", true, kKindNormal, Icon::Document);
    add(kModsRoot, "Zmień katalog…", info_.can_pick_folder, kKindNormal, Icon::Folder);
    add(kModsBack, "Wróć", true, kKindNormal, Icon::Back);
    add(kModsPlay, confirm_play_ ? "Graj mimo to" : "Graj z modami", status_.state == State::Ready, kKindPrimary,
        Icon::Play);
}

void Launcher::wheel(int x, int y, double steps) {
    if (screen_ != Screen::Mods || steps == 0)
        return;
    const float delta = float(steps) * 72.0f; // points per wheel step
    if (detail_rect_.contains(x, y)) {
        const float max = std::max(0.0f, detail_content_ - float(detail_rect_.h) / d_);
        detail_target_ = std::clamp(detail_target_ + delta, 0.0f, max);
    } else if (grid_rect_.contains(x, y) || !detail_rect_.contains(x, y)) {
        const float max = std::max(0.0f, grid_content_ - float(grid_rect_.h) / d_);
        grid_target_ = std::clamp(grid_target_ + delta, 0.0f, max);
    }
    dirty_ = true;
}

void Launcher::type_text(const std::string &utf8) {
    if (!wants_text() || steam_type(utf8))
        return;
    for (char ch : utf8)
        if ((unsigned char)ch < 0x20)
            return;
    search_ += utf8;
    grid_scroll_ = grid_target_ = 0;
    rebuild();
}

void Launcher::erase_text() {
    if (!wants_text() || steam_erase() || search_.empty())
        return;
    size_t k = search_.size() - 1;
    while (k > 0 && (uint8_t(search_[k]) & 0xC0) == 0x80)
        --k;
    search_.erase(k);
    grid_scroll_ = grid_target_ = 0;
    rebuild();
}

Rect Launcher::tile_toggle(const Rect &tile) const {
    const float pad = kCardPad * d_;
    const float cover = (float(tile.w) - 2 * pad) * 312.0f / 315.0f;
    const float size = 34 * d_;
    const float cx = float(tile.x + tile.w) - pad - kToggle * d_ / 2;
    const float cy = float(tile.y) + pad + cover + 12 * d_ + kToggle * d_ / 2;
    return {int(cx - size / 2), int(cy - size / 2), int(size), int(size)};
}

// Keeps the focused card in view after an arrow key.
void Launcher::reveal_focus() {
    if (focus_ < 0 || focus_ >= int(buttons_.size()) || grid_rect_.empty())
        return;
    const Button &b = buttons_[size_t(focus_)];
    if (b.kind != kKindTile || b.rect.empty())
        return;
    const float top = float(b.rect.y - grid_rect_.y) / d_, bottom = top + float(b.rect.h) / d_;
    const float view = float(grid_rect_.h) / d_;
    const float max = std::max(0.0f, grid_content_ - view);
    if (top < 8)
        grid_target_ = std::clamp(grid_target_ + top - 8, 0.0f, max);
    else if (bottom > view - 8)
        grid_target_ = std::clamp(grid_target_ + bottom - view + 8, 0.0f, max);
}

// A card's cover at w x h, decoded a few per frame so the screen stays
// responsive; the least recently drawn ones go when too many are kept.
const Image *Launcher::cover_for(int idx, int w, int h, int *budget, bool *pending) {
    if (cover_w_ != w || cover_h_ != h) {
        covers_.clear();
        cover_order_.clear();
        cover_w_ = w;
        cover_h_ = h;
    }
    auto it = covers_.find(idx);
    if (it == covers_.end()) {
        const ModInfo &mod = mods_.mods[size_t(idx)];
        if (mod.cover.empty() || std::find(no_cover_.begin(), no_cover_.end(), idx) != no_cover_.end())
            return nullptr;
        if (*budget <= 0) {
            *pending = true;
            return nullptr;
        }
        --*budget;
        Image full;
        if (!platform_.decode_image(mod.cover, &full)) {
            no_cover_.push_back(idx);
            return nullptr;
        }
        it = covers_.emplace(idx, cover_fit(full, w, h, 0.5f, 0.5f)).first;
        if (covers_.size() > 72 && !cover_order_.empty()) {
            covers_.erase(cover_order_.front());
            cover_order_.erase(cover_order_.begin());
        }
    }
    cover_order_.erase(std::remove(cover_order_.begin(), cover_order_.end(), idx), cover_order_.end());
    cover_order_.push_back(idx);
    return &it->second;
}

void Launcher::draw_mods(Canvas &c) {
    Look &k = *look_;
    const int cw = c.width(), ch = c.height();
    const float W = float(cw) / d_, H = float(ch) / d_;
    c.reset_clip();
    k.backdrop(c, 1);
    auto px = [&](float pt) { return P(pt); };
    auto measure = [&](Font &f, int size) { return Measure([&f, size](const std::string &s) { return f.measure(s, size); }); };
    auto text = [&](Font &f, float x, float baseline, const std::string &s, float pt, Color col, float track = 0) {
        return f.draw(c, P(x), P(baseline), s, P(pt), col, track * d_);
    };
    // Narrow windows give up the sidebar's and the description's width first.
    const float M = std::clamp(W * 0.02f, 12.0f, 28.0f);
    const float side = std::min(kSide, std::max(150.0f, W * 0.2f));
    const float detail_w = std::min(380.0f, std::max(220.0f, W * 0.3f));
    const float top = kTop, bottom = H - kBottom;
    const float body_top = top + 4, body_h = std::max(40.0f, bottom - body_top - 14);

    // The header: the title, how many mods, the catalog and where it is.
    {
        const float base = 50;
        const int tw = k.display.measure("Mody", P(34));
        text(k.display, M + 1, base + 2, "Mody", 34, hex(0x000000, 160));
        text(k.display, M, base, "Mody", 34, pal::gold);
        int unsupported = 0, builtin = 0;
        for (const ModInfo &mod : mods_.mods) {
            unsupported += !mod.supported() && !mod.builtin;
            builtin += mod.builtin;
        }
        std::string info = "Sefaris  ·  " + std::to_string(mods_.mods.size()) + " " +
                           mods_word(mods_.mods.size()) + ", z tego " + std::to_string(unsupported) + " wymaga DLL";
        if (builtin)
            info += ", " + std::to_string(builtin) + " wbudowane w port";
        text(k.body, M + float(tw) / d_ + 18, base, info, 16, pal::dim);
    }
    // Rects, then drawing. Buttons by kind.
    const float field_h = 40, row_h = 34;
    float sy = body_top + 16;
    const Rect side_rect{P(M), P(body_top), P(side), P(body_h)};
    const Rect detail_rect{P(W - M - detail_w), P(body_top), P(detail_w), P(body_h)};
    const float gx = M + side + kGap, gw = std::max(80.0f, W - M - detail_w - kGap - gx);
    // The grid: as many columns as fit cards of kCardMin..kCardMax.
    const int cols = std::max(1, int((gw + kCardGap) / (kCardMin + kCardGap)));
    const float card_w = std::min(kCardMax, (gw - kCardGap * float(cols - 1)) / float(cols));
    const float cover = (card_w - 2 * kCardPad) * 312.0f / 315.0f;
    const float card_h = kCardPad + cover + 12 + 2 * 20 + 4 + 18 + kCardPad;
    const float grid_head = 44;
    const float grid_top = body_top + grid_head, grid_h = std::max(20.0f, body_top + body_h - grid_top);
    grid_rect_ = {P(gx - 6), P(grid_top), P(gw + 12), P(grid_h)};
    detail_rect_ = detail_rect;
    int tiles = 0;
    for (const Button &b : buttons_)
        tiles += b.kind == kKindTile;
    const int rows = (tiles + cols - 1) / cols;
    grid_content_ = float(rows) * (card_h + kCardGap) + 8;
    grid_target_ = std::clamp(grid_target_, 0.0f, std::max(0.0f, grid_content_ - grid_h));
    grid_scroll_ = std::clamp(grid_scroll_, 0.0f, std::max(0.0f, grid_content_ - grid_h));
    int tile_slot = 0;
    float bar_right = W - M;
    for (Button &b : buttons_) {
        if (b.id == kModsSearch) {
            b.rect = {P(M + 14), P(sy), P(side - 28), P(field_h)};
            sy += field_h + 22;
        } else if (b.kind == kKindList && b.id != kModToggle) {
            if (b.id == kModsPresetNone)
                sy += 30; // the presets' heading
            else if (b.id == kModsCategory + kCategoryAll)
                sy += 22; // the categories' heading
            // A long preset name takes two lines rather than losing its end.
            float rh = row_h;
            if (b.id >= kModsPresetPick && b.id < kModTile && k.body.measure(b.label, P(15)) > P(side - 16 - 28))
                rh = 52;
            b.rect = {P(M + 8), P(sy), P(side - 16), P(rh)};
            sy += rh;
        } else if (b.kind == kKindTile) {
            const int col = tile_slot % cols, row = tile_slot / cols;
            ++tile_slot;
            b.rect = {P(gx + float(col) * (card_w + kCardGap)),
                      P(grid_top + 4 + float(row) * (card_h + kCardGap) - grid_scroll_), P(card_w), P(card_h)};
        } else if (b.id == kModsPlay) {
            b.rect = {P(bar_right - 260), P(H - kBottom + 18), P(260), P(58)};
            bar_right -= 260 + 14;
        }
    }
    for (Button &b : buttons_)
        if (b.id == kModsBack)
            b.rect = {P(bar_right - 170), P(H - kBottom + 20), P(170), P(54)};
        else if (b.id == kModsRoot) {
            b.rect = {P(W - M - 200), P(20), P(200), P(42)};
        }
    // A card chosen from outside (a script) scrolls into view at once.
    if (reveal_selected_) {
        reveal_selected_ = false;
        for (Button &b : buttons_)
            if (b.id == kModTile + mod_selected_) {
                const float y = float(b.rect.y - P(grid_top)) / d_;
                float want = grid_scroll_;
                if (y < 0)
                    want += y - 8;
                else if (y + card_h > grid_h)
                    want += y + card_h - grid_h + 8;
                want = std::clamp(want, 0.0f, std::max(0.0f, grid_content_ - grid_h));
                const int shift = P(grid_scroll_) - P(want);
                grid_scroll_ = grid_target_ = want;
                for (Button &t : buttons_)
                    if (t.kind == kKindTile)
                        t.rect.y += shift;
                break;
            }
    }
    // The sidebar's rows shrink to the room there is.
    {
        const float room = body_top + body_h - 10;
        if (sy > room) {
            const float k2 = std::max(0.6f, (room - body_top) / (sy - body_top));
            for (Button &b : buttons_)
                if ((b.kind == kKindList && b.id != kModToggle) || b.id == kModsSearch) {
                    b.rect.y = P(body_top) + int(float(b.rect.y - P(body_top)) * k2);
                    b.rect.h = int(float(b.rect.h) * k2);
                }
        }
    }

    // The catalog's path left of "Zmień katalog…".
    {
        const std::string where = ellipsize_middle(home_relative(mod_root_), P(std::max(60.0f, W * 0.3f)),
                                                   measure(k.body, P(14)));
        const int ww = k.body.measure(where, P(14));
        k.body.draw(c, P(W - M - 200 - 16) - ww, P(46), where, P(14), pal::faint);
    }

    // The sidebar.
    c.shadow(side_rect, P(10), P(18), hex(0x000000, 120));
    c.fill_round(side_rect, P(10), pal::panel);
    c.stroke_round(side_rect, P(10), std::max(1, P(1)), pal::border);
    {
        c.set_clip(side_rect);
        bool cats_head = false, presets_head = false;
        for (size_t i = 0; i < buttons_.size(); ++i) {
            const Button &b = buttons_[i];
            const float h = hot(int(i));
            if (b.id == kModsSearch) {
                const Rect r = b.rect;
                c.fill_round(r, P(8), hex(0x0d0b09, 200));
                c.stroke_round(r, P(8), std::max(1, P(1)),
                               search_active_ ? pal::gold : mix(pal::border_hi, pal::gold_deep, h));
                draw_icon(c, Icon::Search, float(r.x) + float(P(18)), float(r.y) + float(r.h) / 2, float(P(18)),
                          search_active_ ? pal::gold : pal::dim);
                const int tpx = P(15);
                const int bl = r.y + r.h / 2 + (k.body.ascent(tpx) - k.body.descent(tpx)) / 2;
                if (search_.empty() && !search_active_)
                    k.body.draw(c, r.x + P(34), bl, "Szukaj modów…", tpx, pal::faint);
                else {
                    const std::string shown = search_;
                    const int tw = k.body.draw(c, r.x + P(34), bl, shown, tpx, pal::text);
                    if (search_active_)
                        c.fill({r.x + P(35) + tw, bl - k.body.ascent(tpx), std::max(1, P(1.5f)),
                                k.body.ascent(tpx) + k.body.descent(tpx)},
                               pal::gold);
                }
                if (focus_visible(int(i)))
                    c.stroke_round(r.inset(-P(3)), P(10), std::max(1, P(2)), pal::gold_hot);
                continue;
            }
            if (b.kind != kKindList || b.id == kModToggle)
                continue;
            const Rect r = b.rect;
            auto heading = [&](const char *t) {
                k.body.draw(c, r.x + P(10), r.y - P(9), t, P(12), pal::faint, 2.2f * d_);
            };
            if (b.id >= kModsCategory && b.id < kModsPresetPick && !cats_head) {
                cats_head = true;
                heading("KATEGORIE");
            }
            if ((b.id == kModsPresetNone) && !presets_head) {
                presets_head = true;
                heading("PRESETY");
            }
            const bool current = (b.id >= kModsCategory && b.id < kModsPresetPick && b.id - kModsCategory == mod_category_) ||
                                 (b.id == kModsPresetNone && sel_.preset.empty()) ||
                                 (b.id >= kModsPresetPick && b.id < kModTile &&
                                  size_t(b.id - kModsPresetPick) < mods_.presets.size() &&
                                  mods_.presets[size_t(b.id - kModsPresetPick)].name == sel_.preset);
            if (current || h > 0)
                c.fill_round(r, P(6), current ? hex(0xc9a24a, 34) : fade(hex(0xe8dcc4, 18), h));
            if (current)
                c.fill_round({r.x, r.y + P(7), P(3), r.h - P(14)}, P(1.5f), pal::gold);
            const size_t tab = b.label.find('\t');
            const std::string name = tab == std::string::npos ? b.label : b.label.substr(0, tab);
            const std::string count = tab == std::string::npos ? "" : b.label.substr(tab + 1);
            const int tpx = P(15);
            const int bl = r.y + r.h / 2 + (k.body.ascent(tpx) - k.body.descent(tpx)) / 2;
            int right = r.x + r.w - P(10);
            if (!count.empty()) {
                const int cpx = P(13);
                const int cwid = k.body.measure(count, cpx);
                k.body.draw(c, right - cwid, bl, count, cpx, current ? pal::gold : pal::dim);
                right -= cwid + P(8);
            }
            const Color nc = current ? pal::gold_hot : mix(pal::text, pal::gold_hot, h);
            const std::vector<std::string> nl = wrap_lines(name, right - (r.x + P(14)), r.h > P(44) ? 2 : 1,
                                                           measure(k.body, tpx), true);
            if (nl.size() > 1) {
                const int step = P(19);
                int ly = r.y + (r.h - step) / 2 + (k.body.ascent(tpx) - k.body.descent(tpx)) / 2;
                for (const std::string &l : nl) {
                    k.body.draw(c, r.x + P(14), ly, l, tpx, nc);
                    ly += step;
                }
            } else if (!nl.empty())
                k.body.draw(c, r.x + P(14), bl, nl[0], tpx, nc);
            if (focus_visible(int(i)))
                c.stroke_round(r, P(6), std::max(1, P(2)), pal::gold_hot);
        }
        c.reset_clip();
    }

    // The grid's heading: what is shown.
    {
        std::string what = "Wszystkie mody";
        const std::vector<std::string> cats = mods_.categories();
        if (mod_category_ == kCategoryEnabled)
            what = "Włączone, w kolejności ładowania";
        else if (mod_category_ >= 2 && size_t(mod_category_ - 2) < cats.size())
            what = category_name(cats[size_t(mod_category_ - 2)]);
        const int tw = text(k.strong, gx, body_top + 28, what, 25, pal::text);
        std::string n = std::to_string(tiles);
        if (!search_.empty())
            n += "  ·  „" + search_ + "”";
        text(k.body, gx + float(tw) / d_ + 14, body_top + 28, n, 17, pal::dim);
    }

    // A cover for a mod without one: a dark binding with a gold frame and the
    // rosette (the title is under the cover).
    auto placeholder = [&](Rect cr, const std::string &title, bool dll, int radius) {
        c.fill_round(cr, radius, hex(0x1b1611));
        c.gradient(cr.inset(P(2)), hex(0x2c2319, 200), hex(0x100d0a, 120));
        const Color frame = fade(dll ? pal::faint : pal::gold, 0.45f);
        const Rect f1 = cr.inset(std::max(P(6), cr.w / 22)), f2 = f1.inset(P(4));
        c.stroke_round(f1, P(3), std::max(1, P(1)), frame);
        c.stroke_round(f2, P(2), std::max(1, P(1)), fade(frame, 0.5f));
        for (int corner = 0; corner < 4; ++corner) {
            const float x = float(corner & 1 ? f1.x + f1.w : f1.x), y = float(corner & 2 ? f1.y + f1.h : f1.y);
            c.disc(x, y, float(P(3)), frame);
        }
        const int ros = cr.w * 46 / 100;
        const int ry = cr.y + (cr.h - ros) / 2 - (dll ? cr.h / 10 : 0);
        if (!k.rosette.rgba.empty()) {
            const Image &rimg = k.cached("rosette:" + std::to_string(ros), [&] { return resample(k.rosette, ros, ros); });
            ImageStyle rs;
            rs.opacity = dll ? 0.25f : 0.55f;
            rs.saturation = dll ? 0.0f : 1.0f;
            c.image(cr.x + (cr.w - ros) / 2, ry, ros, ros, rimg.rgba.data(), rs);
        }
    };
    // The cards.
    c.set_clip(grid_rect_);
    int budget = 4;
    bool pending = false;
    const std::vector<ModProblem> problems = mod_problems(mods_, sel_);
    for (size_t i = 0; i < buttons_.size(); ++i) {
        const Button &b = buttons_[i];
        if (b.kind != kKindTile)
            continue;
        const Rect r = b.rect;
        if (r.y + r.h < grid_rect_.y - P(20) || r.y > grid_rect_.y + grid_rect_.h + P(20))
            continue;
        const int idx = b.id - kModTile;
        const ModInfo &mod = mods_.mods[size_t(idx)];
        // A mod the port has redone natively runs always: on, and no DLL to wait for.
        const bool on = sel_.enabled(mod.id) || mod.builtin;
        const bool dll = !mod.supported() && !mod.builtin;
        const std::string blocked = on || dll ? std::string() : mod_blocked(mods_, sel_, mod.id);
        bool problem = false;
        for (const ModProblem &p : problems)
            problem = problem || (same_id(p.id, mod.id) && on);
        const bool selected = idx == mod_selected_;
        const float h = std::max(hot(int(i)), selected ? 0.0f : 0.0f);
        // The card: a panel, lifted a little under the pointer.
        const int lift = int(float(P(3)) * h);
        const Rect card{r.x, r.y - lift, r.w, r.h};
        c.shadow(card, P(10), P(14 + 6 * h), hex(0x000000, uint8_t(120 + 50 * h)));
        c.fill_round(card, P(10), selected ? pal::panel_hi : mix(pal::panel, pal::panel_hi, h));
        Color edge = mix(pal::border, pal::border_hi, h);
        int edge_w = std::max(1, P(1));
        if (problem || !blocked.empty()) {
            edge = problem ? pal::red : fade(pal::red, 0.6f);
            edge_w = std::max(1, P(2));
        } else if (on) {
            edge = pal::gold;
            edge_w = std::max(1, P(2));
        }
        if (selected) {
            // A conflict stays red; otherwise the selection is the brighter gold.
            if (!problem && blocked.empty())
                edge = on ? pal::gold_hot : mix(pal::gold_deep, pal::gold, 0.5f);
            edge_w = std::max(1, P(2.5f));
        }
        c.stroke_round(card, P(10), edge_w, edge);
        // The cover, or a placeholder in the launcher's colours.
        const int pad = P(kCardPad);
        const Rect cr{card.x + pad, card.y + pad, card.w - 2 * pad, P(cover)};
        const Image *img = cover_for(idx, cr.w, cr.h, &budget, &pending);
        ImageStyle st;
        st.radius = P(7);
        if (dll) {
            st.saturation = 0.12f;
            st.brightness = 0.5f;
        }
        if (img && img->w == cr.w)
            c.image(cr.x, cr.y, cr.w, cr.h, img->rgba.data(), st);
        else
            placeholder(cr, readable(mod.title), dll, P(7));
        if (dll || mod.builtin) {
            // A ribbon along the cover's lower edge: what is missing, and that it is coming.
            const int rh = P(46);
            const Rect band{cr.x - P(4), cr.y + cr.h - rh - P(8), cr.w + P(8), rh};
            c.shadow(band, P(2), P(8), hex(0x000000, 140));
            c.gradient(band, hex(0x3a2f22, 240), hex(0x261f17, 240));
            c.fill({band.x, band.y, band.w, std::max(1, P(1))}, fade(pal::gold, 0.7f));
            c.fill({band.x, band.y + band.h - std::max(1, P(1)), band.w, std::max(1, P(1))}, fade(pal::gold, 0.7f));
            const int rpx = P(15), spx = P(13.5f);
            const std::string top_line =
                ellipsize(mod.builtin ? "Wbudowany w port" : "Wymaga DLL", band.w - P(12), measure(k.strong, rpx));
            const int rw = k.strong.measure(top_line, rpx);
            k.strong.draw(c, band.x + (band.w - rw) / 2, band.y + P(20), top_line, rpx, pal::gold_hot);
            const std::string soon = mod.builtin ? "działa zawsze" : "obsługa wkrótce";
            const int sw = k.body.measure(soon, spx);
            k.body.draw(c, band.x + (band.w - sw) / 2, band.y + P(37), soon, spx, hex(0xcdbfa5));
        }
        // The title in two lines and the category; the switch on the right.
        const int tpx = P(15);
        const Rect tg = tile_toggle(r);
        const int text_w = card.w - 2 * pad - P(kToggle + 8);
        const std::vector<std::string> lines =
            wrap_lines(readable(mod.title), text_w, 2, measure(k.strong, tpx), true);
        int ty = cr.y + cr.h + P(12) + k.strong.ascent(tpx);
        for (const std::string &l : lines) {
            k.strong.draw(c, cr.x, ty, l, tpx, dll ? pal::faint : on ? pal::text : mix(pal::text, pal::dim, 0.25f));
            ty += P(20);
        }
        {
            std::string note = category_name(mod.category.empty() ? "Uncategorized" : mod.category);
            Color nc = pal::dim;
            if (!blocked.empty()) {
                note = "Konflikt z włączonym";
                nc = pal::red_text;
            } else if (problem) {
                note = "Ostrzeżenie";
                nc = pal::red_text;
            }
            const int npx = P(13);
            k.body.draw(c, cr.x, cr.y + cr.h + P(12) + P(44) + k.body.ascent(npx),
                        ellipsize(note, cr.w, measure(k.body, npx)), npx, nc);
        }
        if (!dll) {
            const float tcx = float(tg.x) + float(tg.w) / 2, tcy = float(tg.y - lift) + float(tg.h) / 2;
            const float rad = float(P(kToggle / 2));
            const bool over = pointer_x_ >= tg.x && pointer_x_ < tg.x + tg.w && pointer_y_ >= tg.y &&
                              pointer_y_ < tg.y + tg.h && !keyboard_;
            if (on) {
                c.disc(tcx, tcy, rad, over ? pal::gold_hot : pal::gold);
                draw_icon(c, Icon::Check, tcx, tcy, rad * 1.5f, pal::ink);
            } else {
                c.disc(tcx, tcy, rad, hex(0x0d0b09, 180));
                const int R = int(rad);
                c.stroke_round({int(tcx) - R, int(tcy) - R, 2 * R, 2 * R}, R, std::max(1, P(1.5f)),
                               !blocked.empty() ? fade(pal::red, 0.7f) : over ? pal::gold_hot : mix(pal::border_hi, pal::gold, h));
                if (over && blocked.empty())
                    draw_icon(c, Icon::Check, tcx, tcy, rad * 1.5f, fade(pal::gold, 0.6f));
            }
        }
        if (focus_visible(int(i)))
            c.stroke_round({card.x - P(4), card.y - P(4), card.w + P(8), card.h + P(8)}, P(13), std::max(1, P(2)),
                           pal::gold_hot);
    }
    if (!mods_.found)
        k.body.draw(c, P(gx), P(grid_top + 30), "Nie znaleziono katalogu modów Sefarisa: " + home_relative(mod_root_) + "/Mods.",
                    P(16), pal::amber);
    else if (tiles == 0)
        k.body.draw(c, P(gx), P(grid_top + 30), search_.empty() ? "Nic tu nie ma." : "Żaden mod nie pasuje do wyszukiwania.",
                    P(16), pal::dim);
    c.reset_clip();
    if (grid_scroll_ > 1) // cards slide under the heading rather than ending at a hard edge
        c.gradient({grid_rect_.x, grid_rect_.y, grid_rect_.w, P(16)}, hex(0x110e0b, 200), hex(0x110e0b, 0));
    // The grid's scroll bar.
    if (grid_content_ > grid_h + 1) {
        const float track_x = gx + gw + kGap / 2 - 2;
        const float frac = grid_h / grid_content_;
        const float thumb_h = std::max(90.0f, grid_h * frac);
        const float pos = (grid_h - thumb_h) * (grid_scroll_ / std::max(1.0f, grid_content_ - grid_h));
        c.fill_round({P(track_x - 1), P(grid_top), P(6), P(grid_h)}, P(3), hex(0x000000, 110));
        c.fill_round({P(track_x - 1), P(grid_top + pos), P(6), P(thumb_h)}, P(3), fade(pal::gold, 0.8f));
    }

    // The description panel.
    c.shadow(detail_rect, P(10), P(18), hex(0x000000, 130));
    c.fill_round(detail_rect, P(10), hex(0x1c1814, 235));
    c.stroke_round(detail_rect, P(10), std::max(1, P(1)), pal::border);
    {
        const Rect view = detail_rect.inset(P(1));
        c.set_clip(view);
        const float dx = W - M - detail_w + 20, dw = detail_w - 40;
        float dy = body_top + 20 - detail_scroll_;
        if (mod_selected_ >= 0 && mod_selected_ < int(mods_.mods.size())) {
            const ModInfo &mod = mods_.mods[size_t(mod_selected_)];
            const bool on = sel_.enabled(mod.id) || mod.builtin;
            const bool dll = !mod.supported() && !mod.builtin;
            // The big cover.
            const float cover_pt = std::min(dw, 260.0f);
            const int bw = P(cover_pt), bh = int(float(P(cover_pt)) * 312.0f / 315.0f);
            if (detail_cover_for_ != mod_selected_ || detail_cover_w_ != bw) {
                detail_cover_for_ = mod_selected_;
                detail_cover_w_ = bw;
                detail_cover_ = Image{};
                Image full;
                if (!mod.cover.empty() && platform_.decode_image(mod.cover, &full))
                    detail_cover_ = cover_fit(full, bw, bh, 0.5f, 0.5f);
            }
            const Rect cr{P(dx + (dw - cover_pt) / 2), P(dy), bw, bh};
            c.shadow(cr, P(8), P(16), hex(0x000000, 150));
            if (!detail_cover_.rgba.empty()) {
                ImageStyle st;
                st.radius = P(8);
                if (dll) {
                    st.saturation = 0.15f;
                    st.brightness = 0.6f;
                }
                c.image(cr.x, cr.y, cr.w, cr.h, detail_cover_.rgba.data(), st);
            } else
                placeholder(cr, readable(mod.title), dll, P(8));
            dy += float(bh) / d_ + 20;
            // The title, the authors, the category.
            {
                const float step = 31;
                for (const std::string &l : wrap_text(readable(mod.title), P(dw), measure(k.strong, P(26)))) {
                    text(k.strong, dx, dy + 25, l, 26, pal::gold_hot);
                    dy += step;
                }
                dy += 6;
            }
            std::string authors;
            for (const std::string &a : mod.authors)
                authors += (authors.empty() ? "" : ", ") + a;
            if (!authors.empty())
                for (const std::string &l : wrap_text("Autorzy: " + authors, P(dw), measure(k.body, P(15)))) {
                    text(k.body, dx, dy + 16, l, 16, pal::dim);
                    dy += 22;
                }
            {
                const std::string cat = category_name(mod.category.empty() ? "Uncategorized" : mod.category);
                const int cpx = P(12.5f);
                const int cwid = k.body.measure(cat, cpx, 1.2f * d_);
                const Rect chip{P(dx), P(dy + 6), cwid + P(20), P(24)};
                c.fill_round(chip, chip.h / 2, hex(0xc9a24a, 30));
                c.stroke_round(chip, chip.h / 2, std::max(1, P(1)), fade(pal::gold, 0.4f));
                k.body.draw(c, chip.x + P(10), chip.y + chip.h / 2 + (k.body.ascent(cpx) - k.body.descent(cpx)) / 2, cat,
                            cpx, pal::gold, 1.2f * d_);
                dy += 42;
            }
            // The switch, with what it means now.
            for (size_t i = 0; i < buttons_.size(); ++i) {
                Button &b = buttons_[i];
                if (b.id != kModToggle)
                    continue;
                b.rect = {P(dx), P(dy), P(dw), P(56)};
                const float h = hot(int(i));
                const Rect r = b.rect;
                c.fill_round(r, P(8), b.enabled ? mix(hex(0x0d0b09, 160), hex(0x2d261f, 230), h) : hex(0x0d0b09, 120));
                c.stroke_round(r, P(8), std::max(1, P(1)), on ? fade(pal::gold, 0.7f) : mix(pal::border, pal::border_hi, h));
                const Rect track{r.x + P(14), r.y + (r.h - P(26)) / 2, P(48), P(26)};
                c.fill_round(track, track.h / 2, on ? pal::gold : hex(0x2a231b));
                if (!on)
                    c.stroke_round(track, track.h / 2, std::max(1, P(1.5f)),
                                   b.enabled ? mix(pal::gold_deep, pal::gold, h) : pal::border);
                const float knob = on ? float(track.x + track.w - track.h / 2) : float(track.x + track.h / 2);
                c.disc(knob, float(track.y) + float(track.h) / 2, float(track.h) / 2 - float(P(3)),
                       on ? pal::ink : (b.enabled ? mix(pal::gold_deep, pal::gold, 0.4f + 0.6f * h) : pal::faint));
                const std::string label = mod.builtin ? "Wbudowany · zawsze włączony"
                                          : dll       ? "Wymaga DLL · wkrótce"
                                          : on        ? "Włączony"
                                                      : "Wyłączony";
                const int lpx = P(17);
                const int lbase = r.y + r.h / 2 + (k.strong.ascent(lpx) - k.strong.descent(lpx)) / 2;
                const int lw = k.strong.draw(c, track.x + track.w + P(14), lbase, label, lpx,
                                             b.enabled ? (on ? pal::gold_hot : pal::text) : pal::faint);
                if (b.enabled) {
                    // What a click does, on the right.
                    const std::string what = on ? "wyłącz" : "kliknij, aby włączyć";
                    const int wpx = P(14);
                    const int ww = k.body.measure(what, wpx);
                    if (track.x + track.w + P(14) + lw + P(16) + ww < r.x + r.w - P(14))
                        k.body.draw(c, r.x + r.w - P(14) - ww, lbase, what, wpx, mix(pal::dim, pal::gold_hot, h));
                }
                if (focus_visible(int(i)))
                    c.stroke_round(r.inset(-P(3)), P(10), std::max(1, P(2)), pal::gold_hot);
                dy += 56 + 10;
            }
            // Why the switch is locked, the load order.
            auto note = [&](const std::string &t, Color col) {
                for (const std::string &l : wrap_text(t, P(dw), measure(k.body, P(14.5f)))) {
                    text(k.body, dx, dy + 15, l, 14.5f, col);
                    dy += 20;
                }
                dy += 4;
            };
            if (mod.builtin)
                note("Port ma ten mod przepisany natywnie (" + mod.script_dll +
                         " nie jest potrzebny), więc działa zawsze, także bez zaznaczenia.",
                     pal::dim);
            else if (dll)
                note("Ten mod ma " + mod.script_dll + ": kod, którego port jeszcze nie uruchamia.", pal::amber);
            else if (on) {
                const std::vector<const ModInfo *> active = mod_active(mods_, sel_);
                size_t pos = 0;
                while (pos < active.size() && active[pos] != &mod)
                    ++pos;
                note(std::to_string(pos + 1) + ". z " + std::to_string(active.size()) + " w kolejności ładowania.",
                     pal::dim);
                const std::string req = mod_required_by(mods_, sel_, mod.id);
                if (!req.empty())
                    note("Nie da się wyłączyć: " + req + ".", pal::amber);
            } else {
                const std::string why = mod_blocked(mods_, sel_, mod.id);
                if (!why.empty())
                    note("Nie da się włączyć: " + why + ".", pal::red_text);
            }
            // Dependencies and conflicts as lists with their state.
            auto list = [&](const char *head, const std::vector<std::string> &ids, bool conflicts) {
                if (ids.empty())
                    return;
                dy += 8;
                text(k.body, dx, dy + 12, head, 12, pal::faint, 2.2f);
                dy += 22;
                std::vector<std::string> order = ids;
                std::stable_sort(order.begin(), order.end(), [&](const std::string &a, const std::string &b) {
                    return sel_.enabled(a) > sel_.enabled(b); // enabled (real conflicts, met needs) first
                });
                const size_t shown = order.size() > 5 ? 4 : order.size();
                for (size_t n = 0; n < shown; ++n) {
                    const std::string &d = order[n];
                    const int j = mods_.find(d);
                    const bool here = j >= 0, enabled = here && sel_.enabled(d);
                    Color col;
                    Icon icon;
                    if (conflicts) {
                        // Enabled: a real conflict now. Otherwise only a note.
                        icon = enabled ? Icon::Cross : Icon::Back;
                        col = enabled ? pal::red : pal::faint;
                        if (!enabled) {
                            c.disc(float(P(dx + 8)), float(P(dy + 10)), float(P(2.5f)), col);
                            icon = Icon(-1);
                        }
                    } else {
                        icon = enabled ? Icon::Check : here ? Icon::Check : Icon::Cross;
                        col = enabled ? pal::green : here ? (on ? pal::amber : pal::faint) : pal::red;
                    }
                    if (int(icon) >= 0)
                        draw_icon(c, icon, float(P(dx + 8)), float(P(dy + 10)), float(P(16)), col);
                    std::string name = here ? readable(mods_.mods[size_t(j)].title) : d + " (brak w katalogu)";
                    if (conflicts && enabled)
                        name += " (włączony)";
                    else if (!conflicts && here && !enabled)
                        name += on ? " (wyłączony!)" : " (włączy się razem)";
                    const std::vector<std::string> ls = wrap_text(name, P(dw - 26), measure(k.body, P(15)));
                    for (const std::string &l : ls) {
                        text(k.body, dx + 24, dy + 15, l, 15, conflicts && enabled ? pal::red_text : pal::text);
                        dy += 21;
                    }
                    dy += 3;
                }
                if (shown < order.size()) {
                    const size_t more = order.size() - shown;
                    text(k.body, dx + 24, dy + 15,
                         "i " + std::to_string(more) + (more == 1 ? " inny" : more < 5 ? " inne" : " innych"), 15,
                         pal::dim);
                    dy += 24;
                }
            };
            list("WYMAGA", mod.dependencies, false);
            list("NIE DZIAŁA Z", mod.incompatibles, true);
            // The readme: headings and paragraphs.
            dy += 12;
            c.fill({P(dx), P(dy), P(dw), std::max(1, P(1))}, pal::border);
            dy += 18;
            if (readme_.empty())
                note("Brak opisu.", pal::faint);
            for (const ReadmeLine &l : readme_) {
                if (l.text.empty()) {
                    dy += 10;
                    continue;
                }
                if (l.heading) {
                    dy += 8;
                    for (const std::string &w : wrap_text(l.text, P(dw), measure(k.strong, P(18)))) {
                        text(k.strong, dx, dy + 18, w, 18, pal::gold);
                        dy += 25;
                    }
                    dy += 4;
                    continue;
                }
                std::string t = l.text;
                float indent = 0;
                if (t.compare(0, 2, "- ") == 0) {
                    t = t.substr(2);
                    indent = 16;
                    c.disc(float(P(dx + 5)), float(P(dy + 11)), float(P(2.2f)), pal::gold_deep);
                }
                for (const std::string &w : wrap_text(t, P(dw - indent), measure(k.body, P(16)))) {
                    // Only what can be seen is drawn; the rest is measured. dy is already on screen
                    // (the scroll is taken off where it starts), so the band is the panel's own.
                    if (dy + 24 > body_top - 40 && dy < body_top + body_h + 40)
                        text(k.body, dx + indent, dy + 17, w, 16, hex(0xd6cab2));
                    dy += 23;
                }
                dy += 4;
            }
        } else {
            text(k.body, dx, dy + 20, "Wybierz moda, żeby zobaczyć opis.", 16, pal::dim);
            dy += 30;
        }
        detail_content_ = dy + detail_scroll_ - (body_top + 20) + 40;
        c.reset_clip();
        const float max = std::max(0.0f, detail_content_ - body_h);
        detail_target_ = std::clamp(detail_target_, 0.0f, max);
        if (max > 0) {
            const float frac = body_h / detail_content_;
            const float thumb = std::max(36.0f, (body_h - 16) * frac);
            const float pos = (body_h - 16 - thumb) * (detail_scroll_ / max);
            c.fill_round({P(W - M - 9), P(body_top + 8 + pos), P(4), P(thumb)}, P(2), fade(pal::gold, 0.5f));
        }
    }

    // The bottom bar: how many are chosen, the warnings, Back and Play.
    {
        const int by = P(H - kBottom);
        c.gradient({0, by, cw, ch - by}, hex(0x0b0907, 170), hex(0x0b0907, 230));
        c.fill({0, by, cw, std::max(1, P(1))}, fade(pal::border, 0.9f));
        const size_t n = mod_active(mods_, sel_).size();
        std::string chosen = n == 0 ? std::string("Bez modów") : "Wybrano " + std::to_string(n) + " " + mods_word(n);
        if (!sel_.preset.empty())
            chosen += "  ·  preset " + readable(sel_.preset);
        float left_room = W - M - 260 - 14 - 170 - 24 - M;
        text(k.strong, M, H - kBottom + 38, ellipsize(chosen, P(left_room), measure(k.strong, P(19))), 19, pal::text);
        std::string warn;
        Color wc = pal::dim;
        if (!mod_notice_.empty()) {
            warn = mod_notice_;
            wc = confirm_play_ ? pal::amber : pal::dim;
        }
        if (!problems.empty()) {
            const std::string p = problems.front().text +
                                  (problems.size() > 1 ? "  (+" + std::to_string(problems.size() - 1) + " więcej)" : "");
            if (warn.empty() || confirm_play_) {
                if (!warn.empty() && confirm_play_)
                    warn += "  ";
                warn += p;
                wc = problems.front().conflict ? pal::red : pal::amber;
                if (confirm_play_)
                    wc = pal::amber;
            }
        }
        if (!warn.empty()) {
            const std::vector<std::string> wl = wrap_lines(warn, P(left_room), 2, measure(k.body, P(14.5f)));
            float wy = H - kBottom + 60;
            for (const std::string &l : wl) {
                text(k.body, M, wy, l, 14.5f, wc);
                wy += 19;
            }
        }
    }
    // Back, Play and the catalog button.
    for (size_t i = 0; i < buttons_.size(); ++i) {
        const Button &b = buttons_[i];
        if (b.id == kModsBack || b.id == kModsPlay || b.id == kModsRoot)
            draw_button(c, b, int(i));
    }
    (void)px;
    if (pending)
        dirty_ = true;
}

} // namespace launcher
