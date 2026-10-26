// remaster_mods.h - NFSMW Remaster R3 run 19 (ROADMAP M, D-127/D-132): the player's Mods folder.
//
// Texture mods the player downloads (Windows texture packs, e.g. a HUD mod) are matched to the game's textures by
// file name and loaded ahead of our packs (review/run18/m-design.md; lead changes in review/run19/lead-log.md):
//   <profile>/Mods/<mod folder>/.../<NAME>.dds   one folder per mod, any depth; loose files in Mods/ count first;
//                                                folders load in natural A to Z order and the later one wins; a
//                                                folder whose name starts with "_" or "." (and __MACOSX) is ignored.
//   key = bStringHash(NAME) << 32                the game's name hash (h = 0xFFFFFFFF; h = h * 33 + c), as stored
//                                                at TextureInfo+0x24; NAME exactly as typed, then upper case.
//   NAME of 23 characters                        the game stores only 23 characters of a longer name (DebugName[24])
//                                                and hashes the whole one: the known list maps the stem to every
//                                                texture whose stored name is that stem.
//   NAME = 8 hex digits (or 0x + 8)              the hash itself (what texture dumpers write).
// The known list (Resources/ModNames/known.tsv, made by Setup from the app's own game files) holds
// hashes and sizes only, no names: "k\t<hash8>\t<w>\t<h>" per texture, "t\t<stem hash8>\t<full hash8>" per truncated
// name. With it the report can say UNKNOWN NAME and WRONG SHAPE at startup; a name it does not hold still gets a row,
// loaded on demand only (never warmed), so an incomplete list cannot break a mod. Without it nothing is checked.
// Mods/mods-report.txt is rewritten at every scan: one line per file (OK, UNKNOWN NAME, WRONG SHAPE, UNSUPPORTED,
// CORRUPT, TOO LARGE, SHADOWED). Pure C++ (no Metal): the scan runs once, at device creation, on the render thread.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace remaster {

// bStringHash: the game's texture name hash (seed 0xFFFFFFFF, h * 33 + c, bytes as typed).
inline uint32_t name_hash(const std::string &s) {
    uint32_t h = 0xFFFFFFFFu;
    for (unsigned char c : s)
        h = h * 33u + c;
    return h;
}

// DDS formats the replacement loader reads (all sampled as UNORM: the game samples linear).
enum DdsFmt { DDS_NONE, DDS_BGRA8, DDS_RGBA8, DDS_BC1, DDS_BC2, DDS_BC3, DDS_BC7 };
inline const char *dds_fmt_name(int f) {
    static const char *n[] = {"?", "BGRA8", "RGBA8", "BC1/DXT1", "BC2/DXT3", "BC3/DXT5", "BC7"};
    return f >= 0 && f <= DDS_BC7 ? n[f] : "?";
}
inline bool dds_block(int f) { return f == DDS_BC1 || f == DDS_BC2 || f == DDS_BC3 || f == DDS_BC7; }

