// settings.cpp - one JSON file per profile, holding every mod's settings.
//
// Values are 64-bit integers; a toggle is 0 or 1. That is the whole type
// system the settings page needs and a type a mod cannot misread.
//
// Remaster run 16 (external review of runs 11-15, "sparse saving should preserve intent"): three concepts.
//  - declared default: Entry::def, from the manifest/declare call; it may change between builds.
//  - explicit override: a key present in stored() (and so in the file). A player's choice is kept even when it
//    equals today's default, so a later default change never moves it.
//  - effective value: Entry::value, the override when there is one, otherwise the default (inherited).
// Who writes decides what the write means:
//  - the player (menus, settings page, game options, controls): mods_settings_set -> always an override;
//  - Restore defaults (whole page or one section): mods_settings_inherit -> the override is removed;
//  - mod code (presets, labels that follow other values, migrations, NativeOptions.ini; the mod API's
//    settings_set): mods_settings_set_derived -> a write of the declared default returns the key to the default
//    (a migration or preset never manufactures intent for the default), any other value is held as an override
//    (the only way to keep it; a key the player chose stays chosen, with the new value).
// The file stays {"mod/key": value}; "kit.settings/format": 2 marks this format. A file without it is a legacy
// snapshot (runs <= 14 saved every value, run 15 only the non-default ones): once, entries equal to the CURRENT
// default are read as inherited and the others as explicit, then the marker is written (g_legacy below).
//
// A transaction covers one mod's init: begin before it runs, roll back if it
// fails. Rollback restores BOTH the declarations and the persisted values,
// because a mod that changed another mod's setting and then failed must leave
// no trace of either.
#include "mods_internal.h"
#include "../runtime/layout.h"
#include "display_settings.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>
#include "../platform/os.h"

namespace {

struct Entry {
    uint32_t owner;
    std::string mod_id, key, label;
    int32_t kind;
    int64_t value, min, max;
    int64_t step = 1;
    std::string unit;
    std::string group, help, apply, choices; // settings menu metadata (manifest)
    int64_t def = 0;                         // the declared default (Test85: Restore defaults)
};

std::vector<Entry> &entries() {
    static std::vector<Entry> v;
    return v;
}
// "mod_id/key" -> value, the persisted form.
std::map<std::string, int64_t> &stored() {
    static std::map<std::string, int64_t> m;
    return m;
}
std::string g_path;
// The format marker (see the top). Read like any key, so older builds keep it as an unknown key.
const char *const kFormatKey = "kit.settings/format";
constexpr int64_t kFormat = 2;
// The loaded file was a legacy snapshot: a value equal to its declared default meant nothing more than "the
// default when it was saved", so it is read as inherited. Set by mods_settings_load for the whole session (every
// key is read this way once, when first declared; a later re-declaration after a mod reload is not); the marker
// written by the first save means the next session reads the file as overrides. A key whose mod is not loaded in
// that session keeps its value, as explicit.
bool g_legacy = false;
std::set<std::string> g_legacy_done;

std::string skey(const Entry &e) {
    return e.mod_id + "/" + e.key;
}
// Legacy snapshot reading for one declared entry (idempotent; nothing once the marker exists).
void legacy_inherit(const Entry &e) {
    if (!g_legacy || !g_legacy_done.insert(skey(e)).second)
        return;
    auto it = stored().find(skey(e));
    if (it != stored().end() && it->second == e.def)
        stored().erase(it);
}

// Transaction snapshot.
bool g_in_txn = false;
std::vector<Entry> g_txn_entries;
std::map<std::string, int64_t> g_txn_stored;
std::set<std::string> g_txn_legacy_done;

// Remaster run 29 (D-167, review/run29/cleanup-mod.md 2): mods read settings many times a frame, and a linear scan of
// ~160 entries comparing strings made every read cost O(keys). A self-checking index: (owner, key) hash -> the entry's
// position, verified on every hit; a miss or a stale position (entries are pushed, sorted, erased and restored by
// transactions) falls back to the scan and repairs the slot. Answers are the scan's, always.
std::unordered_map<uint64_t, size_t> &index_map() {
    static std::unordered_map<uint64_t, size_t> m;
    return m;
}
uint64_t key_hash(uint32_t owner, const char *key) {
    uint64_t h = 1469598103934665603ull ^ owner;
    for (const char *c = key; *c; ++c)
        h = (h ^ (unsigned char)*c) * 1099511628211ull;
    return h;
}
Entry *find(uint32_t owner, const char *key) {
    std::vector<Entry> &v = entries();
    const uint64_t h = key_hash(owner, key);
    auto it = index_map().find(h);
    if (it != index_map().end() && it->second < v.size() && v[it->second].owner == owner && v[it->second].key == key)
        return &v[it->second];
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].owner == owner && v[i].key == key) {
            index_map()[h] = i;
            return &v[i];
        }
    return nullptr;
}

