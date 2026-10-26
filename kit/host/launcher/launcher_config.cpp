// launcher_config.cpp - the launcher's Spec from this build's game.toml.
#include "launcher.h"
#include "launcher_stage.h"
#include "launcher_steam.h"

#include "game_config.h"

#include <sstream>

namespace launcher {
namespace {
std::vector<std::string> strings(const char *const *list) {
    std::vector<std::string> out;
    for (; *list; ++list)
        out.push_back(*list);
    return out;
}
} // namespace

const Spec &spec_from_config() {
    static const Spec spec = [] {
        static const char *const required[] = RECOMP_REQUIRED_DIRS;
        static const char *const exclude[] = RECOMP_BUNDLE_EXCLUDE;
        static const char *const names[] = RECOMP_LAUNCHER_INSTALL_NAMES;
        static const char *const gog[] = RECOMP_LAUNCHER_GOG_IDS;
        static const char *const steam[] = RECOMP_LAUNCHER_STEAM_IDS;
        Spec s;
        s.title = RECOMP_LAUNCHER_TITLE;
        s.store_url = RECOMP_LAUNCHER_STORE_URL;
        s.executable = RECOMP_EXECUTABLE;
        s.sha256 = RECOMP_EXE_SHA256;
        s.required_dirs = strings(required);
        s.exclude = strings(exclude);
        s.install_names = strings(names);
        s.gog_ids = strings(gog);
        s.steam_ids = strings(steam);
#ifdef RECOMP_LAUNCHER_NATIVE_DLLS
        static const char *const native[] = RECOMP_LAUNCHER_NATIVE_DLLS;
        s.native_dlls = strings(native);
#endif
        s.min_free_bytes = uint64_t(RECOMP_LAUNCHER_MIN_FREE_MB) * 1024 * 1024;
#ifdef RECOMP_LAUNCHER_SEFARIS_DIR
        s.mod_root = RECOMP_LAUNCHER_SEFARIS_DIR;
#endif
#ifdef RECOMP_LAUNCHER_LANGUAGE
        static const char *const display[] = RECOMP_LAUNCHER_FONT_DISPLAY;
        static const char *const body[] = RECOMP_LAUNCHER_FONT_BODY;
        static const char *const strong[] = RECOMP_LAUNCHER_FONT_STRONG;
        s.language = RECOMP_LAUNCHER_LANGUAGE;
        s.banner = RECOMP_LAUNCHER_BANNER;
        s.subtitle = RECOMP_LAUNCHER_SUBTITLE;
        s.font_display = strings(display);
        s.font_body = strings(body);
        s.font_strong = strings(strong);
        s.backdrop_video = RECOMP_LAUNCHER_BACKDROP_VIDEO;
        s.backdrop_seconds = RECOMP_LAUNCHER_BACKDROP_SECONDS;
#endif
        s.steam = steam_spec_from_config();
#ifdef RECOMP_LAUNCHER_MODS_URL
        s.mods_url = RECOMP_LAUNCHER_MODS_URL;
#endif
#ifdef RECOMP_LAUNCHER_DEFAULT_MODS
        {
            std::istringstream lines(RECOMP_LAUNCHER_DEFAULT_MODS);
            for (std::string line; std::getline(lines, line);)
                if (!line.empty())
                    s.default_mods.push_back(line);
        }
#endif
#ifdef RECOMP_LAUNCHER_PROFILE_SEED
        {
            std::istringstream lines(RECOMP_LAUNCHER_PROFILE_SEED);
            for (std::string line; std::getline(lines, line);)
                if (!line.empty())
                    s.profile_seed.push_back(line);
        }
#endif
#ifdef RECOMP_RELEASES_REPO
        s.releases_repo = RECOMP_RELEASES_REPO;
        s.update_asset = std::string(RECOMP_APP_NAME) + "-macOS.zip";
#endif
        s.stage = stage_modules_from_config();
        return s;
    }();
    return spec;
}

const SteamSpec &steam_spec_from_config() {
    static const SteamSpec spec = [] {
        SteamSpec s;
#ifdef RECOMP_LAUNCHER_STEAM_TOOL
        struct Raw {
            unsigned id;
            const char *manifest, *branch, *files;
            unsigned mb;
            const char *label, *ini_on, *ini_off;
        };
        auto lines_of = [](const char *text) {
            std::vector<std::string> out;
            std::istringstream lines(text ? text : "");
            for (std::string line; std::getline(lines, line);)
                if (!line.empty())
                    out.push_back(line);
            return out;
        };
        static const Raw depots[] = RECOMP_LAUNCHER_STEAM_DEPOTS;
        s.tool = RECOMP_LAUNCHER_STEAM_TOOL;
        s.app = RECOMP_LAUNCHER_STEAM_APP;
        for (int i = 0; i < RECOMP_LAUNCHER_STEAM_DEPOT_COUNT; ++i) {
            SteamDepot d;
            d.id = depots[i].id;
            d.manifest = depots[i].manifest;
            d.branch = depots[i].branch;
            std::istringstream lines(depots[i].files);
            for (std::string line; std::getline(lines, line);)
                if (!line.empty())
                    d.files.push_back(line);
            d.mb = depots[i].mb;
            d.label = depots[i].label;
            d.ini_on = lines_of(depots[i].ini_on);
            d.ini_off = lines_of(depots[i].ini_off);
            s.depots.push_back(d);
        }
#ifdef RECOMP_LAUNCHER_STEAM_INI
        s.ini = lines_of(RECOMP_LAUNCHER_STEAM_INI);
#endif
#endif
        return s;
    }();
    return spec;
}

const std::vector<StageModule> &stage_modules_from_config() {
    static const std::vector<StageModule> modules = [] {
        std::vector<StageModule> v;
#ifdef RECOMP_AUX_STAGE_COUNT
        struct Raw {
            const char *name, *dest, *from, *from_sha256, *prepare;
            unsigned base;
            const char *sha256;
        };
        static const Raw raw[] = RECOMP_AUX_STAGE;
        for (int i = 0; i < RECOMP_AUX_STAGE_COUNT; ++i)
            v.push_back({raw[i].name, raw[i].dest, raw[i].from, raw[i].from_sha256, raw[i].prepare,
                         raw[i].base, raw[i].sha256});
#endif
        return v;
    }();
    return modules;
}

} // namespace launcher
