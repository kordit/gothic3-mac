// remaster_tex.h - NFSMW Remaster A1, step 1: the texture keys the Metal D3D9 renderer sees.
//
// A mod keys a texture (PopModApi::d3d9_texture_tag); the key reaches the renderer in the texture's
// HostD9TextureDesc, or later through D3D9Backend::tag. Nothing here changes what is drawn. Off unless asked:
//   RECOMP_REMASTER_TEXDUMP=<dir>  write each keyed texture's top level once, as <key>.dds (or .bin for formats
//                                  DDS does not describe here), plus keys.csv with per-key draw counts;
//   RECOMP_REMASTER_TEXSTATS=1     counters only;
//   RECOMP_REMASTER_PACK_UI=<dir>  R3 run 17 (D-124): the UI pack (sharp race HUD, minimap, fonts), read after the
//                                  world pack so its keys win; rows "albedo" join the startup warm, rows
//                                  "albedo_ondemand" (the 17k minimap tiles) load when first bound. Works alone too.
//   RECOMP_REMASTER_PACK_UI2=<dir> R3 run 28 (A2, D-130): the vector HUD pack (gauges and icons redrawn as SVG), read
//                                  after the UI pack so its rows win; the textures it does not list keep their UI row.
//   RECOMP_REMASTER_MODS=1         R3 run 19 (D-132): the player's Mods folder (remaster_mods.h), read after the UI
//                                  pack so a mod wins over both packs; the folder is RECOMP_REMASTER_PACK_MODS or
//                                  <profile>/Mods, the game texture list RECOMP_REMASTER_MODS_KNOWN. Works alone too.
//                                  RECOMP_REMASTER_MODS_LOG=1 (TEST) logs each mod key the first time it is drawn.
//   RECOMP_REMASTER_PACK=<dir>     replacement pack (step 2): <dir>/index.tsv lines "<key hex16>\t<slot>\t<file>"
//                                  (slot "albedo"; other material slots reserved) naming DDS files with full mip
//                                  chains (see read_dds for formats). RECOMP_REMASTER_TEXSWAP=0 loads but binds the
//                                  original (the Original switch).
// A summary line "[remaster] tex: ..." is logged every 600 presented frames while anything changed.
// Render thread only (every caller is a D3D9Backend method).
#pragma once

#include "../../../dx/host_d9.h"
#include "../../../runtime/layout.h"
#include "remaster_mods.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace remaster {

// Remaster run 12: the synthetic key of a texture key's normal slot (key ^ kSlotNormal; real keys carry the variant,
// 0 today, in the low 32 bits).
constexpr uint64_t kSlotNormal = 0x4E524D31ull;
// R3 run 18 material sets (D-131; review/run18/c-design-check.md): the ORM slot (R cavity, G perceptual roughness,
// B metalness; BC7, linear), key ^ kSlotOrm ('ORM1').
constexpr uint64_t kSlotOrm = 0x4F524D31ull;