void sort_entries() {
    std::stable_sort(entries().begin(), entries().end(),
                     [](const Entry &a, const Entry &b) { return a.owner < b.owner; });
}

} // namespace

const char *mods_settings_path() {
    if (g_path.empty())
        g_path = std::string(mods_overlay_profile_dir()) + "/mod-settings.json";
    return g_path.c_str();
}

void mods_settings_reset() {
    mods_display_reset();
    entries().clear();
    stored().clear();
    g_path.clear();
    g_in_txn = false;
    g_legacy = false;
    g_legacy_done.clear();
}

// A saved value outside the entry's range is unusable: the entry keeps its value (the default at declare) and the
// override is dropped (run 15: at the next save). Remaster run 29 (NFSMW D-163, the retired PC look): said once, so a profile's move is in its log.
static void take_stored(Entry &e) {
    auto it = stored().find(skey(e));
    if (it == stored().end())
        return;
    if (it->second >= e.min && it->second <= e.max) {
        e.value = it->second;
        return;
    }
    printf("mods: settings: %s saved value %lld is no longer offered (%lld..%lld): %lld is used\n",
           skey(e).c_str(), (long long)it->second, (long long)e.min, (long long)e.max, (long long)e.value);
    stored().erase(it);
}

bool mods_settings_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return true; // no file yet is not an error
    std::string text;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        text.append(buf, n);
    fclose(f);

    // {"mod.id/key": 7, ...} - the same shape the registry store uses.
    size_t i = 0;
    while ((i = text.find('"', i)) != std::string::npos) {
        size_t end = text.find('"', i + 1);
        if (end == std::string::npos)
            break;
        std::string key = text.substr(i + 1, end - i - 1);
        size_t colon = text.find(':', end);
        if (colon == std::string::npos)
            break;
        stored()[key] = strtoll(text.c_str() + colon + 1, nullptr, 10);
        i = text.find(',', colon);
        if (i == std::string::npos)
            break;
    }
    auto marker = stored().find(kFormatKey);
    g_legacy = marker == stored().end() ? !stored().empty() : marker->second < kFormat;
    if (g_legacy)
        printf("mods: settings: %s has no format marker (a full or run-15 snapshot): values equal to today's "
               "defaults now follow the defaults, the others stay as chosen (once)\n", path);
    for (Entry &e : entries()) {
        legacy_inherit(e);
        take_stored(e);
    }
    return true;
}

bool mods_settings_save() {
    if (g_in_txn)
        return false; // an init rollback must never reach disk
    // Remaster run 16: stored() is the set of explicit overrides; nothing is inferred from equality here. An override
    // follows its entry's value (a range change may have clamped it); keys of mods not loaded now stay as they were.
    for (const Entry &e : entries()) {
        auto it = stored().find(skey(e));
        if (it != stored().end())
            it->second = e.value;
    }
    stored()[kFormatKey] = kFormat;
    const std::string path = mods_settings_path();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    if (ec)
        return false;
    std::string temporary = path + ".tmp.XXXXXX";
    int fd = os_mkstemp(temporary.data());
    if (fd < 0)
        return false;
    FILE *f = (FILE *)os_fdopen(fd, "wb");
    if (!f) {
        os_fd_close(fd);
        os_unlink(temporary.c_str());
        return false;
    }
    fprintf(f, "{\n");
    bool first = true;
    for (const auto &kv : stored()) {
        fprintf(f, "%s \"%s\": %lld", first ? "" : ",\n", kv.first.c_str(), (long long)kv.second);
        first = false;
    }
    fprintf(f, "\n}\n");
    bool ok = !ferror(f) && fflush(f) == 0 && os_fd_fsync(fd) == 0;
    if (fclose(f) != 0)
        ok = false;
    if (ok)
        ok = os_rename(temporary.c_str(), path.c_str()) == 0;
    if (!ok)
        os_unlink(temporary.c_str());
    return ok;
}

