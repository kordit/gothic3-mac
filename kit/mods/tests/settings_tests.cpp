// settings_tests.cpp - the per-profile settings store, including the
// transaction a failed mod init rolls back.
#include "mods_tests.h"
#include "../mods_internal.h"
#include "../controls_settings.h"
#include "../display_settings.h"
#include "../../runtime/layout.h"
#include "../../runtime/win32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include "../../platform/os.h"

extern "C" bool mods_test_reset_loader(void);

namespace {
const char *PROFILE = nullptr; // this suite's own, from mod_test_dir
void fresh() {
    PROFILE = mod_test_dir("settings");
    os_setenv("RECOMP_PROFILE_DIR", PROFILE);
    mods_settings_reset();
    mods_overlay_set_profile_dir(PROFILE);
}
} // namespace

MOD_TEST_SUITE(settings_declare_and_round_trip) {
    fresh();
    mods_settings_declare(2, "a.mod", "ui_scale", "UI scale", POP_SETTING_INT, 2, 1, 4);
    mods_settings_declare(2, "a.mod", "shadows", "Shadows", POP_SETTING_BOOL, 1, 0, 1);

    int64_t v = 0;
    MOD_CHECK_EQ(mods_settings_get(2, "ui_scale", &v), POP_OK);
    MOD_CHECK_EQ(v, 2); // the declared default
    MOD_CHECK_EQ(mods_settings_set(2, "ui_scale", 4), POP_OK);
    MOD_CHECK_EQ(mods_settings_get(2, "ui_scale", &v), POP_OK);
    MOD_CHECK_EQ(v, 4);
    MOD_CHECK_EQ(mods_settings_set(2, "ui_scale", 9), POP_E_RANGE);
    MOD_CHECK_EQ(mods_settings_get(2, "ui_scale", &v), POP_OK);
    MOD_CHECK_EQ(v, 4); // a refused write changes nothing
    MOD_CHECK_EQ(mods_settings_get(2, "nope", &v), POP_E_NOTFOUND);
    // Another mod's key is not this mod's, even under the same name.
    MOD_CHECK_EQ(mods_settings_get(3, "ui_scale", &v), POP_E_NOTFOUND);

    // A boolean default written as `true` is 1, not 0.
    mods_settings_declare(3, "b.mod", "enabled", "Enabled", POP_SETTING_BOOL, 1, 0, 1);
    MOD_CHECK_EQ(mods_settings_get(3, "enabled", &v), POP_OK);
    MOD_CHECK_EQ(v, 1);

    // One JSON file per profile, reloaded next run, keyed by mod id and key.
    MOD_CHECK(std::string(mods_settings_path()).find(PROFILE) == 0);
    // No shutdown/save call: a successful settings change is durable already.
    mods_settings_reset();
    MOD_CHECK(mods_settings_load(mods_settings_path()));
    mods_settings_declare(2, "a.mod", "ui_scale", "UI scale", POP_SETTING_INT, 2, 1, 4);
    MOD_CHECK_EQ(mods_settings_get(2, "ui_scale", &v), POP_OK);
    MOD_CHECK_EQ(v, 4); // the persisted value beats the default

    // Remaster run 16 (replaces run 15's sparse rule): a value the player sets back to its default is still the
    // player's choice, so a later default change does not move it; Restore (inherit) is what follows the default.
    MOD_CHECK_EQ(mods_settings_set(2, "ui_scale", 2), POP_OK);
    MOD_CHECK(mods_settings_is_override("a.mod/ui_scale"));
    mods_settings_reset();
    MOD_CHECK(mods_settings_load(mods_settings_path()));
    mods_settings_declare(2, "a.mod", "ui_scale", "UI scale", POP_SETTING_INT, 3, 1, 4); // the new default
    MOD_CHECK_EQ(mods_settings_get(2, "ui_scale", &v), POP_OK);
    MOD_CHECK_EQ(v, 2);
    MOD_CHECK_EQ(mods_settings_inherit(2, "ui_scale"), POP_OK);
    MOD_CHECK_EQ(mods_settings_get(2, "ui_scale", &v), POP_OK);
    MOD_CHECK_EQ(v, 3);
    MOD_CHECK(!mods_settings_is_override("a.mod/ui_scale"));
}

namespace {
std::string read_profile() {
    std::string text;
    if (FILE *f = fopen(mods_settings_path(), "rb")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0)
            text.append(buf, n);
        fclose(f);
    }
    return text;
}
void write_profile(const char *text) {
    FILE *f = fopen(mods_settings_path(), "wb");
    MOD_CHECK(f != nullptr);
    if (f) {
        MOD_CHECK(fputs(text, f) >= 0);
        MOD_CHECK_EQ(fclose(f), 0);
    }
}
// A fresh session over the same profile: nothing declared, the file read again.
void relaunch() {
    mods_settings_reset();
    mods_overlay_set_profile_dir(PROFILE);
    MOD_CHECK(mods_settings_load(mods_settings_path()));
}
// core.nfsmw's settings_migrate step 1 through the mod API's write (derived), as nfsmw.c does it.
void sky_migration() {
    int64_t version = 0, sky = 2;
    mods_settings_get(2, "settings_version", &version);
    if (version >= 1)
        return;
    mods_settings_get(2, "x360_sky", &sky);
    if (sky == 1)
        mods_settings_set_derived(2, "x360_sky", 2);
    mods_settings_set_derived(2, "settings_version", 1);
}
} // namespace