// A DDS header read without trusting it: format, size, levels and the byte total the file must hold.
struct DdsProbe {
    int fmt = DDS_NONE;
    uint32_t w = 0, h = 0, mips = 0;
    uint64_t need = 0;          // header + every level: the file must be at least this long
    uint32_t data_offset = 0;   // 128, or 148 with a DX10 header
    bool force_alpha = false;   // X8 formats in a mod file: alpha bytes are padding, written as 255 at load
    const char *why = nullptr;  // why it cannot be read (nullptr = fmt is set)
    bool corrupt = false;       // the file is shorter than its header says
};
// h31: the 31 header words after "DDS "; dx10: the 5 DX10 words (read when h31 says DX10); mod: a player file
// (sRGB-tagged and X8 formats accepted, 8192 px cap; our packs keep the strict rules and 16384).
inline DdsProbe dds_probe(const uint32_t *h, const uint32_t *dx10, bool have_dx10, uint64_t file_size, bool mod) {
    DdsProbe p;
    auto cc = [](const char *s) {
        return (uint32_t)(uint8_t)s[0] | (uint32_t)(uint8_t)s[1] << 8 | (uint32_t)(uint8_t)s[2] << 16 |
               (uint32_t)(uint8_t)s[3] << 24;
    };
    if (h[0] != 124 || h[18] != 32) {
        p.why = "not a DDS header";
        p.corrupt = true;
        return p;
    }
    if ((h[27] & 0x200u) || (h[27] & 0x200000u)) {   // DDSCAPS2_CUBEMAP / VOLUME
        p.why = "cube map or volume";
        return p;
    }
    p.data_offset = 128;
    if (h[19] & 0x4u) {   // FOURCC
        const uint32_t f = h[20];
        if (f == cc("DXT1")) p.fmt = DDS_BC1;
        else if (f == cc("DXT2") || f == cc("DXT3")) p.fmt = mod || f == cc("DXT3") ? DDS_BC2 : DDS_NONE;
        else if (f == cc("DXT4") || f == cc("DXT5")) p.fmt = mod || f == cc("DXT5") ? DDS_BC3 : DDS_NONE;
        else if (f == cc("DX10")) {
            p.data_offset = 148;
            if (!have_dx10) {
                p.why = "short DX10 header";
                p.corrupt = true;
                return p;
            }
            if (dx10[1] != 3 || dx10[3] > 1 || (dx10[2] & 0x4u)) {   // TEXTURE2D, one image, not a cube
                p.why = "texture array, cube or not 2D";
                return p;
            }
            switch (dx10[0]) {
            case 87: p.fmt = DDS_BGRA8; break;
            case 28: p.fmt = DDS_RGBA8; break;
            case 71: p.fmt = DDS_BC1; break;
            case 74: p.fmt = DDS_BC2; break;
            case 77: p.fmt = DDS_BC3; break;
            case 98: p.fmt = DDS_BC7; break;
            // Mod files only: the sRGB-tagged twins hold the same bytes; the game samples linear, so they show
            // exactly like a UNORM export. B8G8R8X8 (88, 93): alpha is padding.
            case 91: p.fmt = mod ? DDS_BGRA8 : DDS_NONE; break;
            case 29: p.fmt = mod ? DDS_RGBA8 : DDS_NONE; break;
            case 72: p.fmt = mod ? DDS_BC1 : DDS_NONE; break;
            case 75: p.fmt = mod ? DDS_BC2 : DDS_NONE; break;
            case 78: p.fmt = mod ? DDS_BC3 : DDS_NONE; break;
            case 99: p.fmt = mod ? DDS_BC7 : DDS_NONE; break;
            case 88: case 93:
                p.fmt = mod ? DDS_BGRA8 : DDS_NONE;
                p.force_alpha = mod;
                break;
            default: break;
            }
        }
    } else if ((h[19] & 0x40u) && h[21] == 32) {   // uncompressed RGB, 32 bits
        const bool alpha = (h[19] & 0x1u) && h[25] == 0xff000000u;
        if (h[22] == 0x00ff0000u && h[23] == 0x0000ff00u && h[24] == 0x000000ffu)
            p.fmt = DDS_BGRA8;
        else if (mod && h[22] == 0x000000ffu && h[23] == 0x0000ff00u && h[24] == 0x00ff0000u)
            p.fmt = DDS_RGBA8;
        p.force_alpha = mod && p.fmt != DDS_NONE && !alpha;
    }
    if (p.fmt == DDS_NONE) {
        p.why = (h[19] & 0x40u) && h[21] != 32 ? "uncompressed but not 32 bits per pixel" : "pixel format not read";
        return p;
    }
    p.w = h[3];
    p.h = h[2];
    p.mips = h[6] ? h[6] : 1;
    const uint32_t cap = mod ? 8192 : 16384;
    if (!p.w || !p.h || p.mips > 15) {
        p.why = "bad size or mip count";
        p.fmt = DDS_NONE;
        p.corrupt = true;
        return p;
    }
    if (p.w > cap || p.h > cap) {
        p.why = "too large";
        p.fmt = DDS_NONE;
        return p;
    }
    // More levels than a full chain is a broken header; mod tools sometimes write a full chain for odd sizes.
    uint32_t full = 1;
    for (uint32_t m = std::max(p.w, p.h); m > 1; m >>= 1)
        ++full;
    if (p.mips > full) {
        if (!mod) {
            p.why = "bad size or mip count";
            p.fmt = DDS_NONE;
            p.corrupt = true;
            return p;
        }
        p.mips = full;
    }
    const bool blk = dds_block(p.fmt);
    const uint32_t unit = p.fmt == DDS_BC1 ? 8 : 16;
    uint64_t total = p.data_offset;
    for (uint32_t l = 0; l < p.mips; ++l) {
        const uint64_t lw = std::max<uint32_t>(p.w >> l, 1), lh = std::max<uint32_t>(p.h >> l, 1);
        total += blk ? (lw + 3) / 4 * ((lh + 3) / 4) * unit : lw * lh * 4;
    }
    p.need = total;
    if (file_size < total) {
        p.why = "file shorter than its header says: truncated or corrupt";
        p.fmt = DDS_NONE;
        p.corrupt = true;
    }
    return p;
}