class TexKeys {
public:
    TexKeys() {
        const char *d = std::getenv("RECOMP_REMASTER_TEXDUMP");
        if (d && *d) {
            dir_ = d;
            ::mkdir(d, 0755);
            on_ = true;
            count_binds_ = true;
        }
        const char *s = std::getenv("RECOMP_REMASTER_TEXSTATS");
        if (s && *s == '1')
            on_ = count_binds_ = true;
        const char *p = std::getenv("RECOMP_REMASTER_PACK");
        if (p && *p) {
            on_ = true;
            pack_dir_ = p;
            read_index();
        }
        const char *u = std::getenv("RECOMP_REMASTER_PACK_UI");
        if (u && *u) {
            on_ = true;
            read_ui_index(u);
        }
        const char *u2 = std::getenv("RECOMP_REMASTER_PACK_UI2");
        if (u2 && *u2) {
            on_ = true;
            read_ui_index(u2, "vector HUD");
        }
        const char *m = std::getenv("RECOMP_REMASTER_MODS");
        if (m && *m == '1') {
            on_ = true;
            read_mods();
            const char *ml = std::getenv("RECOMP_REMASTER_MODS_LOG");
            mod_log_ = ml && *ml == '1';
        }
        const char *w = std::getenv("RECOMP_REMASTER_TEXSWAP");
        swap_ = !(w && *w == '0');
    }
    // The pack file for a key's albedo slot, or "" (and the lookup is counted either way).
    std::string pack_file(uint64_t key) {
        if (!key || pack_.empty())
            return {};
        auto it = pack_.find(key);
        if (it == pack_.end()) {
            ++look_none_;
            return {};
        }
        return it->second[0] == '/' ? it->second : pack_dir_ + "/" + it->second;   // run 12: slot rows are absolute
    }
    // Remaster run 12 PBR step 2 pilot: the "normal" rows of <dir>/index.tsv, under key ^ kSlotNormal (absolute paths).
    // A real key (low 32 bits = the variant) that equals a synthetic one keeps its row; the slot row is dropped.
    // R3 run 18: any slot name and xor (the material sets' "orm" rows under kSlotOrm).
    size_t add_slots(const std::string &dir, const char *slot_name = "normal", uint64_t xorv = kSlotNormal) {
        std::FILE *f = std::fopen((dir + "/index.tsv").c_str(), "r");
        if (!f) {
            std::fprintf(stderr, "[remaster] pack: no slot index.tsv in %s\n", dir.c_str());
            return 0;
        }
        char line[512];
        size_t n = 0, clash = 0;
        while (std::fgets(line, sizeof line, f)) {
            char key[32], slot[32], file[400];
            if (line[0] == '#' || std::sscanf(line, "%31s %31s %399s", key, slot, file) != 3 || std::strcmp(slot, slot_name))
                continue;
            const uint64_t k = std::strtoull(key, nullptr, 16) ^ xorv;
            if (pack_.count(k)) {
                ++clash;
                continue;
            }
            pack_[k] = dir + "/" + file;
            ++n;
        }
        std::fclose(f);
        std::fprintf(stderr, "[remaster] pack: %zu %s slot entries from %s (%zu dropped: key clash)\n", n, slot_name,
                     dir.c_str(), clash);
        return n;
    }
    bool has(uint64_t key) const { return pack_.count(key) != 0; }   // run 18: a pack row (albedo or slot) exists
    bool swap() const { return swap_; }
    bool has_pack() const { return !pack_dir_.empty() || ui_rows_ > 0 || !mod_keys_.empty(); }
    // Run 19: a row from the player's Mods folder (read by the cache workers; fixed after construction).
    bool is_mod(uint64_t key) const { return !mod_keys_.empty() && mod_keys_.count(key) != 0; }
    // Run 15: every pack key with its file (albedo rows and slot rows), for the cache's startup warm.
    std::vector<std::pair<uint64_t, std::string>> pack_files() const {
        std::vector<std::pair<uint64_t, std::string>> v;
        v.reserve(pack_.size());
        for (const auto &kv : pack_)
            if (!ondemand_.count(kv.first))   // run 17: the UI pack's minimap tiles stay out of the warm
                v.emplace_back(kv.first, kv.second[0] == '/' ? kv.second : pack_dir_ + "/" + kv.second);
        return v;
    }
    void redefined() { ++redefined_; changed_ = true; }
    void load_failed() { ++look_fail_; changed_ = true; }
    void load_rejected() { ++look_reject_; changed_ = true; }
    void loaded() { ++look_loaded_; changed_ = true; }
    void replaced_bind(uint64_t key) {
        ++replaced_binds_;
        if (mod_binds_.empty())
            return;
        auto it = mod_binds_.find(key);
        if (it != mod_binds_.end()) {
            if (it->second++ == 0) {
                ++mod_shown_;
                if (mod_log_) { // TEST (RECOMP_REMASTER_MODS_LOG=1): each mod key the first time its file is drawn
                    auto f = pack_.find(key);
                    std::fprintf(stderr, "[remaster] mods: shown %08x %s\n", (uint32_t)(key >> 32),
                                 f != pack_.end() ? f->second.c_str() : "?");
                }
            }
            ++mod_replaced_binds_;
        }
    }
    void dropped_rewrite() { ++dropped_rewrite_; changed_ = true; }