// Remaster run 16 (review of runs 11-15, "sparse saving should preserve intent"): declared default, effective value,
// explicit override.
MOD_TEST_SUITE(settings_explicit_choice_survives_default_change) {
    fresh();
    remove(mods_settings_path());
    relaunch();
    mods_settings_declare(2, "a.mod", "bloom", "Bloom", POP_SETTING_BOOL, 0, 0, 1);
    mods_settings_declare(2, "a.mod", "fog", "Fog", POP_SETTING_BOOL, 0, 0, 1);
    // The player picks Off while Off is the default: kept as a choice. Fog is never touched: inherited.
    MOD_CHECK_EQ(mods_settings_set(2, "bloom", 1), POP_OK);
    MOD_CHECK_EQ(mods_settings_set(2, "bloom", 0), POP_OK);
    MOD_CHECK(mods_settings_is_override("a.mod/bloom"));
    MOD_CHECK(!mods_settings_is_override("a.mod/fog"));
    MOD_CHECK(read_profile().find("\"a.mod/bloom\": 0") != std::string::npos);
    MOD_CHECK(read_profile().find("a.mod/fog") == std::string::npos);
    MOD_CHECK(read_profile().find("\"kit.settings/format\": 2") != std::string::npos);
    // A later build turns both on by default: the choice stays Off, the inherited one follows.
    relaunch();
    mods_settings_declare(2, "a.mod", "bloom", "Bloom", POP_SETTING_BOOL, 1, 0, 1);
    mods_settings_declare(2, "a.mod", "fog", "Fog", POP_SETTING_BOOL, 1, 0, 1);
    int64_t v = -1;
    MOD_CHECK_EQ(mods_settings_get(2, "bloom", &v), POP_OK);
    MOD_CHECK_EQ(v, 0);
    MOD_CHECK_EQ(mods_settings_get(2, "fog", &v), POP_OK);
    MOD_CHECK_EQ(v, 1);
    mods_settings_reset();
}

// Mod code (presets, labels, migrations) neither manufactures nor erases a choice by writing what is already there;
// writing the declared default returns a key to the default, any other value is held.
MOD_TEST_SUITE(settings_derived_writes_keep_ownership) {
    fresh();
    remove(mods_settings_path());
    relaunch();
    mods_settings_declare(2, "a.mod", "exposure", "Exposure", POP_SETTING_INT, 50, 0, 100);
    mods_settings_declare(2, "a.mod", "quality", "Quality", POP_SETTING_INT, 2, 0, 4);
    MOD_CHECK_EQ(mods_settings_set_derived(2, "exposure", 50), POP_OK); // a recipe writes the default: no choice
    MOD_CHECK(!mods_settings_is_override("a.mod/exposure"));
    MOD_CHECK_EQ(mods_settings_set(2, "exposure", 50), POP_OK);         // re-sending the value: nothing changes
    MOD_CHECK(!mods_settings_is_override("a.mod/exposure"));
    MOD_CHECK_EQ(mods_settings_set(2, "exposure", 60), POP_OK);
    MOD_CHECK_EQ(mods_settings_set(2, "exposure", 50), POP_OK);         // the player picks it back: a choice
    MOD_CHECK_EQ(mods_settings_set_derived(2, "exposure", 50), POP_OK); // the recipe again: the choice stays
    MOD_CHECK(mods_settings_is_override("a.mod/exposure"));
    MOD_CHECK_EQ(mods_settings_set_derived(2, "quality", 4), POP_OK);   // a label that is not the default is held
    MOD_CHECK(mods_settings_is_override("a.mod/quality"));
    MOD_CHECK_EQ(mods_settings_set_derived(2, "quality", 2), POP_OK);   // back at the default: follows it
    MOD_CHECK(!mods_settings_is_override("a.mod/quality"));
    MOD_CHECK_EQ(mods_settings_set_derived(2, "quality", 9), POP_E_RANGE);
    mods_settings_reset();
}