// Reads and probes the header of a DDS file (no pixel data).
inline DdsProbe dds_probe_file(const std::string &path, bool mod) {
    DdsProbe p;
    std::error_code ec;
    const uint64_t size = std::filesystem::file_size(path, ec);
    std::FILE *f = ec ? nullptr : std::fopen(path.c_str(), "rb");
    if (!f) {
        p.why = "cannot be opened";
        return p;
    }
    char magic[4];
    uint32_t h[31] = {}, dx10[5] = {};
    const bool ok = std::fread(magic, 1, 4, f) == 4 && !std::memcmp(magic, "DDS ", 4) && std::fread(h, 4, 31, f) == 31;
    const bool have_dx10 = ok && std::fread(dx10, 4, 5, f) == 5;
    std::fclose(f);
    if (!ok) {
        p.why = "not a DDS file";
        p.corrupt = true;
        return p;
    }
    return dds_probe(h, dx10, have_dx10, size, mod);
}

// The game's texture list (hashes and sizes, no names), for the report and for 23-character names.
struct ModKnown {
    bool loaded = false;
    size_t textures = 0;
    std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> sizes;
    std::unordered_map<uint32_t, std::vector<uint32_t>> truncated;   // stem hash -> full hashes
    bool load(const std::string &file) {
        std::FILE *f = file.empty() ? nullptr : std::fopen(file.c_str(), "r");
        if (!f)
            return false;
        char line[128];
        while (std::fgets(line, sizeof line, f)) {
            char kind = 0;
            unsigned a = 0, b = 0, c = 0;
            if (line[0] == '#')
                continue;
            if (std::sscanf(line, "%c %x %u %u", &kind, &a, &b, &c) == 4 && kind == 'k') {
                auto &v = sizes[a];
                if (std::find(v.begin(), v.end(), std::make_pair(b, c)) == v.end())
                    v.emplace_back(b, c);
            } else if (std::sscanf(line, "%c %x %x", &kind, &a, &b) == 3 && kind == 't') {
                truncated[a].push_back(b);
            }
        }
        std::fclose(f);
        textures = sizes.size();
        loaded = textures > 0;
        return loaded;
    }
};

// A replacement row from the Mods folder.
struct ModRow {
    uint64_t key;
    std::string file;   // absolute
    bool ondemand;      // not warmed: unknown to the list, or larger than 2048 px
};

struct ModScan {
    std::string dir, report;
    bool list = false;
    std::vector<ModRow> rows;
    std::vector<std::string> mods, ignored;   // load order; folders skipped by name
    size_t files = 0, ok = 0, unknown = 0, unsupported = 0, wrong_shape = 0, corrupt = 0, too_large = 0,
           shadowed = 0, no_mips = 0, other_files = 0;
    uint64_t bytes = 0;   // files with rows
};