void mods_settings_declare(uint32_t owner, const char *mod_id, const char *key, const char *label,
                           int32_t kind, int64_t def, int64_t min, int64_t max) {
    if (!key || find(owner, key))
        return;
    Entry e{owner, mod_id ? mod_id : "", key, label && *label ? label : key, kind, def, min, max};
    e.def = def;
    legacy_inherit(e);
    take_stored(e);
    entries().push_back(e);
    sort_entries();
}

void mods_settings_set_range(uint32_t owner, const char *key, int64_t min, int64_t max) {
    Entry *e = key ? find(owner, key) : nullptr;
    if (!e || min > max)
        return;
    e->min = min;
    e->max = max;
    if (e->value < min)
        e->value = min;
    if (e->value > max)
        e->value = max;
    auto it = stored().find(skey(*e));
    if (it != stored().end())
        it->second = e->value;
}

// Test85: last-known-good graphics. Apply in the settings menu records the values it replaces in
// <settings>.graphics-pending; the file goes once the game has run 20 s after that Apply, or at a clean
// shutdown. If it is still there at the next launch, that Apply never proved itself (crash, hang, killed), so its
// previous values are put back before anything reads them.
namespace {
std::string pending_path() {
    return std::string(mods_settings_path()) + ".graphics-pending";
}
std::map<std::string, int64_t> parse_values(const std::string &text) {
    std::map<std::string, int64_t> out;
    size_t i = 0;
    while ((i = text.find('"', i)) != std::string::npos) {
        const size_t end = text.find('"', i + 1);
        if (end == std::string::npos)
            break;
        const size_t colon = text.find(':', end);
        if (colon == std::string::npos)
            break;
        out[text.substr(i + 1, end - i - 1)] = strtoll(text.c_str() + colon + 1, nullptr, 10);
        i = text.find(',', colon);
        if (i == std::string::npos)
            break;
    }
    return out;
}
} // namespace

void mods_cpp_settings_mark_pending(const std::vector<std::pair<std::string, int64_t>> &previous) {
    if (previous.empty())
        return;
    FILE *f = fopen(pending_path().c_str(), "wb");
    if (!f)
        return;
    fprintf(f, "{\n");
    // Run 16: a key that only followed its default is recorded as "~mod/key", so a recovery returns it to the
    // default rather than turning the old value into a choice. Older pending files have no "~" keys.
    for (size_t i = 0; i < previous.size(); ++i)
        fprintf(f, "%s \"%s%s\": %lld", i ? ",\n" : "", stored().count(previous[i].first) ? "" : "~",
                previous[i].first.c_str(), (long long)previous[i].second);
    fprintf(f, "\n}\n");
    fclose(f);
}

void mods_settings_clear_pending() {
    std::error_code ec;
    if (std::filesystem::remove(pending_path(), ec))
        printf("mods: graphics change confirmed (the game kept running); last-known-good updated\n");
}

