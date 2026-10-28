// launcher_mods.h - the launcher's mod screen model for a Sefaris Mod Manager
// catalog (Gothic 3): reading <root>/Mods/*/mod.json and
// <root>/presets/*/preset.json, the selection rules Sefaris applies while the
// player clicks, the load order, and the files the launcher keeps:
// <profile>/launcher-mods.json (the choice between runs) and
// <profile>/active-mods.json (what the game loads, written at Play).
// Portable: no SDL, no UI. The rules follow docs/mods/sefaris.md.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace launcher {

struct ModInfo {
    std::string id, title, category; // category "" when mod.json has none
    std::vector<std::string> dependencies, incompatibles, authors;
    std::string dir;    // the folder's name under Mods/
    std::string path;   // its absolute path
    std::string cover;  // images/315x312.jpg, else the first .jpg in images/; "" if none
    std::string readme; // Readme/readme_pl.md, else _gb, else _de; "" if none
    // A Script_*.dll anywhere in the folder: code the port does not run yet,
    // so the mod cannot be enabled.
    std::string script_dll; // its path relative to the mod folder; "" when none
    // That DLL is one the port has redone as native code ([launcher] native_dlls): the mod runs
    // always, without its DLL, and the launcher shows it as built in rather than blocked.
    bool builtin = false;
    // That DLL is one the port has redone natively for the player to switch ([launcher]
    // native_mod_dlls): the mod is enabled like one without a DLL, and its DLL is never loaded.
    bool native = false;
    bool supported() const {
        return script_dll.empty() || native;
    }
};

struct ModPreset {
    std::string name, dir;
    std::vector<std::string> mod_ids;
};

struct ModCatalog {
    std::string root;
    std::vector<ModInfo> mods; // in load order (mod_load_order)
    std::vector<ModPreset> presets;
    std::vector<std::string> errors; // folders skipped, files that did not parse
    bool found = false;              // root/Mods exists

    // Index into mods by id, compared without case as Sefaris does; -1 if none.
    int find(const std::string &id) const;
    // The categories in the order the screen lists them (alphabetical, no case;
    // mods without one are "Uncategorized", last).
    std::vector<std::string> categories() const;
};

// Mod ids are compared without case.
bool same_id(const std::string &a, const std::string &b);

// The catalog when the player has not chosen one: $RECOMP_SEFARIS_DIR, else
// `configured` (Spec::mod_root) with a leading "~/" as the home folder.
std::string default_mod_root(const std::string &configured);

// Reads the catalog. Missing pieces are not fatal: a folder without a valid
// mod.json is skipped and named in errors.
ModCatalog load_mod_catalog(const std::string &root);

// THE load order. Sefaris installs the selected mods in the order of the
// folders in Mods/ as NTFS lists them: names compared after upper-casing,
// byte by byte, so '_' (0x5F) sorts after the letters (ZZ_ before Z_,
// Script_ before S_) and digits before them. Category, click order and
// preset order do not matter. Sorts folder names in place.
void mod_load_order(std::vector<std::string> *dirs);
bool mod_dir_before(const std::string &a, const std::string &b);

// The selection as Sefaris keeps it while the player clicks
// (ModCheckboxTooltip.vue): enabling a mod enables its dependencies,
// recursively; a mod incompatible with an enabled one (either way) is locked;
// a mod an enabled one depends on cannot be disabled. A preset sets the
// selection directly, without those checks.
struct ModSelection {
    std::vector<std::string> ids; // enabled, as the catalog spells them
    std::string preset;           // the preset chosen last ("" for none); kept after edits, as Sefaris does

    bool enabled(const std::string &id) const;
};

// Why `id` cannot be enabled now; "" when it can. Names the blocking mods.
std::string mod_blocked(const ModCatalog &cat, const ModSelection &sel, const std::string &id);
// Why `id` cannot be disabled now (enabled mods that depend on it); "" when it can.
std::string mod_required_by(const ModCatalog &cat, const ModSelection &sel, const std::string &id);
// Enables `id` with its dependencies, or disables it. Returns "" when done,
// else why nothing changed.
std::string mod_toggle(const ModCatalog &cat, ModSelection *sel, const std::string &id);
// Applies a preset: its ids that the catalog has and the port supports.
// *missing gets the ids the catalog lacks, *unsupported those it skipped.
void mod_apply_preset(const ModCatalog &cat, ModSelection *sel, const ModPreset &preset,
                      std::vector<std::string> *missing, std::vector<std::string> *unsupported);

// What is wrong with the selection as it stands (after a preset, or a catalog
// that changed): missing dependencies, conflicts, unsupported or unknown mods.
struct ModProblem {
    std::string id, text;
    bool conflict = false;
};
std::vector<ModProblem> mod_problems(const ModCatalog &cat, const ModSelection &sel);

