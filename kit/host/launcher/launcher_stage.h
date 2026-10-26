// launcher_stage.h - the modules the loader maps at their own base, made from
// the player's files on the player's machine: each one the install's DLL
// rebased by its own .reloc table (or first taken out of a cabinet), checked
// against the SHA-256 game.toml pins for the result (Gothic 3, D-037; the
// same steps as the game's tools/stage.py). Portable: no SDL, no UI.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace launcher {

struct StageModule {
    std::string name;        // the DLL's name
    std::string dest;        // where the loader reads it, relative to the executable's folder
    std::string from;        // the install's file it is made from, '/' separated
    std::string from_sha256; // that file's pin
    std::string prepare;     // "" or "cab:<member>": the member of a cabinet
    uint32_t base = 0;       // the base it is rebased to
    std::string sha256;      // the result's pin
};
const std::vector<StageModule> &stage_modules_from_config();

// Rebases a PE32 image held as file bytes to `base` by its base relocations
// (HIGHLOW only, like tools/stage.py) and rewrites its ImageBase. false with
// *error when the image is not one or a fixup lies outside its file.
bool rebase_pe(std::vector<uint8_t> *file, uint32_t base, std::string *error);

// True when every module at root/dest hashes to its pin.
bool modules_staged(const std::string &root, const std::vector<StageModule> &modules);

struct StageOutcome {
    bool ok = false;
    std::string error; // names the file that is wrong or missing
    int made = 0, kept = 0;
};
// Makes each module whose root/dest does not already hash to its pin, from
// root/from: the source must hash to from_sha256 (a file already staged
// there in place hashes to sha256 and is kept). Each result is written as
// <dest>.part and renamed. `progress` hears each module as it starts.
StageOutcome stage_modules(const std::string &root, const std::vector<StageModule> &modules,
                           const std::function<void(size_t done, size_t total,
                                                    const std::string &name)> &progress = nullptr);

// Edits of the game's own .ini files after a download ([launcher.steam] ini and a depot's ini_on and
// ini_off), each "file|section|key=value" (sets the key in [section], added at the section's end when
// missing) or "file||+line" / "file||-line" (the line present or absent). The file keeps its bytes,
// its line endings and its encoding everywhere else. false with *error on the first one that fails.
bool apply_ini_edits(const std::string &root, const std::vector<std::string> &edits,
                     std::string *error);

} // namespace launcher