int mods_settings_recover_pending() {
    FILE *f = fopen(pending_path().c_str(), "rb");
    if (!f)
        return 0;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        text.append(buf, n);
    fclose(f);
    const auto previous = parse_values(text);
    std::string list;
    for (const auto &[raw, value] : previous) {
        const bool inherited = !raw.empty() && raw[0] == '~';
        const std::string key = inherited ? raw.substr(1) : raw;
        if (inherited)
            stored().erase(key);
        else
            stored()[key] = value;
        for (Entry &e : entries())
            if (skey(e) == key && (inherited || (value >= e.min && value <= e.max)))
                e.value = inherited ? std::clamp(e.def, e.min, e.max) : value;
        list += " " + key + (inherited ? "=default" : "=" + std::to_string(value));
    }
    std::error_code ec;
    std::filesystem::remove(pending_path(), ec);
    if (!previous.empty()) {
        // Written straight into the stored form: nothing is declared yet at this point.
        const std::string path = mods_settings_path();
        if (FILE *w = fopen((path + ".tmp-recover").c_str(), "wb")) {
            fprintf(w, "{\n");
            bool first = true;
            for (const auto &kv : stored()) {
                fprintf(w, "%s \"%s\": %lld", first ? "" : ",\n", kv.first.c_str(), (long long)kv.second);
                first = false;
            }
            fprintf(w, "\n}\n");
            fclose(w);
            std::filesystem::rename(path + ".tmp-recover", path, ec);
        }
    }
    printf("mods: the last graphics Apply did not see the game run 20 s (crash, hang or forced quit); its "
           "previous values are back:%s\n", list.c_str());
    fflush(stdout);
    return int(previous.size());
}

int64_t mods_setting_value(const char *mod_id_slash_key, int64_t fallback) {
    if (!mod_id_slash_key)
        return fallback;
    for (const Entry &e : entries())
        if (e.mod_id.size() + 1 + e.key.size() == strlen(mod_id_slash_key) &&
            !strncmp(mod_id_slash_key, e.mod_id.c_str(), e.mod_id.size()) &&
            mod_id_slash_key[e.mod_id.size()] == '/' && e.key == mod_id_slash_key + e.mod_id.size() + 1)
            return e.value;
    return fallback;
}

bool mods_settings_default(uint32_t i, int64_t *out) {
    if (i >= entries().size() || !out)
        return false;
    *out = entries()[i].def;
    return true;
}

bool mods_settings_stored_value(const char *mod_id_slash_key, int64_t *out) {
    if (!mod_id_slash_key || !out)
        return false;
    auto it = stored().find(mod_id_slash_key);
    if (it == stored().end())
        return false;
    *out = it->second;
    return true;
}

PopModStatus mods_settings_get(uint32_t owner, const char *key, int64_t *out) {
    if (!key || !out)
        return POP_E_INVAL;
    Entry *e = find(owner, key);
    if (!e)
        return POP_E_NOTFOUND;
    *out = e->value;
    return POP_OK;
}

namespace {
// One write of an entry's value and override state, saved at once; the old state comes back if the save fails.
PopModStatus commit(Entry *e, int64_t v, bool override_after) {
    const std::string k = skey(*e);
    auto it = stored().find(k);
    const bool had = it != stored().end();
    const int64_t had_value = had ? it->second : 0;
    if (v == e->value && had == override_after && (!had || had_value == v))
        return POP_OK;
    const int64_t previous = e->value;
    e->value = v;
    if (override_after)
        stored()[k] = v;
    else
        stored().erase(k);
    if (!g_in_txn && !mods_settings_save()) {
        e->value = previous;
        if (had)
            stored()[k] = had_value;
        else
            stored().erase(k);
        LOGW("settings: could not save %s", mods_settings_path());
        return POP_E_STATE;
    }
    return POP_OK;
}
} // namespace

// The player's choice: always an explicit override, even when it equals the default. A write of the value already
// there changes nothing (kit code re-sends values, e.g. the controls' layout row after a name refresh).
PopModStatus mods_settings_set(uint32_t owner, const char *key, int64_t v) {
    if (!key)
        return POP_E_INVAL;
    Entry *e = find(owner, key);
    if (!e)
        return POP_E_NOTFOUND;
    if (v < e->min || v > e->max)
        return POP_E_RANGE;
    if (v == e->value)
        return POP_OK;
    return commit(e, v, true);
}

