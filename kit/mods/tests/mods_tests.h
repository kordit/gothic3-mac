#pragma once
#include <stdio.h>
#include <string.h>
#include "../../platform/os.h"

struct ModTestSuite {
    const char *name;
    void (*fn)();
    ModTestSuite *next;
};
void mod_test_register(ModTestSuite *s);
ModTestSuite *mod_test_suites();
void mod_test_fail(const char *what, const char *file, int line);
void mod_test_pass();
int mod_test_failures();

#define MOD_TEST_SUITE(name_)                                                                      \
    static void name_##_run();                                                                     \
    static ModTestSuite name_##_suite = {#name_, name_##_run, nullptr};                            \
    static struct name_##_reg_t {                                                                  \
        name_##_reg_t() {                                                                          \
            mod_test_register(&name_##_suite);                                                     \
        }                                                                                          \
    } name_##_reg_instance;                                                                        \
    static void name_##_run()

// Where the suites find their files, from any working folder.
//
// Build artefacts (plugin fixtures, scratch trees, symbols): RECOMP_BUILD_ROOT
// when set (CTest sets it), else the build root this binary sits in - it is
// built as <build root>/recomp/mods_tests - else "build" under the working
// folder, as before. Source fixtures (Lua scripts, baseline JSON): the kit's
// source folder, RECOMP_KIT_DIR from the generated game_config.h. Before run 31's
// fix the first was cwd-relative unless CTest ran it and the second always
// was, so no single working folder satisfied both.
#include <stdlib.h>
#include <string>
#include "game_config.h"
inline std::string mods_test_build_root() {
    const char *root = recomp_env("BUILD_ROOT");
    if (root && *root)
        return root;
    static std::string from_exe = [] {
        char exe[4096];
        if (os_exe_path(exe, sizeof exe) != 0)
            return std::string();
        std::string dir = exe;
        size_t slash = dir.find_last_of("/\\");
        if (slash == std::string::npos)
            return std::string();
        dir.resize(slash); // <build root>/recomp
        slash = dir.find_last_of("/\\");
        if (slash == std::string::npos || dir.compare(slash + 1, std::string::npos, "recomp") != 0)
            return std::string();
        dir.resize(slash);
        return dir;
    }();
    return from_exe.empty() ? "build" : from_exe;
}
inline std::string mods_test_build_path(const char *rel) {
    return mods_test_build_root() + "/" + rel;
}
inline std::string mods_test_source_path(const char *rel) {
    return std::string(RECOMP_KIT_DIR) + "/" + rel;
}

// Marks the running suite as skipped, with the reason the runner prints, and
// lets the suite return early. A suite that has already failed a check stays
// FAILED. Use it for what this binary cannot have, never for a wrong answer.
void mod_test_skip(const char *reason);

// Some suites were written against Populous: The Beginning's image, the kit's
// first game, and use its fixed addresses: main_loop_inner 0x004ec6f0 (the
// turn routine, on_turn), load_objs 0x0040c690 (on_level_load), its menu,
// settings and animation routines and its levels\\levl2001.dat. (The kit's
// games/stub has no image at all.) On any other game's image those addresses
// are not functions; looking one up gives -1, which run 31 of the NFSMW port
// found indexed straight into the hook table (a SIGBUS that ended the run).
// Such a suite calls MOD_TEST_NEEDS_POPULOUS_IMAGE() first and is reported
// "skipped (needs the Populous image ...)" elsewhere. Game-independent suites
// pick their targets at run time instead (hook_snapshot_tests.cpp,
// overlay_tests.cpp's game_file()).
#define MOD_TEST_POPULOUS_IMAGE_REASON                                                             \
    "needs the Populous image: fixed addresses main_loop_inner 0x004ec6f0, load_objs 0x0040c690"
bool mod_test_have_populous_image();
#define MOD_TEST_NEEDS_POPULOUS_IMAGE()                                                            \
    do {                                                                                           \
        if (!mod_test_have_populous_image()) {                                                     \
            mod_test_skip(MOD_TEST_POPULOUS_IMAGE_REASON);                                         \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define MOD_CHECK(x)                                                                               \
    do {                                                                                           \
        if (x)                                                                                     \
            mod_test_pass();                                                                       \
        else                                                                                       \
            mod_test_fail(#x, __FILE__, __LINE__);                                                 \
    } while (0)

#define MOD_CHECK_EQ(a, b)                                                                         \
    do {                                                                                           \
        long long va = (long long)(a), vb = (long long)(b);                                        \
        if (va == vb)                                                                              \
            mod_test_pass();                                                                       \
        else {                                                                                     \
            char m[256];                                                                           \
            snprintf(m, sizeof m, "%s == %s (%lld vs %lld)", #a, #b, va, vb);                      \
            mod_test_fail(m, __FILE__, __LINE__);                                                  \
        }                                                                                          \
    } while (0)

#define MOD_CHECK_STR(a, b)                                                                        \
    do {                                                                                           \
        const char *sa = (a);                                                                      \
        const char *sb = (b);                                                                      \
        if (sa && sb && strcmp(sa, sb) == 0)                                                       \
            mod_test_pass();                                                                       \
        else {                                                                                     \
            char m[512];                                                                           \
            snprintf(m, sizeof m, "%s == %s (\"%s\" vs \"%s\")", #a, #b, sa ? sa : "(null)",       \
                     sb ? sb : "(null)");                                                          \
            mod_test_fail(m, __FILE__, __LINE__);                                                  \
        }                                                                                          \
    } while (0)

// A scratch directory of this suite's own, created empty. Rooted at
// RECOMP_TEST_DIR (default build/recomp/mods-test), so two suites - or two tasks
// in two worktrees - never share a path and none reads another's leftovers.
const char *mod_test_dir(const char *suite);