namespace mods_detail {
// Finder-like order: case-insensitive, digit runs by value ("2 b" before "10 a").
inline bool natural_less(const std::string &a, const std::string &b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const unsigned char ca = a[i], cb = b[j];
        if (std::isdigit(ca) && std::isdigit(cb)) {
            size_t ie = i, je = j;
            while (ie < a.size() && a[ie] == '0') ++ie;
            while (je < b.size() && b[je] == '0') ++je;
            size_t ia = ie, jb = je;
            while (ia < a.size() && std::isdigit((unsigned char)a[ia])) ++ia;
            while (jb < b.size() && std::isdigit((unsigned char)b[jb])) ++jb;
            if (ia - ie != jb - je)
                return ia - ie < jb - je;
            const int c = a.compare(ie, ia - ie, b, je, jb - je);
            if (c)
                return c < 0;
            i = ia;
            j = jb;
            continue;
        }
        const int la = std::tolower(ca), lb = std::tolower(cb);
        if (la != lb)
            return la < lb;
        ++i;
        ++j;
    }
    if (a.size() - i != b.size() - j)
        return a.size() - i < b.size() - j;
    return a < b;
}
inline std::string lower(std::string s) {
    for (char &c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}
inline std::string upper(std::string s) {
    for (char &c : s)
        c = (char)std::toupper((unsigned char)c);
    return s;
}
inline bool skipped_name(const std::string &n) {
    return n.empty() || n[0] == '.' || n == "__MACOSX";
}
inline bool hex8(const std::string &s, uint32_t &out) {
    std::string t = s;
    if (t.size() == 10 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X'))
        t = t.substr(2);
    if (t.size() != 8)
        return false;
    for (char c : t)
        if (!std::isxdigit((unsigned char)c))
            return false;
    out = (uint32_t)std::strtoul(t.c_str(), nullptr, 16);
    return true;
}
inline bool size_ok(uint32_t iw, uint32_t ih, uint32_t gw, uint32_t gh) {   // remaster_cache.h's rule
    return iw >= gw && ih >= gh && (uint64_t)iw * gh == (uint64_t)ih * gw;
}
} // namespace mods_detail

// Scans `dir` (created when missing), writes dir/mods-report.txt, returns the rows in load order (a later row for
// the same key wins when the caller inserts them in order). `known_file` may be "" (nothing checked).
inline ModScan scan_mods(const std::string &dir, const std::string &known_file, size_t max_files = 100000) {
    namespace fs = std::filesystem;
    using namespace mods_detail;
    ModScan s;
    s.dir = dir;
    ModKnown known;
    s.list = known.load(known_file);
    std::error_code ec;
    fs::create_directories(dir, ec);

    struct Entry {
        std::string rel, abs, status, detail;
        std::vector<uint64_t> keys;
        bool ondemand = false, has_rows = false;
        uint64_t bytes = 0;
    };
    std::vector<Entry> out;
    std::vector<std::string> loose;
    std::vector<std::pair<std::string, fs::path>> folders;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = it->path().filename().string();
        if (skipped_name(n) || n == "mods-report.txt")
            continue;
        std::error_code e2;
        if (fs::is_directory(it->path(), e2)) {   // follows a symlinked mod folder (one level)
            if (n[0] == '_')
                s.ignored.push_back(n);
            else
                folders.emplace_back(n, it->path());
        } else if (fs::is_regular_file(it->path(), e2)) {
            loose.push_back(n);
        }
    }
    std::sort(folders.begin(), folders.end(), [](const auto &a, const auto &b) { return natural_less(a.first, b.first); });
    std::sort(loose.begin(), loose.end(), natural_less);
    std::sort(s.ignored.begin(), s.ignored.end(), natural_less);

    // (mod name, relative paths) in load order
    std::vector<std::pair<std::string, std::vector<std::string>>> order;
    if (!loose.empty())
        order.emplace_back("(loose files in Mods)", loose);
    size_t seen = loose.size();
    for (const auto &f : folders) {
        std::vector<std::string> files;
        fs::recursive_directory_iterator it(f.second, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end && seen < max_files; it.increment(ec)) {
            const std::string n = it->path().filename().string();
            if (skipped_name(n)) {
                if (it->is_directory(ec))
                    it.disable_recursion_pending();
                ec.clear();
                continue;
            }
            if (it.depth() > 16) {
                it.disable_recursion_pending();
                continue;
            }
            std::error_code e2;
            if (it->is_regular_file(e2)) {
                files.push_back(f.first + "/" + it->path().lexically_relative(f.second).generic_string());
                ++seen;
            }
        }
        ec.clear();
        std::sort(files.begin(), files.end(), natural_less);
        s.mods.push_back(f.first);
        order.emplace_back(f.first, std::move(files));
    }

    std::unordered_map<uint64_t, size_t> owner;   // key -> index into out of the file that holds it now
    for (const auto &mod : order) {
        for (const std::string &rel : mod.second) {
            Entry e;
            e.rel = rel;
            e.abs = dir + "/" + rel;
            const size_t slash = rel.find_last_of('/');
            const std::string base = slash == std::string::npos ? rel : rel.substr(slash + 1);
            const size_t dot = base.find_last_of('.');
            const std::string ext = dot == std::string::npos ? "" : lower(base.substr(dot + 1));
            const std::string stem = dot == std::string::npos ? base : base.substr(0, dot);
            if (ext != "dds") {
                if (ext == "png" || ext == "tga" || ext == "jpg" || ext == "jpeg" || ext == "bmp") {
                    e.status = "UNSUPPORTED";
                    e.detail = "(" + upper(ext) + " files are not read yet: save it as .dds)";
                    ++s.files;
                    ++s.unsupported;
                    out.push_back(std::move(e));
                } else {
                    ++s.other_files;
                }
                continue;
            }
            ++s.files;
            const DdsProbe p = dds_probe_file(e.abs, true);
            if (p.fmt == DDS_NONE) {
                const bool large = p.why && !std::strcmp(p.why, "too large");
                e.status = p.corrupt ? "CORRUPT" : large ? "TOO LARGE" : "UNSUPPORTED";
                e.detail = std::string("(") + (large ? "over 8192 pixels" : p.why ? p.why : "?") + ")";
                (p.corrupt ? s.corrupt : large ? s.too_large : s.unsupported)++;
                out.push_back(std::move(e));
                continue;
            }
            char dims[96];
            std::snprintf(dims, sizeof dims, "%ux%u %s mips %u", p.w, p.h, dds_fmt_name(p.fmt), p.mips);
            if (p.mips == 1 && dds_block(p.fmt) && (p.w > 4 || p.h > 4))
                ++s.no_mips;
            // name -> keys
            std::vector<uint32_t> hashes;
            std::string how;
            auto by_name = [&](const std::string &n) {
                const uint32_t hh = name_hash(n);
                if (known.sizes.count(hh))
                    hashes.push_back(hh);
                auto t = known.truncated.find(hh);
                if (t != known.truncated.end())
                    for (uint32_t x : t->second)
                        if (std::find(hashes.begin(), hashes.end(), x) == hashes.end())
                            hashes.push_back(x);
            };
            uint32_t hx = 0;
            if (s.list) {
                by_name(stem);
                if (hashes.empty() && upper(stem) != stem) {
                    by_name(upper(stem));
                    if (!hashes.empty())
                        how = "upper case";
                }
                if (hashes.empty() && hex8(stem, hx) && known.sizes.count(hx)) {
                    hashes.push_back(hx);
                    how = "hash in the name";
                }
            }
            bool unknown = s.list && hashes.empty();
            if (!s.list || unknown) {   // not checkable: the name's hash (and its upper case, and a hex name)
                if (hex8(stem, hx))
                    hashes.push_back(hx);
                hashes.push_back(name_hash(stem));
                if (upper(stem) != stem)
                    hashes.push_back(name_hash(upper(stem)));
            }
            // shape against the game's sizes (when known)
            std::vector<uint64_t> keys;
            std::string game_size;
            for (uint32_t hh : hashes) {
                auto sz = known.sizes.find(hh);
                bool fits = sz == known.sizes.end();
                if (!fits)
                    for (const auto &g : sz->second) {
                        if (size_ok(p.w, p.h, g.first, g.second))
                            fits = true;
                        else if (game_size.empty())
                            game_size = std::to_string(g.first) + "x" + std::to_string(g.second);
                    }
                if (fits)
                    keys.push_back((uint64_t)hh << 32);
            }
            if (keys.empty()) {
                e.status = "WRONG SHAPE";
                e.detail = std::string(dims) + "; the game's is " + game_size +
                           " (a replacement must have the same proportions and be at least as large)";
                ++s.wrong_shape;
                out.push_back(std::move(e));
                continue;
            }
            e.keys = keys;
            e.has_rows = true;
            e.bytes = p.need;
            e.ondemand = unknown || !s.list || std::max(p.w, p.h) > 2048;
            char hs[64];
            std::snprintf(hs, sizeof hs, "%08x", (uint32_t)(keys[0] >> 32));
            if (unknown) {
                e.status = "UNKNOWN NAME";
                e.detail = std::string(dims) + " (no game texture has this name: kept, loaded only if the game uses it)";
                ++s.unknown;
            } else {
                e.status = "OK";
                e.detail = "-> " + stem + " (" + hs + (keys.size() > 1 ? " and " + std::to_string(keys.size() - 1) +
                                                                         " more" : std::string()) + ") " + dims;
                if (!s.list)
                    e.detail += " (name not checked: no game texture list)";
                else if (stem.size() == 23 && keys.size() > 1)
                    e.detail += " (long name: replaces " + std::to_string(keys.size()) + " game textures)";
                if (!how.empty())
                    e.detail += " (matched by " + how + ")";
                ++s.ok;
            }
            const size_t idx = out.size();
            out.push_back(std::move(e));
            for (uint64_t k : keys)
                owner[k] = idx;
        }
    }
    // A file none of whose keys it still holds is SHADOWED by the file that took them last.
    for (size_t i = 0; i < out.size(); ++i) {
        Entry &e = out[i];
        if (!e.has_rows)
            continue;
        size_t by = SIZE_MAX;
        bool any = false;
        for (uint64_t k : e.keys) {
            const size_t o = owner[k];
            if (o == i)
                any = true;
            else
                by = o;
        }
        if (!any) {
            (e.status == "OK" ? s.ok : s.unknown)--;
            e.status = "SHADOWED";
            e.detail = "(replaced by " + out[by].rel + ", later in the load order)";
            e.has_rows = false;
            ++s.shadowed;
        }
    }
    for (size_t i = 0; i < out.size(); ++i) {
        const Entry &e = out[i];
        if (!e.has_rows)
            continue;
        s.bytes += e.bytes;
        for (uint64_t k : e.keys)
            if (owner[k] == i)
                s.rows.push_back(ModRow{k, e.abs, e.ondemand});
    }

    s.report = dir + "/mods-report.txt";
    if (std::FILE *f = std::fopen(s.report.c_str(), "w")) {
        std::fprintf(f, "Player mods report (rewritten at every launch)\nFolder: %s\n", dir.c_str());
        std::fprintf(f, "Game texture list: %s\n",
                     s.list ? (known_file + " (" + std::to_string(known.textures) + " textures)").c_str()
                            : "none (names and shapes not checked)");
        std::fprintf(f, "Load order (a later mod wins):");
        size_t n = 0;
        if (!loose.empty())
            std::fprintf(f, " %zu. (loose files in Mods)", ++n);
        for (const auto &m : s.mods)
            std::fprintf(f, " %zu. %s", ++n, m.c_str());
        std::fprintf(f, n ? "\n" : " none\n");
        if (!s.ignored.empty()) {
            std::fprintf(f, "Ignored (name starts with _):");
            for (const auto &m : s.ignored)
                std::fprintf(f, " %s", m.c_str());
            std::fprintf(f, "\n");
        }
        std::fprintf(f, "Textures: %zu OK, %zu unknown name, %zu wrong shape, %zu unsupported, %zu corrupt, %zu too large, "
                        "%zu shadowed; %zu other files ignored (.txt, .ini, .asi ...)\n\n",
                     s.ok, s.unknown, s.wrong_shape, s.unsupported, s.corrupt, s.too_large, s.shadowed, s.other_files);
        for (const Entry &e : out)
            std::fprintf(f, "%-13s %s %s\n", e.status.c_str(), e.rel.c_str(), e.detail.c_str());
        std::fclose(f);
    } else {
        s.report.clear();
    }
    return s;
}

} // namespace remaster