// Mod code's write (presets, labels, migrations, NativeOptions.ini): see the top of this file.
PopModStatus mods_settings_set_derived(uint32_t owner, const char *key, int64_t v) {
    if (!key)
        return POP_E_INVAL;
    Entry *e = find(owner, key);
    if (!e)
        return POP_E_NOTFOUND;
    if (v < e->min || v > e->max)
        return POP_E_RANGE;
    if (v == e->value)
        return POP_OK; // nothing changes, so neither does who owns the value
    return commit(e, v, v != e->def);
}

// Restore defaults: the override goes and the key follows its declared default again.
PopModStatus mods_settings_inherit(uint32_t owner, const char *key) {
    if (!key)
        return POP_E_INVAL;
    Entry *e = find(owner, key);
    if (!e)
        return POP_E_NOTFOUND;
    return commit(e, std::clamp(e->def, e->min, e->max), false);
}

bool mods_settings_is_override(const char *mod_id_slash_key) {
    return mod_id_slash_key && stored().count(mod_id_slash_key) != 0;
}

void mods_settings_remove_all(uint32_t owner) {
    for (auto it = entries().begin(); it != entries().end();)
        it = (it->owner == owner) ? entries().erase(it) : it + 1;
}

void mods_settings_txn_begin() {
    g_txn_entries = entries();
    g_txn_stored = stored();
    g_txn_legacy_done = g_legacy_done;
    g_in_txn = true;
}

void mods_settings_txn_commit() {
    g_in_txn = false;
}

void mods_settings_txn_rollback() {
    if (!g_in_txn)
        return;
    entries() = g_txn_entries;
    stored() = g_txn_stored;
    g_legacy_done = g_txn_legacy_done;
    g_in_txn = false;
}

void mods_settings_set_presentation(uint32_t owner, const char *key, int64_t step,
                                    const char *unit) {
    Entry *e = key ? find(owner, key) : nullptr;
    if (!e)
        return;
    e->step = step >= 1 ? step : 1;
    e->unit = unit && !strcmp(unit, "$") ? "$" : "";
}

void mods_settings_set_meta(uint32_t owner, const char *key, const char *group, const char *help,
                            const char *apply, const char *choices) {
    Entry *e = key ? find(owner, key) : nullptr;
    if (!e)
        return;
    e->group = group ? group : "";
    e->help = help ? help : "";
    e->apply = apply ? apply : "";
    e->choices = choices ? choices : "";
}

bool mods_settings_meta(uint32_t i, const char **group, const char **help, const char **apply,
                        const char **choices) {
    if (i >= entries().size())
        return false;
    const Entry &e = entries()[i];
    if (group)
        *group = e.group.c_str();
    if (help)
        *help = e.help.c_str();
    if (apply)
        *apply = e.apply.c_str();
    if (choices)
        *choices = e.choices.c_str();
    return true;
}

bool mods_settings_presentation(uint32_t i, int64_t *step, const char **unit) {
    if (i >= entries().size())
        return false;
    if (step)
        *step = entries()[i].step;
    if (unit)
        *unit = entries()[i].unit.c_str();
    return true;
}

uint32_t mods_settings_entry_count() {
    return (uint32_t)entries().size();
}

bool mods_settings_entry(uint32_t i, uint32_t *owner, const char **mod_id, const char **key,
                         const char **label, int32_t *kind, int64_t *value, int64_t *min,
                         int64_t *max) {
    if (i >= entries().size())
        return false;
    const Entry &e = entries()[i];
    if (owner)
        *owner = e.owner;
    if (mod_id)
        *mod_id = e.mod_id.c_str();
    if (key)
        *key = e.key.c_str();
    if (label)
        *label = e.label.c_str();
    if (kind)
        *kind = e.kind;
    if (value)
        *value = e.value;
    if (min)
        *min = e.min;
    if (max)
        *max = e.max;
    return true;
}

// The profile directory is the overlay's to own (Task 7). Weak here so this
// task's tests link in wave 1; the overlay's definition wins in every build
// that has one.
extern "C" __attribute__((weak)) const char *mods_overlay_profile_dir(void) {
    return host_layout().profile_dir.c_str();
}
extern "C" __attribute__((weak)) void mods_overlay_set_profile_dir(const char *) {}