// Restore removes overrides, only for the keys asked; other mods' keys in the file are kept untouched.
MOD_TEST_SUITE(settings_inherit_scope_and_unknown_keys) {
    fresh();
    write_profile("{\n \"a.mod/glow\": 90,\n \"a.mod/volume\": 30,\n \"kit.settings/format\": 2,\n"
                  " \"other.mod/x\": 5\n}\n");
    relaunch();
    mods_settings_declare(2, "a.mod", "glow", "Glow", POP_SETTING_INT, 40, 0, 100);
    mods_settings_declare(2, "a.mod", "volume", "Volume", POP_SETTING_INT, 100, 0, 100);
    MOD_CHECK_EQ(mods_settings_inherit(2, "glow"), POP_OK);
    int64_t v = -1;
    MOD_CHECK_EQ(mods_settings_get(2, "glow", &v), POP_OK);
    MOD_CHECK_EQ(v, 40);
    MOD_CHECK(!mods_settings_is_override("a.mod/glow"));
    MOD_CHECK(mods_settings_is_override("a.mod/volume"));
    MOD_CHECK_EQ(mods_settings_inherit(2, "nope"), POP_E_NOTFOUND);
    const std::string text = read_profile();
    MOD_CHECK(text.find("\"other.mod/x\": 5") != std::string::npos);
    MOD_CHECK(text.find("\"a.mod/volume\": 30") != std::string::npos);
    MOD_CHECK(text.find("a.mod/glow") == std::string::npos);
    mods_settings_reset();
}

// A legacy snapshot (no format marker): once, values equal to today's default follow the default, others stay as
// chosen; the sky migration through the derived write leaves the sky inherited; running it all again changes nothing.
MOD_TEST_SUITE(settings_legacy_snapshot_and_migration_idempotence) {
    fresh();
    write_profile("{\n \"a.mod/aa\": 0,\n \"a.mod/sky\": 1,\n \"a.mod/x360_sky\": 1,\n \"a.mod/volume\": 70,\n"
                  " \"other.mod/x\": 5\n}\n");
    relaunch();
    auto declare_all = [](int64_t aa_default) {
        mods_settings_declare(2, "a.mod", "aa", "AA", POP_SETTING_INT, aa_default, 0, 2);
        mods_settings_declare(2, "a.mod", "sky", "Sky", POP_SETTING_INT, 2, 0, 2);
        mods_settings_declare(2, "a.mod", "x360_sky", "360 sky", POP_SETTING_INT, 2, 0, 2);
        mods_settings_declare(2, "a.mod", "volume", "Volume", POP_SETTING_INT, 100, 0, 100);
        mods_settings_declare(2, "a.mod", "settings_version", "v", POP_SETTING_INT, 0, 0, 100);
    };
    declare_all(0);
    MOD_CHECK(!mods_settings_is_override("a.mod/aa"));    // equal to today's default: inherited
    MOD_CHECK(mods_settings_is_override("a.mod/sky"));    // differs: explicit
    MOD_CHECK(mods_settings_is_override("a.mod/volume"));
    sky_migration();
    int64_t v = -1;
    MOD_CHECK_EQ(mods_settings_get(2, "x360_sky", &v), POP_OK);
    MOD_CHECK_EQ(v, 2);
    MOD_CHECK(!mods_settings_is_override("a.mod/x360_sky")); // the migration made no choice of the default
    MOD_CHECK(mods_settings_save());
    const std::string first = read_profile();
    MOD_CHECK(first.find("\"kit.settings/format\": 2") != std::string::npos);
    MOD_CHECK(first.find("\"other.mod/x\": 5") != std::string::npos);
    MOD_CHECK(first.find("a.mod/aa") == std::string::npos);
    // Again: the marker ends the legacy reading, the migration's version stops it; the file is unchanged.
    relaunch();
    declare_all(0);
    sky_migration();
    MOD_CHECK(mods_settings_save());
    MOD_CHECK(read_profile() == first);
    // After the conversion a choice equal to the default is a choice (no second legacy reading), and the inherited
    // AA follows a new default.
    MOD_CHECK_EQ(mods_settings_set(2, "volume", 100), POP_OK);
    relaunch();
    declare_all(1);
    MOD_CHECK_EQ(mods_settings_get(2, "aa", &v), POP_OK);
    MOD_CHECK_EQ(v, 1);
    MOD_CHECK_EQ(mods_settings_get(2, "sky", &v), POP_OK);
    MOD_CHECK_EQ(v, 1);
    MOD_CHECK(mods_settings_is_override("a.mod/volume"));
    MOD_CHECK_EQ(mods_settings_get(2, "volume", &v), POP_OK);
    MOD_CHECK_EQ(v, 100);
    mods_settings_reset();
}