    // A DDS file's mip chain. Formats (all UNORM; sRGB variants are rejected for our packs: the game samples
    // linear): legacy A8R8G8B8 (BGRA8), DXT1 (BC1), DXT3 (BC2, run 19), DXT5 (BC3); DX10 header BGRA8 87, RGBA8 28,
    // BC1 71, BC2 74, BC3 77, BC7 98. Run 19: the header is checked against the file size before anything is
    // allocated (remaster_mods.h dds_probe); a player's mod file (`mod`) also takes the sRGB-tagged twins, X8 and
    // RGBA-ordered uncompressed files, at most 8192 px, and gets a box-filtered mip chain when an uncompressed file
    // has none.
    enum Fmt { F_NONE = DDS_NONE, F_BGRA8 = DDS_BGRA8, F_RGBA8 = DDS_RGBA8, F_BC1 = DDS_BC1, F_BC2 = DDS_BC2,
               F_BC3 = DDS_BC3, F_BC7 = DDS_BC7 };
    struct Image {
        uint32_t w = 0, h = 0;
        Fmt fmt = F_NONE;
        std::vector<std::vector<uint8_t>> levels;
        std::vector<uint32_t> row_bytes; // per level: bytes per row of pixels, or per row of 4x4 blocks
        size_t bytes() const {
            size_t n = 0;
            for (const auto &l : levels)
                n += l.size();
            return n;
        }
    };
    static bool block_fmt(Fmt f) { return dds_block(f); }
    static Image read_dds(const std::string &path, bool mod = false) {
        Image im;
        const DdsProbe p = dds_probe_file(path, mod);
        if (p.fmt == DDS_NONE)
            return im;
        std::FILE *f = std::fopen(path.c_str(), "rb");
        if (!f || std::fseek(f, (long)p.data_offset, SEEK_SET)) {
            if (f)
                std::fclose(f);
            return im;
        }
        const Fmt fmt = (Fmt)p.fmt;
        const bool blk = block_fmt(fmt);
        const uint32_t unit = fmt == F_BC1 ? 8 : 16; // bytes per 4x4 block
        bool ok = true;
        for (uint32_t l = 0; l < p.mips && ok; ++l) {
            const uint32_t lw = std::max<uint32_t>(p.w >> l, 1), lh = std::max<uint32_t>(p.h >> l, 1);
            const uint32_t rows = blk ? (lh + 3) / 4 : lh;
            const uint32_t rb = blk ? (lw + 3) / 4 * unit : lw * 4;
            std::vector<uint8_t> px((size_t)rows * rb);
            ok = std::fread(px.data(), 1, px.size(), f) == px.size();
            if (ok) {
                if (p.force_alpha)
                    for (size_t i = 3; i < px.size(); i += 4)
                        px[i] = 0xff;
                im.levels.push_back(std::move(px));
                im.row_bytes.push_back(rb);
            }
        }
        std::fclose(f);
        if (!ok) {
            im.levels.clear();
            im.row_bytes.clear();
            return im;
        }
        im.w = p.w;
        im.h = p.h;
        im.fmt = fmt;
        if (mod && !blk && im.levels.size() == 1)
            box_mips(im);
        return im;
    }
    bool on() const { return on_; }