// The enabled mods in load order.
std::vector<const ModInfo *> mod_active(const ModCatalog &cat, const ModSelection &sel);

// <profile>/launcher-mods.json: {"version":1,"root":..., "preset":..., "mods":[ids]}.
// load returns false when there is no saved choice.
bool load_mod_choice(const std::string &file, std::string *root, ModSelection *sel);
bool save_mod_choice(const std::string &file, const std::string &root, const ModSelection &sel);

// <profile>/active-mods.json, the contract with the game's mod loading:
// {"version":1,"preset":"<name>"|null,"static":"<catalog>/Static" (when it
// holds Projects_compiled.m0x),"mods":[{"id":...,"path":...}]} in load order
// (a later mod overrides an earlier one). Written as .part and renamed.
std::string active_mods_json(const ModCatalog &cat, const ModSelection &sel);
bool write_active_mods(const std::string &file, const ModCatalog &cat, const ModSelection &sel);

// The view of the mods the game reads: tools/g3mods.py `build`, the same
// rules, so a player who starts the app alone (no Python) gets it at Play.
// From <profile>/active-mods.json, symbolic links in <profile>/Data
// (<base>.mod, .m00, .m01 ... per base name in load order, the .nod chain the
// same way, numbering after what <game_data> already holds) and
// <profile>/ini (a later mod's ini wins), each to the file in the mod folder.
// As Sefaris does, strings.mod (the game's string table and every mod's
// stringtable.ini) and, with the catalog's Static/, the merged world sector
// list and Static's cache-hiding archives go last on the projects_compiled
// chains; the two merged archives are written to <profile>/mods-generated/,
// again only when their inputs changed. Program files, other stringtable
// variants, ge3.ini and .pak/.cpt generations are left out with a reason.
// <profile>/mods-view.json records the links and generated files; the next
// build removes exactly those links that are still links, and a regular file
// in the way is never touched. Without active-mods.json the old view goes and
// nothing is written.
struct ModViewLink {
    std::string path, target, mod; // path relative to the profile ("Data/x.mod")
};
struct ModViewSkip {
    std::string mod, file, reason;
};
// A file the build writes into <profile>/mods-generated: "strings" (base: the
// game's Strings archive) or "sectors" (base: Static/G3_World_01.wrldatasc),
// from the mods' inputs in load order; signature as tools/g3mods.py writes it.
struct ModViewGen {
    std::string file, kind, base, signature;
    std::vector<std::string> inputs;
};
struct ModView {
    std::vector<ModViewLink> links;
    std::vector<ModViewGen> generated;
    std::vector<ModViewSkip> skipped;
    std::vector<std::string> errors;
};
// G3V0 archives (tools/g3pak.py): the unpacked entry `key` (any case), and
// entries packed as G3PakDir.exe --no-comments does; a null entry is a deleted
// one, which hides that name in lower archives.
bool g3pak_read(const std::string &archive, const std::string &key, std::string *out);
bool g3pak_write(const std::string &archive,
                 const std::vector<std::pair<std::string, const std::string *>> &entries, uint64_t filetime);
// What build would link and generate, nothing touched (g3mods.py plan).
ModView plan_mod_view(const std::string &profile, const std::string &game_data);
// Removes the links the last build recorded and the record, and unless
// `generated` is false the files it generated; the count of links removed.
int clear_mod_view(const std::string &profile, bool generated = true);
ModView build_mod_view(const std::string &profile, const std::string &game_data);
std::string mod_view_json(const ModView &view);

// A readme as the screen shows it: lines of text, headings marked. Markdown is
// simplified: '#' headings, list bullets as "- ", emphasis, code marks and
// links reduced to their text, images and HTML tags dropped.
struct ReadmeLine {
    std::string text;
    bool heading = false;
};
std::vector<ReadmeLine> simplify_markdown(const std::string &text);
// A mod's readme file, simplified; empty when it cannot be read.
std::vector<ReadmeLine> load_readme(const std::string &path);

// UTF-8 as the launcher's ASCII font can draw it: Polish, German and other
// Latin letters lose their marks, typographic quotes, dashes and bullets
// become ASCII, anything else '?'.
std::string ascii_text(const std::string &utf8);

// A decoded picture, RGBA8, rows packed.
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
};
// `src` scaled to fill w x h, cropped to keep its aspect (box filter).
Image scale_cover(const Image &src, int w, int h);
// The pixel size of a baseline or progressive JPEG from its SOF marker.
bool jpeg_size(const uint8_t *data, size_t size, int *w, int *h);

} // namespace launcher