// Last-known-good: a recovered Apply puts an inherited key back to inherited, not to a choice of its old value.
MOD_TEST_SUITE(settings_pending_recovery_keeps_ownership) {
    fresh();
    remove(mods_settings_path());
    relaunch();
    mods_settings_declare(2, "a.mod", "aa", "AA", POP_SETTING_INT, 0, 0, 2);
    mods_settings_declare(2, "a.mod", "look", "Look", POP_SETTING_INT, 0, 0, 2);
    MOD_CHECK_EQ(mods_settings_set(2, "look", 1), POP_OK);
    mods_cpp_settings_mark_pending({{"a.mod/aa", 0}, {"a.mod/look", 1}});
    MOD_CHECK_EQ(mods_settings_set(2, "aa", 2), POP_OK);   // the Apply that never proved itself
    MOD_CHECK_EQ(mods_settings_set(2, "look", 2), POP_OK);
    relaunch();
    MOD_CHECK_EQ(mods_settings_recover_pending(), 2);
    mods_settings_declare(2, "a.mod", "aa", "AA", POP_SETTING_INT, 0, 0, 2);
    mods_settings_declare(2, "a.mod", "look", "Look", POP_SETTING_INT, 0, 0, 2);
    int64_t v = -1;
    MOD_CHECK_EQ(mods_settings_get(2, "aa", &v), POP_OK);
    MOD_CHECK_EQ(v, 0);
    MOD_CHECK(!mods_settings_is_override("a.mod/aa"));
    MOD_CHECK_EQ(mods_settings_get(2, "look", &v), POP_OK);
    MOD_CHECK_EQ(v, 1);
    MOD_CHECK(mods_settings_is_override("a.mod/look"));
    mods_settings_reset();
}

MOD_TEST_SUITE(settings_host_display_survives_page_reinit_and_profile_reload) {
    fresh();
    mods_host_set_main_thread();
    mods_settings_rows_for_test((1u << DISPLAY_WINDOW) | (1u << DISPLAY_OVERLAY));
    mods_page_init();
    MOD_CHECK_EQ(mods_display_set(DISPLAY_WINDOW, 2), POP_OK);
    MOD_CHECK_EQ(mods_display_set(DISPLAY_OVERLAY, 1), POP_OK);
    std::string path = mods_settings_path();
    // Reconstructing the fallback page during a display restart must retain
    // both the user's values and the host's published overlay state.
    mods_page_init();
    MOD_CHECK_EQ(mods_display_value(DISPLAY_WINDOW), 2);
    MOD_CHECK_EQ(mods_display_value(DISPLAY_OVERLAY), 1);
    MOD_CHECK_EQ(mods_display_overlay(), 1);
    mods_settings_reset();
    MOD_CHECK(mods_settings_load(path.c_str()));
    mods_page_init();
    MOD_CHECK_EQ(mods_display_value(DISPLAY_WINDOW), 2);
    MOD_CHECK_EQ(mods_display_value(DISPLAY_OVERLAY), 1);
    MOD_CHECK_EQ(mods_display_overlay(), 1);
    mods_input_remove_all(MODS_OWNER_RUNTIME);
    mods_settings_rows_for_test((2u << DISPLAY_CONTROLS_BIT) - 1);
}

MOD_TEST_SUITE(settings_failed_save_keeps_previous_value) {
    fresh();
    mods_settings_declare(2, "a.mod", "volume", "Volume", POP_SETTING_INT, 5, 0, 10);
    MOD_CHECK_EQ(mods_settings_set(2, "volume", 7), POP_OK);
    const std::string path = mods_settings_path();
    // A file in place of the parent directory is an actual filesystem error,
    // independent of permissions (the test can also run as root).
    const std::string backup = std::string(PROFILE) + "-saved";
    MOD_CHECK_EQ(rename(PROFILE, backup.c_str()), 0);
    FILE *blocker = fopen(PROFILE, "wb");
    MOD_CHECK(blocker != nullptr);
    if (blocker)
        fclose(blocker);
    MOD_CHECK_EQ(mods_settings_set(2, "volume", 9), POP_E_STATE);
    int64_t value = 0;
    mods_settings_get(2, "volume", &value);
    MOD_CHECK_EQ(value, 7);
    MOD_CHECK_EQ(remove(PROFILE), 0);
    MOD_CHECK_EQ(rename(backup.c_str(), PROFILE), 0);
    mods_settings_reset();
    MOD_CHECK(mods_settings_load(path.c_str()));
    mods_settings_declare(2, "a.mod", "volume", "Volume", POP_SETTING_INT, 5, 0, 10);
    mods_settings_get(2, "volume", &value);
    MOD_CHECK_EQ(value, 7);
}