    void defined(const HostD9TextureDesc &d) {
        if (!d.key)
            return;
        Key &k = keys_[d.key];
        k.w = d.width;
        k.h = d.height;
        k.format = d.format;
        k.levels = d.levels;
        ++k.defines;
        ++defined_;
        changed_ = true;
    }
    void tagged_late(uint64_t key, const HostD9TextureDesc &d) {
        ++late_;
        HostD9TextureDesc c = d;
        c.key = key;
        defined(c);
    }
    void dropped(uint64_t key) {
        if (key)
            ++dropped_;
    }
    void bind(uint64_t key) {
        if (!count_binds_)   // run 29 (D-167): the per-key map only for the dump / stats (keys.csv, the report)
            return;
        if (key) {
            ++keys_[key].binds;
            ++keyed_binds_;
        } else {
            ++unkeyed_binds_;
        }
    }
    // Top level, first face: dumped once per key.
    void upload(const HostD9TextureDesc &d, uint32_t face, uint32_t level, const uint8_t *bytes,
                uint32_t pitch) {
        if (!d.key)
            return;
        Key &k = keys_[d.key];
        ++k.uploads;
        if (dir_.empty() || k.dumped || face || level || !bytes)
            return;
        k.dumped = write_level(d, bytes, pitch);
        if (k.dumped)
            ++dumped_;
        changed_ = true;
    }
    void frame() {
        if (++frames_ % 600)
            return;
        if (!mod_binds_.empty() && mod_replaced_binds_ != mod_logged_) {
            mod_logged_ = mod_replaced_binds_;
            std::fprintf(stderr, "[remaster] mods: frame %llu rows %zu shown %llu draw-binds replaced %llu\n",
                         (unsigned long long)frames_, mod_binds_.size(), (unsigned long long)mod_shown_,
                         (unsigned long long)mod_replaced_binds_);
        }
        if (!changed_)
            return;
        changed_ = false;
        std::fprintf(stderr,
                     "[remaster] tex: frame %llu keys %zu defined %llu late-tagged %llu dropped %llu dumped %llu "
                     "draw-binds keyed %llu unkeyed %llu\n",
                     (unsigned long long)frames_, keys_.size(), (unsigned long long)defined_,
                     (unsigned long long)late_, (unsigned long long)dropped_, (unsigned long long)dumped_,
                     (unsigned long long)keyed_binds_, (unsigned long long)unkeyed_binds_);
        if (!pack_dir_.empty())
            std::fprintf(stderr,
                         "[remaster] pack: entries %zu swap %d lookups no-entry %llu load-failed %llu rejected %llu "
                         "loaded %llu; draw-binds replaced %llu; dropped (rewritten) %llu; redefined %llu\n",
                         pack_.size(), swap_ ? 1 : 0, (unsigned long long)look_none_,
                         (unsigned long long)look_fail_, (unsigned long long)look_reject_,
                         (unsigned long long)look_loaded_, (unsigned long long)replaced_binds_,
                         (unsigned long long)dropped_rewrite_, (unsigned long long)redefined_);
        if (!dir_.empty())
            write_csv();
    }

private:
    struct Key {
        uint32_t w = 0, h = 0, format = 0, levels = 0;
        uint64_t defines = 0, uploads = 0, binds = 0;
        bool dumped = false;
    };

    // Run 19: a mod file without mips (uncompressed): each level the 2x2 average of the one above (an odd edge
    // repeats its last texel), every channel apart, so a HUD texture drawn small does not shimmer.
    static void box_mips(Image &im) {
        uint32_t w = im.w, h = im.h;
        while (w > 1 || h > 1) {
            const uint32_t nw = std::max<uint32_t>(w >> 1, 1), nh = std::max<uint32_t>(h >> 1, 1);
            const std::vector<uint8_t> &src = im.levels.back();
            std::vector<uint8_t> dst((size_t)nw * nh * 4);
            for (uint32_t y = 0; y < nh; ++y)
                for (uint32_t x = 0; x < nw; ++x) {
                    const uint32_t x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
                    const uint32_t y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
                    for (uint32_t c = 0; c < 4; ++c) {
                        const uint32_t sum = src[((size_t)y0 * w + x0) * 4 + c] + src[((size_t)y0 * w + x1) * 4 + c] +
                                             src[((size_t)y1 * w + x0) * 4 + c] + src[((size_t)y1 * w + x1) * 4 + c];
                        dst[((size_t)y * nw + x) * 4 + c] = (uint8_t)((sum + 2) / 4);
                    }
                }
            im.levels.push_back(std::move(dst));
            im.row_bytes.push_back(nw * 4);
            w = nw;
            h = nh;
        }
    }