MOD_TEST_SUITE(settings_transaction_rolls_back) {
    fresh();
    mods_settings_declare(2, "a.mod", "keep", "Keep", POP_SETTING_INT, 1, 0, 9);
    MOD_CHECK_EQ(mods_settings_set(2, "keep", 5), POP_OK);
    MOD_CHECK(mods_settings_save());

    // A mod that declares settings, changes an existing one and then fails.
    mods_settings_txn_begin();
    mods_settings_declare(3, "b.mod", "temp", "Temp", POP_SETTING_INT, 0, 0, 9);
    MOD_CHECK_EQ(mods_settings_set(3, "temp", 7), POP_OK);
    MOD_CHECK_EQ(mods_settings_set(2, "keep", 9), POP_OK);
    mods_settings_txn_rollback();
    mods_settings_remove_all(3);

    int64_t v = 0;
    // Its own declaration is gone, and the value it changed is back.
    MOD_CHECK_EQ(mods_settings_get(3, "temp", &v), POP_E_NOTFOUND);
    MOD_CHECK_EQ(mods_settings_get(2, "keep", &v), POP_OK);
    MOD_CHECK_EQ(v, 5);
    // The persisted file is not left holding the rolled-back value either.
    MOD_CHECK(mods_settings_save());
    mods_settings_reset();
    MOD_CHECK(mods_settings_load(mods_settings_path()));
    mods_settings_declare(2, "a.mod", "keep", "Keep", POP_SETTING_INT, 1, 0, 9);
    MOD_CHECK_EQ(mods_settings_get(2, "keep", &v), POP_OK);
    MOD_CHECK_EQ(v, 5);
    MOD_CHECK(!mods_settings_entry_count() || mods_settings_entry_count() == 1);
}

MOD_TEST_SUITE(settings_entries_are_ordered_and_labelled) {
    fresh();
    mods_settings_declare(3, "b.mod", "second", "Second", POP_SETTING_INT, 1, 0, 9);
    mods_settings_declare(2, "a.mod", "first", "First", POP_SETTING_BOOL, 0, 0, 1);
    MOD_CHECK_EQ(mods_settings_entry_count(), 2u);

    uint32_t owner = 0;
    const char *mod_id = nullptr, *key = nullptr, *label = nullptr;
    int32_t kind = 0;
    int64_t value = 0, min = 0, max = 0;
    // Ordered by owner (load order), not by declaration order.
    MOD_CHECK(mods_settings_entry(0, &owner, &mod_id, &key, &label, &kind, &value, &min, &max));
    MOD_CHECK_EQ(owner, 2u);
    MOD_CHECK_STR(mod_id, "a.mod");
    MOD_CHECK_STR(key, "first");
    MOD_CHECK_STR(label, "First");
    MOD_CHECK_EQ(kind, POP_SETTING_BOOL);
    MOD_CHECK(mods_settings_entry(1, &owner, &mod_id, &key, &label, &kind, &value, &min, &max));
    MOD_CHECK_EQ(owner, 3u);
    MOD_CHECK(!mods_settings_entry(2, &owner, &mod_id, &key, &label, &kind, &value, &min, &max));
}

MOD_TEST_SUITE(controls_settings_defaults) {
    fresh();
    mods_controls_reset();
    mods_controls_set_names({"pad", "keys", "pad+keys"});
    mods_controls_init("keys");
    MOD_CHECK_EQ(mods_controls_value(CONTROLS_LAYOUT_ROW), 1); // "keys" is index 1
    MOD_CHECK(mods_controls_layout_name() == "keys");
    MOD_CHECK_EQ(mods_controls_value(CONTROLS_SIZE_ROW), 1);
    MOD_CHECK_EQ(mods_controls_value(CONTROLS_OPACITY_ROW), 100);
    MOD_CHECK_EQ(mods_controls_value(CONTROLS_HAPTICS_ROW), 1);
    MOD_CHECK_EQ(mods_controls_value(CONTROLS_PAD_WITH_CONTROLLER_ROW), 0);
    MOD_CHECK_EQ(mods_controls_value(CONTROLS_SNAP_ROW), 1);
    MOD_CHECK_EQ(mods_controls_hidden_groups(CONTROLS_FORM_TABLET), 0u);
    // Size clamps at its ends, like the old keypad size row.
    MOD_CHECK_EQ(mods_controls_set(CONTROLS_SIZE_ROW, 7), POP_OK);
    MOD_CHECK_EQ(mods_controls_value(CONTROLS_SIZE_ROW), 2);
    MOD_CHECK(mods_controls_line(CONTROLS_SIZE_ROW).find("Large") != std::string::npos);
    mods_controls_reset();
    mods_settings_reset();
}