    // Run 19 (ROADMAP M): the player's Mods folder, scanned once here; its rows win over both packs.
    void read_mods() {
        const char *d = std::getenv("RECOMP_REMASTER_PACK_MODS");
        std::string dir = d && *d ? d : "";
        if (dir.empty()) {
            const std::string &profile = host_layout().profile_dir;
            if (profile.empty()) {
                std::fprintf(stderr, "[remaster] mods: no profile folder; player mods off\n");
                return;
            }
            dir = profile + "/Mods";
        }
        std::error_code ec;
        const std::filesystem::path abs = std::filesystem::absolute(dir, ec);
        if (!ec)
            dir = abs.lexically_normal().string();
        while (dir.size() > 1 && dir.back() == '/')
            dir.pop_back();
        const char *k = std::getenv("RECOMP_REMASTER_MODS_KNOWN");
        const ModScan s = scan_mods(dir, k && *k ? k : "");
        size_t replaced = 0, lazy = 0;
        for (const ModRow &r : s.rows) {
            replaced += pack_.count(r.key);
            pack_[r.key] = r.file;
            if (r.ondemand) {
                ondemand_.insert(r.key);
                ++lazy;
            } else {
                ondemand_.erase(r.key);
            }
            mod_keys_.insert(r.key);
            mod_binds_[r.key] = 0;
        }
        std::string names;
        for (size_t i = 0; i < s.mods.size() && names.size() < 160; ++i)
            names += (i ? ", " : "") + s.mods[i];
        std::fprintf(stderr,
                     "[remaster] mods: %zu textures from %zu folder%s%s%s%s, %zu unknown names, %zu unsupported, %zu wrong "
                     "shape, %zu corrupt, %zu too large, %zu shadowed; %zu rows (%zu on demand), %zu replace pack rows, "
                     "%.1f MB; list %s; report %s\n",
                     s.ok + s.unknown, s.mods.size(), s.mods.size() == 1 ? "" : "s", names.empty() ? "" : " (",
                     names.c_str(), names.empty() ? "" : ")", s.unknown, s.unsupported, s.wrong_shape, s.corrupt,
                     s.too_large, s.shadowed, s.rows.size(), lazy, replaced, s.bytes / 1048576.0,
                     s.list ? "yes" : "no", s.report.empty() ? "(not written)" : s.report.c_str());
    }

    static uint32_t fourcc(char a, char b, char c, char d) {
        return (uint32_t)(uint8_t)a | (uint32_t)(uint8_t)b << 8 | (uint32_t)(uint8_t)c << 16 |
               (uint32_t)(uint8_t)d << 24;
    }

    bool write_level(const HostD9TextureDesc &d, const uint8_t *bytes, uint32_t pitch) {
        const uint32_t dxt1 = fourcc('D', 'X', 'T', '1'), dxt3 = fourcc('D', 'X', 'T', '3'),
                       dxt5 = fourcc('D', 'X', 'T', '5');
        const bool block = d.format == dxt1 || d.format == dxt3 || d.format == dxt5;
        const bool argb = d.format == 21 || d.format == 22; // A8R8G8B8, X8R8G8B8
        uint32_t rows, row_bytes;
        if (block) {
            rows = (d.height + 3) / 4 ? (d.height + 3) / 4 : 1;
            row_bytes = ((d.width + 3) / 4 ? (d.width + 3) / 4 : 1) * (d.format == dxt1 ? 8u : 16u);
        } else if (argb) {
            rows = d.height;
            row_bytes = d.width * 4;
        } else {
            rows = d.height;
            row_bytes = pitch; // undescribed format: raw rows as the game wrote them
        }
        if (pitch < row_bytes)
            return false;
        char name[64];
        std::snprintf(name, sizeof name, "%016llx.%s", (unsigned long long)d.key, block || argb ? "dds" : "bin");
        std::FILE *f = std::fopen((dir_ + "/" + name).c_str(), "wb");
        if (!f)
            return false;
        if (block || argb) {
            uint32_t h[32] = {};
            h[0] = 124;                                   // dwSize
            h[1] = 0x1007 | (block ? 0x80000u : 0x8u);    // CAPS|HEIGHT|WIDTH|PIXELFORMAT + LINEARSIZE or PITCH
            h[2] = d.height;
            h[3] = d.width;
            h[4] = block ? rows * row_bytes : row_bytes;  // linear size or pitch
            h[6] = 1;                                     // mip count: the top level only
            h[18] = 32;                                   // ddspf.dwSize
            if (block) {
                h[19] = 0x4;                              // DDPF_FOURCC
                h[20] = d.format;
            } else {
                h[19] = d.format == 21 ? 0x41u : 0x40u;   // RGB (+ALPHAPIXELS)
                h[21] = 32;
                h[22] = 0x00ff0000;
                h[23] = 0x0000ff00;
                h[24] = 0x000000ff;
                h[25] = d.format == 21 ? 0xff000000u : 0;
            }
            h[26] = 0x1000;                               // DDSCAPS_TEXTURE
            std::fwrite("DDS ", 1, 4, f);
            std::fwrite(h, 4, 31, f);
        }
        for (uint32_t r = 0; r < rows; ++r)
            std::fwrite(bytes + (size_t)r * pitch, 1, row_bytes, f);
        std::fclose(f);
        return true;
    }

    void write_csv() {
        std::FILE *f = std::fopen((dir_ + "/keys.csv").c_str(), "w");
        if (!f)
            return;
        std::fprintf(f, "key,name_hash,variant,width,height,format,levels,defines,uploads,draw_binds,dumped\n");
        for (const auto &kv : keys_) {
            const Key &k = kv.second;
            std::fprintf(f, "%016llx,%08x,%u,%u,%u,%u,%u,%llu,%llu,%llu,%d\n", (unsigned long long)kv.first,
                         (uint32_t)(kv.first >> 32), (uint32_t)kv.first, k.w, k.h, k.format, k.levels,
                         (unsigned long long)k.defines, (unsigned long long)k.uploads,
                         (unsigned long long)k.binds, k.dumped ? 1 : 0);
        }
        std::fclose(f);
    }

    void read_index() {
        std::FILE *f = std::fopen((pack_dir_ + "/index.tsv").c_str(), "r");
        if (!f) {
            std::fprintf(stderr, "[remaster] pack: no index.tsv in %s\n", pack_dir_.c_str());
            return;
        }
        char line[512];
        while (std::fgets(line, sizeof line, f)) {
            char key[32], slot[32], file[400];
            if (line[0] == '#' || std::sscanf(line, "%31s %31s %399s", key, slot, file) != 3)
                continue;
            if (!std::strcmp(slot, "albedo"))
                pack_[std::strtoull(key, nullptr, 16)] = file;
        }
        std::fclose(f);
        std::fprintf(stderr, "[remaster] pack: %zu albedo entries from %s\n", pack_.size(), pack_dir_.c_str());
    }

    // R3 run 17 (D-124): the UI pack's rows, stored with absolute paths; a key also in the world pack takes the UI
    // file (HUD keys are not world keys, but the A2 batch upscaled a few HUD textures with the photo model).
    void read_ui_index(const std::string &dir, const char *what = "UI") {
        std::FILE *f = std::fopen((dir + "/index.tsv").c_str(), "r");
        if (!f) {
            std::fprintf(stderr, "[remaster] pack: no UI index.tsv in %s\n", dir.c_str());
            return;
        }
        char line[512];
        size_t won = 0, lazy = 0, rows = 0;
        while (std::fgets(line, sizeof line, f)) {
            char key[32], slot[32], file[400];
            if (line[0] == '#' || std::sscanf(line, "%31s %31s %399s", key, slot, file) != 3)
                continue;
            const bool ondemand = !std::strcmp(slot, "albedo_ondemand");
            if (!ondemand && std::strcmp(slot, "albedo"))
                continue;
            const uint64_t k = std::strtoull(key, nullptr, 16);
            won += pack_.count(k);
            pack_[k] = dir + "/" + file;
            if (ondemand) {
                ondemand_.insert(k);
                ++lazy;
            }
            ++ui_rows_;
            ++rows;
        }
        std::fclose(f);
        std::fprintf(stderr, "[remaster] pack: %zu %s entries from %s (%zu on demand, %zu replace earlier rows)\n",
                     rows, what, dir.c_str(), lazy, won);
    }

    bool on_ = false, changed_ = false, swap_ = true;
    size_t ui_rows_ = 0;
    std::unordered_set<uint64_t> ondemand_;
    std::unordered_set<uint64_t> mod_keys_;                // run 19: rows from the Mods folder (never changed later)
    std::unordered_map<uint64_t, uint64_t> mod_binds_;     // draw-binds of each mod key's replacement
    uint64_t mod_shown_ = 0, mod_replaced_binds_ = 0, mod_logged_ = 0;
    bool mod_log_ = false;
    std::string pack_dir_;
    std::unordered_map<uint64_t, std::string> pack_;
    uint64_t redefined_ = 0;
    uint64_t look_none_ = 0, look_fail_ = 0, look_reject_ = 0, look_loaded_ = 0, replaced_binds_ = 0,
             dropped_rewrite_ = 0;
    std::string dir_;
    std::unordered_map<uint64_t, Key> keys_;
    uint64_t frames_ = 0, defined_ = 0, late_ = 0, dropped_ = 0, dumped_ = 0, keyed_binds_ = 0,
             unkeyed_binds_ = 0;
    bool count_binds_ = false;   // RECOMP_REMASTER_TEXDUMP or _TEXSTATS (run 29: was every draw with a pack)
};

} // namespace remaster