// A profile that only ever knew the old host.keypad/* keys migrates into
// host.controls/* the first time mods_controls_init runs; the keypad keys
// stay on disk untouched (this only reads them through
// mods_settings_stored_value, never declares them).
MOD_TEST_SUITE(controls_settings_migration) {
    fresh();
    mods_controls_reset();
    FILE *f = fopen(mods_settings_path(), "wb");
    MOD_CHECK(f != nullptr);
    if (f) {
        MOD_CHECK(fputs("{\"host.keypad/left\": 0, \"host.keypad/right\": 1, "
                        "\"host.keypad/size\": 2}\n",
                        f) >= 0);
        MOD_CHECK_EQ(fclose(f), 0);
    }
    MOD_CHECK(mods_settings_load(mods_settings_path()));
    mods_controls_set_names({"pad", "keys", "pad+keys"});
    mods_controls_init("pad");
    MOD_CHECK_EQ(mods_controls_value(CONTROLS_LAYOUT_ROW), 1); // the index of "keys"
    MOD_CHECK(mods_controls_layout_name() == "keys");
    // The keypad's bits belong to whichever form first asks for them.
    MOD_CHECK_EQ(mods_controls_hidden_groups(CONTROLS_FORM_TABLET), 1u); // left 0, right 1
    MOD_CHECK_EQ(mods_controls_hidden_groups(CONTROLS_FORM_PHONE_PORTRAIT), 0u);
    MOD_CHECK_EQ(mods_controls_value(CONTROLS_SIZE_ROW), 2);
    // Nothing under host.controls/ was written by the migration itself.
    int64_t discard = 0;
    MOD_CHECK(!mods_settings_stored_value("host.controls/layout", &discard));
    // The keypad keys are untouched and still readable.
    int64_t left = -1;
    MOD_CHECK(mods_settings_stored_value("host.keypad/left", &left));
    MOD_CHECK_EQ(left, 0);
    mods_controls_reset();
    mods_settings_reset();
}

MOD_TEST_SUITE(controls_settings_hidden_choice) {
    fresh();
    mods_controls_reset();
    mods_controls_set_names({"pad", "keys", "pad+keys"});
    mods_controls_init("pad+keys"); // the last name, index 2
    MOD_CHECK_EQ(mods_controls_value(CONTROLS_LAYOUT_ROW), 2);
    // Nudging past the last name reaches the Hidden choice.
    MOD_CHECK_EQ(mods_controls_nudge(CONTROLS_LAYOUT_ROW, +1), POP_OK);
    MOD_CHECK_EQ(mods_controls_value(CONTROLS_LAYOUT_ROW), 3);
    MOD_CHECK(mods_controls_layout_name().empty());
    MOD_CHECK(mods_controls_line(CONTROLS_LAYOUT_ROW).find("hidden") != std::string::npos);
    // It turns over, like the display rows.
    MOD_CHECK_EQ(mods_controls_nudge(CONTROLS_LAYOUT_ROW, +1), POP_OK);
    MOD_CHECK_EQ(mods_controls_value(CONTROLS_LAYOUT_ROW), 0);
    mods_controls_reset();
    mods_settings_reset();
}

// The editor saved a layout under a new name: the row has to reach it, so
// mods_controls_refresh_names moves the declared maximum, which
// mods_settings_declare alone cannot do once the key exists.
MOD_TEST_SUITE(controls_settings_refresh_names) {
    fresh();
    mods_controls_reset();
    mods_controls_set_names({"pad", "keys", "pad+keys"});
    mods_controls_init("pad");
    // Hidden is index 3; there is no index 4 yet.
    MOD_CHECK_EQ(mods_controls_set(CONTROLS_LAYOUT_ROW, 4), POP_OK);
    MOD_CHECK_EQ(mods_controls_value(CONTROLS_LAYOUT_ROW), 3);

    mods_controls_set_names({"pad", "keys", "pad+keys", "mine"});
    mods_controls_refresh_names();
    MOD_CHECK_EQ(mods_controls_set(CONTROLS_LAYOUT_ROW, 3), POP_OK);
    MOD_CHECK_EQ(mods_controls_value(CONTROLS_LAYOUT_ROW), 3);
    MOD_CHECK(mods_controls_layout_name() == "mine");
    // The value survives the round trip through the settings store.
    int64_t stored = 0;
    MOD_CHECK_EQ(mods_settings_get(MODS_OWNER_RUNTIME, "layout", &stored), POP_OK);
    MOD_CHECK_EQ((int)stored, 3);

    // A shorter list clamps the row back into range.
    mods_controls_set_names({"pad"});
    mods_controls_refresh_names();
    MOD_CHECK_EQ(mods_controls_value(CONTROLS_LAYOUT_ROW), 1);
    mods_controls_reset();
    mods_settings_reset();
}

// The hidden-group bits are per form factor: portrait "keys" is one board
// where landscape is two halves, so hiding the portrait board must not come
// back as a hidden left half after a rotation. The three lanes share one
// stored value and survive a reload of the profile.
MOD_TEST_SUITE(controls_settings_hidden_groups_per_form) {
    fresh();
    mods_controls_reset();
    mods_controls_set_names({"pad", "keys", "pad+keys"});
    mods_controls_init("keys");
    mods_controls_set_hidden_groups(CONTROLS_FORM_PHONE_PORTRAIT, 1u);
    mods_controls_set_hidden_groups(CONTROLS_FORM_PHONE_LANDSCAPE, 2u);
    MOD_CHECK_EQ(mods_controls_hidden_groups(CONTROLS_FORM_PHONE_PORTRAIT), 1u);
    MOD_CHECK_EQ(mods_controls_hidden_groups(CONTROLS_FORM_PHONE_LANDSCAPE), 2u);
    MOD_CHECK_EQ(mods_controls_hidden_groups(CONTROLS_FORM_TABLET), 0u); // the rotation home
    // The press itself writes nothing: the host flushes once per pump.
    int64_t stored = 0;
    MOD_CHECK(!mods_settings_stored_value("host.controls/hidden", &stored));
    mods_controls_flush();
    MOD_CHECK(mods_settings_stored_value("host.controls/hidden", &stored));
    mods_controls_flush(); // nothing left to write

    // A relaunch reads all three lanes back.
    mods_controls_reset();
    mods_settings_reset();
    mods_overlay_set_profile_dir(PROFILE);
    MOD_CHECK(mods_settings_load(mods_settings_path()));
    mods_controls_set_names({"pad", "keys", "pad+keys"});
    mods_controls_init("keys");
    MOD_CHECK_EQ(mods_controls_hidden_groups(CONTROLS_FORM_PHONE_PORTRAIT), 1u);
    MOD_CHECK_EQ(mods_controls_hidden_groups(CONTROLS_FORM_PHONE_LANDSCAPE), 2u);
    MOD_CHECK_EQ(mods_controls_hidden_groups(CONTROLS_FORM_TABLET), 0u);
    mods_controls_reset();
    mods_settings_reset();
}

// A profile from the build that kept one set of bits for every form: they
// belong to the form the player hid the group in, which is the first one to
// ask, and the other forms start clean.
MOD_TEST_SUITE(controls_settings_hidden_groups_legacy_value) {
    fresh();
    mods_controls_reset();
    FILE *f = fopen(mods_settings_path(), "wb");
    MOD_CHECK(f != nullptr);
    if (f) {
        MOD_CHECK(fputs("{\"host.controls/layout\": 1, \"host.controls/hidden\": 1}\n", f) >= 0);
        MOD_CHECK_EQ(fclose(f), 0);
    }
    MOD_CHECK(mods_settings_load(mods_settings_path()));
    mods_controls_set_names({"pad", "keys", "pad+keys"});
    mods_controls_init("keys");
    MOD_CHECK_EQ(mods_controls_hidden_groups(CONTROLS_FORM_PHONE_PORTRAIT), 1u);
    MOD_CHECK_EQ(mods_controls_hidden_groups(CONTROLS_FORM_TABLET), 0u);
    MOD_CHECK_EQ(mods_controls_hidden_groups(CONTROLS_FORM_PHONE_LANDSCAPE), 0u);
    mods_controls_reset();
    mods_settings_reset();
}

// A layout index the name list no longer reaches (the player deleted their
// copy) falls back to the game's default_layout, not to the Hidden slot,
// which would have left a blank screen.
MOD_TEST_SUITE(controls_settings_out_of_range_layout_falls_back) {
    fresh();
    mods_controls_reset();
    FILE *f = fopen(mods_settings_path(), "wb");
    MOD_CHECK(f != nullptr);
    if (f) {
        MOD_CHECK(fputs("{\"host.controls/layout\": 7}\n", f) >= 0);
        MOD_CHECK_EQ(fclose(f), 0);
    }
    MOD_CHECK(mods_settings_load(mods_settings_path()));
    mods_controls_set_names({"pad", "keys", "pad+keys"});
    mods_controls_init("pad+keys");
    MOD_CHECK_EQ(mods_controls_value(CONTROLS_LAYOUT_ROW), 2);
    MOD_CHECK(mods_controls_layout_name() == "pad+keys");
    mods_controls_reset();
    mods_settings_reset();
}

// The host sets the editing flag; the F10 page's gates read it.
MOD_TEST_SUITE(controls_settings_editing_flag) {
    mods_controls_reset();
    MOD_CHECK(!mods_controls_editing());
    mods_controls_set_editing(true);
    MOD_CHECK(mods_controls_editing());
    mods_controls_set_editing(false);
    MOD_CHECK(!mods_controls_editing());
}

MOD_TEST_SUITE(controls_settings_edit_request) {
    fresh();
    mods_controls_reset();
    mods_controls_set_names({"pad", "keys", "pad+keys"});
    mods_controls_init("keys");
    MOD_CHECK(!mods_controls_take_edit_request());
    MOD_CHECK_EQ(mods_controls_nudge(CONTROLS_EDIT_ROW, +1), POP_OK);
    MOD_CHECK(mods_controls_take_edit_request());
    MOD_CHECK(!mods_controls_take_edit_request()); // cleared by the take
    mods_controls_reset();
    mods_settings_reset();
}

// Settings suites run before any suite loads a symbol map. Exercise the
// fallback page itself, including its row metadata used for navigation.
MOD_TEST_SUITE(settings_page_without_symbols_shows_host_controls) {
    fresh();
    MOD_CHECK_EQ(mods_symbols_count(), 0u);
    mods_host_set_main_thread();
    mods_display_reset();
    mods_page_init();
    MOD_CHECK_EQ(mods_page_open(nullptr), POP_OK);
    const DisplayRow hidden[] = {DISPLAY_RENDERING,    DISPLAY_UI_SCALE, DISPLAY_WIDE,
                                 DISPLAY_CLASSIC_MODE, DISPLAY_TEXTURES, DISPLAY_FILTERING};
    const DisplayRow visible[] = {DISPLAY_WINDOW, DISPLAY_FPS, DISPLAY_OVERLAY};
    auto page_has_row = [](DisplayRow row) {
        const std::string label = mods_display_line(row);
        for (uint32_t i = 0; i < mods_page_line_count(); ++i)
            if (label == mods_page_line(i))
                return true;
        return false;
    };
    for (DisplayRow row : hidden) {
        MOD_CHECK(!mods_display_row_applies(row));
        MOD_CHECK(!page_has_row(row));
    }
    for (DisplayRow row : visible) {
        MOD_CHECK(mods_display_row_applies(row));
        MOD_CHECK(page_has_row(row));
    }
    MOD_CHECK_EQ(mods_page_line_count(), 3u + CONTROLS_ROW_COUNT);
    // The first visible row still targets window mode after filtering.
    mods_input_key(0xcd, 0, true);
    mods_input_key(0xcd, 0, false);
    MOD_CHECK_EQ(mods_display_value(DISPLAY_WINDOW), 1);
    mods_page_close();
    mods_input_remove_all(MODS_OWNER_RUNTIME);
    mods_display_reset();
    mods_controls_reset();
    mods_settings_reset();
}

// Like the stub, this port can have no usable symbol table. The existing
// layout test seam makes that failure deterministic without touching a map.
MOD_TEST_SUITE(settings_load_before_missing_symbols) {
    // The load and the teardown below mutate the hook registry. Without the
    // baton those mutations are queued for a checkpoint this binary never
    // runs, and every later suite's installs wait behind them.
    sched_set_guest_thread(true);
    mods_host_set_main_thread();
    MOD_CHECK(mods_test_reset_loader());
    fresh();
    mods_display_reset();
    MOD_CHECK_EQ(mods_symbols_count(), 0u);
    FILE *f = fopen(mods_settings_path(), "wb");
    MOD_CHECK(f != nullptr);
    if (f) {
        MOD_CHECK(fputs("{\"host.display/window\": 2}\n", f) >= 0);
        MOD_CHECK_EQ(fclose(f), 0);
    }
    host_layout_set_exe_path_for_test("/nowhere/at/all/exe");
    MOD_CHECK(!mods_load_all());
    MOD_CHECK_EQ(mods_symbols_count(), 0u);
    mods_display_init();
    int64_t v = -1;
    MOD_CHECK_EQ(mods_settings_get(MODS_OWNER_RUNTIME, "window", &v), POP_OK);
    MOD_CHECK_EQ(v, 2);
    MOD_CHECK_EQ(mods_display_value(DISPLAY_WINDOW), 2);
    MOD_CHECK(mods_test_reset_loader());
    host_layout_set_exe_path_for_test(nullptr);
    win32_set_file_ops(nullptr, nullptr);
    mods_display_reset();
    mods_settings_reset();
}

// Remaster run 29 (NFSMW D-163, the PC look retired): a saved value the range no longer offers gives the default,
// whether the file is read before the declare or after it (an existing entry keeps its value), and the next save
// drops it.
MOD_TEST_SUITE(settings_retired_value_takes_default) {
    fresh();
    mods_settings_declare(2, "a.mod", "look", "Look", POP_SETTING_INT, 0, 0, 2);
    MOD_CHECK_EQ(mods_settings_set(2, "look", 2), POP_OK);
    mods_settings_reset();
    MOD_CHECK(mods_settings_load(mods_settings_path()));
    mods_settings_declare(2, "a.mod", "look", "Look", POP_SETTING_INT, 0, 0, 1);
    int64_t v = -1;
    MOD_CHECK_EQ(mods_settings_get(2, "look", &v), POP_OK);
    MOD_CHECK_EQ(v, 0);
    MOD_CHECK(mods_settings_load(mods_settings_path())); // the file still says 2: read again over the entry
    MOD_CHECK_EQ(mods_settings_get(2, "look", &v), POP_OK);
    MOD_CHECK_EQ(v, 0);
    MOD_CHECK(mods_settings_save());
    mods_settings_reset();
    MOD_CHECK(mods_settings_load(mods_settings_path()));
    mods_settings_declare(2, "a.mod", "look", "Look", POP_SETTING_INT, 0, 0, 2); // the old range again
    MOD_CHECK_EQ(mods_settings_get(2, "look", &v), POP_OK);
    MOD_CHECK_EQ(v, 0); // the save dropped the 2
}
