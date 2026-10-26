// launcher_tests.cpp - finding, checking and importing a game, over fake
// installs built in a scratch folder. No window, no game.
#include "../launcher/launcher.h"
#include "../launcher/launcher_stage.h"
#include "../launcher/launcher_steam.h"
#include "../launcher/launcher_text.h"
#include "../launcher/launcher_ui.h"

#include "../../platform/os.h"
#include "../../third_party/miniz/miniz.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>
#endif

static int g_failures = 0, g_checks = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(c)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

using namespace launcher;

namespace {

std::string g_scratch;

void write_file(const std::string &path, const std::string &text) {
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/' && i)
            os_mkdir(path.substr(0, i).c_str());
    }
    FILE *f = fopen(path.c_str(), "wb");
    if (f) {
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }
}

std::string read_file(const std::string &path) {
    std::string out;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        out.append(buf, n);
    fclose(f);
    return out;
}

bool exists(const std::string &path) {
    OsStat st{};
    return os_stat(path.c_str(), &st) == 0;
}

// A fresh folder under the scratch directory.
std::string fresh(const char *name) {
    const std::string dir = g_scratch + "/" + name;
    remove_tree(dir);
    os_mkdir(dir.c_str());
    return dir;
}

const std::string kExe = "GAME.EXE";
const std::string kExeBytes = "MZ fake game executable\n";

Spec spec() {
    Spec s;
    s.title = "Test Game";
    s.executable = kExe;
    s.sha256 = sha256_file(g_scratch + "/exe-bytes");
    s.required_dirs = {"data", "levels"};
    s.exclude = {"__redist", "*.dll", "Setup?.txt"};
    s.install_names = {"Test Game"};
    return s;
}

// A plausible install: the executable, required folders, clutter to skip.
void make_install(const std::string &root, const std::string &exe_name = "game.exe") {
    write_file(root + "/" + exe_name, kExeBytes);
    write_file(root + "/DATA/sprites.bin", std::string(10000, 's'));
    write_file(root + "/levels/one.lvl", "level one");
    write_file(root + "/levels/deep/two.lvl", "level two");
    write_file(root + "/__redist/vcredist.exe", "skip me");
    write_file(root + "/binkw32.dll", "skip me too");
    write_file(root + "/Setup1.txt", "skip");
    write_file(root + "/readme.txt", "keep");
}

void make_zip(const std::string &zip_path, const std::string &prefix,
              const std::vector<std::pair<std::string, std::string>> &files) {
    mz_zip_archive zip{};
    CHECK(mz_zip_writer_init_file(&zip, zip_path.c_str(), 0));
    for (const auto &f : files) {
        const std::string name = prefix + f.first;
        CHECK(mz_zip_writer_add_mem(&zip, name.c_str(), f.second.data(), f.second.size(),
                                    MZ_BEST_SPEED));
    }
    CHECK(mz_zip_writer_finalize_archive(&zip));
    mz_zip_writer_end(&zip);
}

void test_sha256() {
    write_file(g_scratch + "/abc", "abc");
    CHECK(sha256_file(g_scratch + "/abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    write_file(g_scratch + "/empty", "");
    CHECK(sha256_file(g_scratch + "/empty") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    // Longer than one block and than the 1 MiB read buffer.
    write_file(g_scratch + "/long", std::string(1500000, 'a'));
    CHECK(sha256_file(g_scratch + "/long").size() == 64);
    CHECK(sha256_file(g_scratch + "/no-such-file").empty());
}

void test_wildcards() {
    CHECK(wildcard_match("*.dll", "BINKW32.DLL"));
    CHECK(wildcard_match("Setup?.txt", "setup1.TXT"));
    CHECK(!wildcard_match("Setup?.txt", "Setup10.txt"));
    CHECK(wildcard_match("[a-c]x", "Bx"));
    CHECK(!wildcard_match("[!a-c]x", "bx"));
    CHECK(wildcard_match("*", ""));
    CHECK(!wildcard_match("app", "apps"));
    const std::vector<std::string> ex = {"__redist", "*.dll"};
    CHECK(excluded("__redist/vc/x.exe", ex));
    CHECK(excluded("data/sub/x.dll", ex));
    CHECK(excluded("data/__redist", ex));        // a file name matches too
    CHECK(!excluded("data/__redist/x.exe", ex)); // a folder below the top does not
    CHECK(!excluded("data/x.dat", ex));
}

void test_find_and_check() {
    const Spec s = spec();
    const std::string top = fresh("find");
    CHECK(find_root(s, top).empty());
    make_install(top + "/GOG Games/Test Game/app");
    const std::string root = top + "/GOG Games/Test Game/app";
    CHECK(find_root(s, top + "/GOG Games") == root); // two levels down
    CHECK(find_root(s, top).empty());                // three is too deep
    CHECK(find_root(s, root + "/game.exe") == root); // the executable itself
    CHECK(find_root(s, root + "/readme.txt").empty());
    CHECK(find_name(root, "Levels") == "levels");

    Status st = check(s, root);
    CHECK(st.state == State::Ready);
    CHECK(st.exe == root + "/game.exe");
    CHECK(st.digest == s.sha256);
    CHECK(check(s, top).state == State::NotFound);

    // A missing required folder, then a wrong executable.
    remove_tree(root + "/levels");
    st = check(s, root);
    CHECK(st.state == State::Incomplete);
    CHECK(st.missing.size() == 1 && st.missing[0] == "levels");
    write_file(root + "/game.exe", "another build");
    CHECK(check(s, root).state == State::WrongVersion);

    // The cache spares a second hash; a changed file invalidates it.
    const std::string cache = top + "/profile/verified.txt";
    make_install(top + "/cached");
    CHECK(check(s, top + "/cached", cache).state == State::Ready);
    const std::string text = read_file(cache);
    CHECK(text.find(s.sha256) != std::string::npos);
    std::string forged = text;
    forged.replace(forged.find(s.sha256), s.sha256.size(), std::string(64, '0'));
    write_file(cache, forged);
    CHECK(check(s, top + "/cached", cache).state == State::WrongVersion); // trusts the cache
    write_file(top + "/cached/game.exe", kExeBytes + "x");                // size changes
    CHECK(check(s, top + "/cached", cache).state == State::WrongVersion);
    write_file(top + "/cached/game.exe", kExeBytes);
    os_set_mtime((top + "/cached/game.exe").c_str(), 12345);
    CHECK(check(s, top + "/cached", cache).state == State::Ready); // rehashed
    // One cache line per executable.
    const std::string after = read_file(cache);
    size_t lines = 0;
    for (char c : after)
        lines += c == '\n';
    CHECK(lines == 1);
}

void test_folder_import() {
    const Spec s = spec();
    const std::string src = fresh("src");
    make_install(src + "/nested/Test Game");
    const std::string dest = fresh("dest") + "/game";
    auto source = folder_source(src);
    std::vector<Progress> seen;
    ImportOutcome out = import_game(s, *source, dest, [&](const Progress &p) {
        seen.push_back(p);
        return true;
    });
    CHECK(out.result == ImportResult::Done);
    if (out.result != ImportResult::Done)
        fprintf(stderr, "  import: %s: %s\n", import_result_name(out.result), out.error.c_str());
    CHECK(out.files_copied == 5); // game.exe, sprites, two levels, readme
    CHECK(exists(dest + "/game.exe"));
    CHECK(exists(dest + "/DATA/sprites.bin"));
    CHECK(exists(dest + "/levels/deep/two.lvl"));
    CHECK(!exists(dest + "/__redist"));
    CHECK(!exists(dest + "/binkw32.dll"));
    CHECK(!exists(dest + "/Setup1.txt"));
    CHECK(read_file(dest + "/.stamp") == s.sha256 + "\n");
    CHECK(!seen.empty() && seen.back().files_done == 5 &&
          seen.back().bytes_done == seen.back().bytes_total);
    CHECK(check(s, dest).state == State::Ready);

    // Again: everything is kept, nothing is copied.
    out = import_game(s, *source, dest);
    CHECK(out.result == ImportResult::Done);
    CHECK(out.files_copied == 0 && out.files_skipped == 5);

    // A changed source file is copied again.
    write_file(src + "/nested/Test Game/levels/one.lvl", "level one, patched");
    out = import_game(s, *source, dest);
    CHECK(out.result == ImportResult::Done && out.files_copied == 1);
    CHECK(read_file(dest + "/levels/one.lvl") == "level one, patched");
}

void test_cancel_and_resume() {
    const Spec s = spec();
    const std::string src = fresh("resume-src");
    make_install(src);
    for (int i = 0; i < 20; ++i)
        write_file(src + "/data/file" + std::to_string(i) + ".bin",
                   std::string(1000, char('a' + i)));
    const std::string dest = fresh("resume-dest") + "/game";
    auto source = folder_source(src);
    int calls = 0;
    ImportOutcome out = import_game(s, *source, dest, [&](const Progress &p) {
        return ++calls < 8 || p.files_done == p.files_total;
    });
    CHECK(out.result == ImportResult::Cancelled);
    CHECK(!exists(dest + "/.stamp"));
    CHECK(check(s, dest).state != State::Ready || !exists(dest + "/.stamp"));
    // No half-written files are left under their real names.
    const uint64_t first = out.files_copied;
    CHECK(first > 0 && first < 25);
    out = import_game(s, *source, dest);
    CHECK(out.result == ImportResult::Done);
    CHECK(out.files_skipped >= first);
    CHECK(out.files_copied + out.files_skipped == 25);
    CHECK(exists(dest + "/.stamp"));
}

void test_move_import() {
    const Spec s = spec();
    const std::string src = fresh("move-src");
    make_install(src + "/copied");
    const std::string dest = fresh("move-dest") + "/game";
    auto source = folder_source(src);
    ImportOutcome out = import_game(s, *source, dest, nullptr, true);
    CHECK(out.result == ImportResult::Done);
    CHECK(out.files_copied == 5);
    CHECK(exists(dest + "/levels/deep/two.lvl"));
    CHECK(!exists(src + "/copied/levels/deep/two.lvl")); // moved, not copied
    CHECK(exists(src + "/copied/binkw32.dll"));          // excluded files stay
    CHECK(check(s, dest).state == State::Ready);
}

void test_failures() {
    Spec s = spec();
    // No executable at all.
    const std::string empty = fresh("empty-src");
    write_file(empty + "/readme.txt", "hi");
    auto source = folder_source(empty);
    CHECK(import_game(s, *source, fresh("empty-dest")).result == ImportResult::NoExecutable);
    // A required folder is missing: nothing is copied.
    const std::string partial = fresh("partial-src");
    make_install(partial);
    remove_tree(partial + "/levels");
    source = folder_source(partial);
    const std::string pdest = fresh("partial-dest") + "/game";
    ImportOutcome out = import_game(s, *source, pdest);
    CHECK(out.result == ImportResult::Incomplete);
    CHECK(out.error.find("levels") != std::string::npos);
    CHECK(!exists(pdest));
    // The wrong build: copied, but never stamped.
    const std::string wrong = fresh("wrong-src");
    make_install(wrong);
    write_file(wrong + "/game.exe", "not the supported build");
    source = folder_source(wrong);
    const std::string wdest = fresh("wrong-dest") + "/game";
    out = import_game(s, *source, wdest);
    CHECK(out.result == ImportResult::WrongVersion);
    CHECK(!exists(wdest + "/.stamp"));
    // More headroom than any disk has.
    s.min_free_bytes = uint64_t(1) << 62;
    const std::string ndest = fresh("nospace-dest") + "/game";
    make_install(fresh("nospace-src"));
    source = folder_source(g_scratch + "/nospace-src");
    out = import_game(s, *source, ndest);
    CHECK(out.result == ImportResult::NoSpace);
    CHECK(out.bytes_needed > out.bytes_free);
    CHECK(!exists(ndest + "/game.exe"));
    std::vector<Entry> none;
    CHECK(!folder_source(g_scratch + "/nowhere")->list(&none, &out.error));
}

void test_zip_import() {
    const Spec s = spec();
    const std::string dir = fresh("zip");
    const std::vector<std::pair<std::string, std::string>> files = {
        {"GAME.exe", kExeBytes},        {"data/a.bin", "aaaa"}, {"Levels/one.lvl", "1"},
        {"__redist/setup.exe", "skip"}, {"x.dll", "skip"},      {"notes.txt", "keep"}};
    // Inside a top folder, as most archives are, with macOS clutter.
    std::vector<std::pair<std::string, std::string>> with_junk = files;
    make_zip(dir + "/game.zip", "Test Game/", with_junk);
    {
        mz_zip_archive zip{};
        // Add clutter and a hostile name to a second archive.
        CHECK(mz_zip_writer_init_file(&zip, (dir + "/evil.zip").c_str(), 0));
        for (const auto &f : files)
            mz_zip_writer_add_mem(&zip, f.first.c_str(), f.second.data(), f.second.size(), 0);
        mz_zip_writer_add_mem(&zip, "__MACOSX/._GAME.exe", "x", 1, 0);
        mz_zip_writer_add_mem(&zip, "../escape.txt", "x", 1, 0);
        mz_zip_writer_add_mem(&zip, "data/.DS_Store", "x", 1, 0);
        CHECK(mz_zip_writer_finalize_archive(&zip));
        mz_zip_writer_end(&zip);
    }
    std::string error;
    auto zip = zip_source(dir + "/game.zip", &error);
    CHECK(zip != nullptr);
    if (!zip)
        return;
    const std::string dest = dir + "/out/game";
    ImportOutcome out = import_game(s, *zip, dest);
    CHECK(out.result == ImportResult::Done);
    CHECK(out.files_copied == 4);
    CHECK(read_file(dest + "/GAME.exe") == kExeBytes);
    CHECK(exists(dest + "/Levels/one.lvl"));
    CHECK(!exists(dest + "/x.dll"));
    CHECK(check(s, dest).state == State::Ready);

    auto evil = zip_source_fd(os_fd_open((dir + "/evil.zip").c_str(), OS_O_RDONLY), &error);
    CHECK(evil != nullptr);
    if (evil) {
        const std::string edest = dir + "/evil-out/game";
        out = import_game(s, *evil, edest);
        CHECK(out.result == ImportResult::Done);
        CHECK(!exists(dir + "/evil-out/escape.txt"));
        CHECK(!exists(edest + "/__MACOSX"));
        CHECK(!exists(edest + "/data/.DS_Store"));
    }
    write_file(dir + "/not.zip", "plain text");
    CHECK(zip_source(dir + "/not.zip", &error) == nullptr);
    CHECK(!error.empty());
    CHECK(zip_source_fd(-1, &error) == nullptr);
}

void test_folders_and_profile() {
    const std::string dir = fresh("folders");
    const std::string file = dir + "/profile/launcher-folders.txt";
    CHECK(load_folders(file).empty());
    CHECK(remember_folder(file, "/a"));
    CHECK(remember_folder(file, "/b/"));
    CHECK(remember_folder(file, "/a"));
    std::vector<std::string> f = load_folders(file);
    CHECK(f.size() == 2 && f[0] == "/a" && f[1] == "/b");
    for (int i = 0; i < 12; ++i)
        remember_folder(file, "/x" + std::to_string(i));
    CHECK(load_folders(file).size() == 8);
    CHECK(forget_folder(file, "/x11"));
    CHECK(load_folders(file)[0] == "/x10");

    const std::string profile = fresh("profile");
    write_file(profile + "/mod-settings.json", "{}\n");
    write_file(profile + "/saves/slot1.sav", "save one");
    write_file(profile + "/game/huge.bin", "not a save");
    write_file(profile + "/logs/run.log", "log");
    const std::string zip = dir + "/saves.zip";
    std::string error;
    CHECK(export_profile(profile, zip, {"game", "logs"}, &error));
    const std::string restored = fresh("restored");
    write_file(restored + "/saves/slot2.sav", "kept");
    CHECK(import_profile(zip, restored, &error));
    CHECK(read_file(restored + "/saves/slot1.sav") == "save one");
    CHECK(read_file(restored + "/saves/slot2.sav") == "kept");
    CHECK(exists(restored + "/mod-settings.json"));
    CHECK(!exists(restored + "/game"));
    CHECK(!exists(restored + "/logs"));
    CHECK(!import_profile(dir + "/missing.zip", restored, &error));
}

void test_detection() {
    const Spec s = spec();
    const std::string base = fresh("detect");
    make_install(base + "/Games/Test Game/sub");              // named: two levels
    make_install(base + "/Games/Other Name");                 // direct
    make_install(base + "/Games/Unrelated/deep/deeper");      // too deep, unnamed
    make_install(base + "/Steam/steamapps/common/Test Game"); // a base that is the game
    write_file(base + "/Games/A Patch/game.exe", kExeBytes);  // no data: not offered
    write_file(base + "/Games/A Patch/levels/one.lvl", "patched");
    std::vector<std::string> found = detect_under(
        s, {base + "/Games", base + "/Steam/steamapps/common/Test Game", base + "/missing"});
    CHECK(found.size() == 3);
    auto has = [&](const std::string &p) {
        for (const std::string &f : found)
            if (f == p)
                return true;
        return false;
    };
    CHECK(has(base + "/Games/Test Game/sub"));
    CHECK(has(base + "/Games/Other Name"));
    CHECK(has(base + "/Steam/steamapps/common/Test Game"));
    CHECK(!has(base + "/Games/A Patch"));
    // The real scan runs and returns folders that hold the executable.
    for (const std::string &f : detect_installs(s))
        CHECK(!find_name(f, s.executable).empty());
}

// A platform whose pickers answer at once with whatever the test set.
struct FakePlatform : Platform {
    PlatformInfo pi;
    std::vector<Picked> next;
    std::vector<std::string> cands;
    bool move_all = false;
    std::string opened_url, opened_folder;
    int activity_on = 0, activity_off = 0, protected_count = 0, released = 0, exported = 0;
    PlatformInfo info() override {
        return pi;
    }
    void answer(PickDone done) {
        auto n = next;
        next.clear();
        done(n, "");
    }
    void pick_folder(PickDone done) override {
        answer(done);
    }
    void pick_zip(PickDone done) override {
        answer(done);
    }
    void pick_export(const std::string &, PickDone done) override {
        answer(done);
    }
    void pick_saves(PickDone done) override {
        answer(done);
    }
    std::vector<std::string> candidates(const Spec &) override {
        return cands;
    }
    void open_folder(const std::string &f) override {
        opened_folder = f;
    }
    void open_url(const std::string &u) override {
        opened_url = u;
    }
    void import_activity(bool active, const Progress *p) override {
        if (!p)
            (active ? activity_on : activity_off)++;
    }
    void protect_import(const std::string &) override {
        ++protected_count;
    }
    void release(const Picked &) override {
        ++released;
    }
    bool movable(const Picked &) override {
        return move_all;
    }
    void export_ready(const Picked &) override {
        ++exported;
    }
    void run_in_background(std::function<void()> work) override {
        work();
    }
};

bool has_button(Launcher &l, int id) {
    for (const Button &b : l.buttons())
        if (b.id == id)
            return true;
    return false;
}

void test_ui_desktop() {
    Spec s = spec();
    s.store_url = "https://example.test/buy";
    const std::string dir = fresh("ui-desktop");
    FakePlatform fp;
    fp.pi.plays_in_place = true;
    fp.pi.can_open_folder = true;
    fp.pi.import_root = dir + "/profile/game";
    fp.pi.profile_dir = dir + "/profile";
    fp.pi.folders_file = dir + "/profile/launcher-folders.txt";
    fp.pi.cache_file = dir + "/profile/launcher-verified.txt";
    Launcher l(s, fp);
    l.start("");
    CHECK(l.status().state == State::NotFound);
    CHECK(!has_button(l, kPlay));
    CHECK(has_button(l, kLocate) && has_button(l, kStore) && has_button(l, kQuit));
    l.activate(kStore);
    CHECK(fp.opened_url == s.store_url);

    // Locate a folder two levels above the install.
    make_install(dir + "/Games/Test Game/app");
    fp.next = {Picked{dir + "/Games", "", false, ""}};
    l.activate(kLocate);
    l.tick();
    CHECK(l.status().state == State::Ready);
    CHECK(has_button(l, kPlay) && has_button(l, kCopyIntoApp));
    CHECK(load_folders(fp.pi.folders_file).size() == 1);

    // A new launcher remembers it; Play finishes with the executable.
    Launcher again(s, fp);
    again.start("");
    CHECK(again.status().state == State::Ready);
    again.key(Key::Activate); // Play has the focus
    CHECK(again.finished());
    CHECK(again.exe() == dir + "/Games/Test Game/app/game.exe");

    // A wrong pick explains itself.
    fp.next = {Picked{dir + "/profile", "", false, ""}};
    l.activate(kLocate);
    l.tick();
    CHECK(l.screen() == Screen::Message);
    CHECK(l.message().find("GAME.EXE") != std::string::npos);
    l.key(Key::Back);
    CHECK(l.screen() == Screen::Main);

    // Copy into app storage, then export and import saves.
    l.activate(kCopyIntoApp);
    l.tick();
    CHECK(l.screen() == Screen::Main);
    CHECK(l.status().state == State::Ready && l.status().root == fp.pi.import_root);
    CHECK(fp.activity_on == 1 && fp.activity_off == 1 && fp.protected_count == 1);
    write_file(fp.pi.profile_dir + "/saves/a.sav", "A");
    l.activate(kManage);
    CHECK(l.screen() == Screen::Manage && has_button(l, kDeleteData));
    fp.next = {Picked{dir + "/export.zip", "", false, ""}};
    l.activate(kExportSaves);
    l.tick();
    CHECK(l.message().find("exported") != std::string::npos);
    CHECK(fp.exported == 1);
    l.activate(kOk);
    l.activate(kManage);
    write_file(fp.pi.profile_dir + "/saves/a.sav", "changed");
    fp.next = {Picked{dir + "/export.zip", "", false, ""}};
    l.activate(kImportSaves);
    l.tick();
    CHECK(read_file(fp.pi.profile_dir + "/saves/a.sav") == "A");
    // The export leaves the copied game out.
    std::string err;
    auto zip = zip_source(dir + "/export.zip", &err);
    std::vector<Entry> entries;
    CHECK(zip && zip->list(&entries, &err));
    for (const Entry &e : entries)
        CHECK(e.relative.compare(0, 5, "game/") != 0);
    l.activate(kOk);
    l.activate(kManage);
    l.activate(kOpenFolder);
    CHECK(fp.opened_folder == fp.pi.import_root);
    l.activate(kOpenFolder + 2000);   // run 19: the mods folder, created on demand
    CHECK(fp.opened_folder == fp.pi.profile_dir + "/Mods");
    CHECK(exists(fp.pi.profile_dir + "/Mods"));
    l.activate(kOpenFolder + 3000);   // run 31: the DataMods folder, created on demand
    CHECK(fp.opened_folder == fp.pi.profile_dir + "/DataMods");
    CHECK(exists(fp.pi.profile_dir + "/DataMods"));
    // Delete needs a second press.
    l.activate(kDeleteData);
    CHECK(exists(fp.pi.import_root));
    l.activate(kDeleteData);
    CHECK(!exists(fp.pi.import_root));
    CHECK(exists(fp.pi.profile_dir + "/saves/a.sav"));
    CHECK(l.screen() == Screen::Message);

    // Dropping a folder locates it; dropping a ZIP imports it.
    make_install(dir + "/Dropped");
    l.activate(kOk);
    l.drop(dir + "/Dropped");
    CHECK(l.status().state == State::Ready && l.status().root == dir + "/Dropped");
    l.drop(dir + "/Dropped/readme.txt");
    CHECK(l.screen() == Screen::Message); // not a ZIP
    l.drop(dir + "/nothing-here");
    CHECK(l.screen() == Screen::Message);

    // Drawing lays every button out inside the canvas; clicks hit them.
    Canvas c;
    for (auto size : {std::pair<int, int>{1280, 800}, {640, 400}, {2732, 2048}}) {
        c.resize(size.first, size.second, true);
        const int scale = std::max(1, std::min(size.first / 480, size.second / 300));
        l.activate(kOk);
        l.draw(c, scale);
        for (const Button &b : l.buttons())
            CHECK(b.rect.w > 0 && b.rect.x >= 0 && b.rect.y >= 0 &&
                  b.rect.x + b.rect.w <= c.width() && b.rect.y + b.rect.h <= c.height());
    }
    // A short, wide screen keeps every button on it; Down moves to a button
    // below and Up comes back.
    c.resize(2856, 800, true);
    l.draw(c, 4);
    {
        const std::vector<Button> bs = l.buttons();
        for (const Button &b : bs)
            CHECK(b.rect.x >= 0 && b.rect.y >= 0 && b.rect.x + b.rect.w <= c.width() &&
                  b.rect.y + b.rect.h <= c.height());
        const int from = l.focus();
        l.key(Key::Down);
        CHECK(l.focus() != from && bs[size_t(l.focus())].rect.y > bs[size_t(from)].rect.y);
        l.key(Key::Up);
        CHECK(l.focus() == from);
    }
    Button quit{0, "", true, {}};
    for (const Button &b : l.buttons())
        if (b.id == kQuit)
            quit = b;
    CHECK(quit.id == kQuit);
    l.pointer(quit.rect.x + 2, quit.rect.y + 2, true);
    l.pointer(quit.rect.x + 2, quit.rect.y + 2, false);
    CHECK(l.quit());
}

void test_ui_mobile() {
    const Spec s = spec();
    const std::string dir = fresh("ui-mobile");
    FakePlatform fp;
    fp.pi.plays_in_place = false;
    fp.pi.touch = true;
    fp.pi.import_root = dir + "/Documents/game";
    fp.pi.profile_dir = dir + "/profile";
    fp.pi.drop_hint = "Or copy the folder into the app's Documents.";
    Launcher l(s, fp);
    l.start("");
    CHECK(l.status().state == State::NotFound);
    CHECK(has_button(l, kImportFolder) && has_button(l, kImportZip) && !has_button(l, kLocate));

    // Importing a ZIP.
    make_zip(dir + "/game.zip", "Test Game/",
             {{"GAME.EXE", kExeBytes}, {"DATA/x.bin", "x"}, {"levels/l.lvl", "l"}});
    fp.next = {Picked{dir + "/game.zip", "", false, "game.zip"}};
    l.activate(kImportZip);
    l.tick();
    CHECK(l.status().state == State::Ready);
    CHECK(l.screen() == Screen::Main);
    CHECK(has_button(l, kPlay) && has_button(l, kImportFolder));
    CHECK(fp.protected_count == 1 && fp.released == 1);

    // A phone in landscape: every screen's buttons stay on it, whatever the text.
    {
        FakePlatform tall = fp;
        tall.pi.drop_hint = std::string(400, 'x');
        tall.pi.import_root = dir + "/elsewhere/game";
        tall.pi.profile_dir = dir + "/" + std::string(300, 'p');
        Launcher small(s, tall);
        small.start("");
        CHECK(small.status().state == State::NotFound);
        Canvas c;
        c.resize(2856, 1280, true);
        for (int id : {0, int(kManage)}) {
            if (id)
                small.activate(id);
            small.draw(c, 4);
            CHECK(!small.buttons().empty());
            for (const Button &b : small.buttons())
                CHECK(b.rect.y >= 0 && b.rect.y + b.rect.h <= c.height());
        }
    }

    // A device that cannot run the game still imports; Play explains why not.
    Launcher cannot(s, fp);
    cannot.start("");
    cannot.set_unplayable("No usable graphics.");
    cannot.set_auto_play(1.5);
    CHECK(!cannot.counting_down());
    cannot.activate(kPlay);
    CHECK(!cannot.finished() && cannot.screen() == Screen::Message);
    CHECK(cannot.message() == "No usable graphics.");

    // A ready game starts by itself unless the screen is touched.
    Launcher auto_play(s, fp);
    auto_play.start("");
    auto_play.set_auto_play(1.5);
    CHECK(auto_play.counting_down());
    Canvas c;
    c.resize(960, 600, false);
    auto_play.draw(c, 2);
    auto_play.advance(1.0);
    CHECK(!auto_play.finished());
    auto_play.advance(1.0);
    CHECK(auto_play.finished());
    Launcher stopped(s, fp);
    stopped.start("");
    stopped.set_auto_play(1.5);
    stopped.pointer(5, 5, true);
    stopped.pointer(5, 5, false);
    stopped.advance(5);
    CHECK(!stopped.finished() && !stopped.quit() && stopped.screen() == Screen::Main);

    // A developer build's bundled copy is imported without asking.
    const std::string bundle = dir + "/App.app/game";
    make_install(bundle);
    remove_tree(fp.pi.import_root);
    fp.pi.auto_import = bundle;
    Launcher seeded(s, fp);
    seeded.start("");
    seeded.tick();
    CHECK(seeded.status().state == State::Ready && seeded.status().root == fp.pi.import_root);
    fp.pi.auto_import.clear();

    // A folder copied into Documents shows as a candidate and imports.
    make_install(dir + "/Documents/My Copy");
    fp.cands = {dir + "/Documents/My Copy"};
    remove_tree(fp.pi.import_root);
    l.start("");
    CHECK(l.status().state == State::NotFound);
    CHECK(has_button(l, kUseCandidate));
    fp.move_all = true;
    l.activate(kUseCandidate);
    l.tick();
    CHECK(l.status().state == State::Ready);
    CHECK(!exists(dir + "/Documents/My Copy")); // moved in, and the rest removed
    fp.move_all = false;
    make_install(dir + "/Documents/My Copy");

    // A failing import says why; cancelling says it can continue.
    write_file(dir + "/empty/readme.txt", "nothing here");
    fp.next = {Picked{dir + "/empty", "", false, ""}};
    l.activate(kImportFolder);
    l.tick();
    CHECK(l.screen() == Screen::Message);
    CHECK(l.message().find("do not contain") != std::string::npos);
    CHECK(l.status().state == State::Ready); // the earlier import is untouched
    // The app's own copy, or a folder holding it, is refused.
    fp.next = {Picked{dir + "/Documents", "", false, ""}};
    l.activate(kOk);
    remove_tree(dir + "/Documents/My Copy");
    l.activate(kImportFolder);
    l.tick();
    CHECK(l.message().find("already imported") != std::string::npos);

    fp.next = {Picked{dir + "/missing.zip", "", false, "missing.zip"}};
    l.activate(kOk);
    l.activate(kImportZip);
    l.tick();
    CHECK(l.message().find("Cannot read missing.zip") != std::string::npos);
}

} // namespace

// TrueType text: UTF-8, wrapping by measured width, cut ends, the glyph cache.
void test_text() {
    // UTF-8 code points, malformed bytes as U+FFFD.
    {
        const std::string s = "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87 \xE2\x80\xA6\xF0\x9F\x98\x80\xC5";
        std::vector<uint32_t> cps;
        for (size_t i = 0; i < s.size();)
            cps.push_back(utf8_next(s, &i));
        CHECK((cps == std::vector<uint32_t>{'Z', 'a', 0x17C, 0xF3, 0x142, 0x107, ' ', 0x2026, 0x1F600, 0xFFFD}));
        CHECK(utf8_length("\xC5\xBC\xC3\xB3\xC5\x82w") == 4);
    }
    // A measure of one unit per code point: wrapping never splits a letter's bytes.
    const Measure units = [](const std::string &t) { return int(utf8_length(t)); };
    {
        const std::vector<std::string> lines = wrap_text("Zażółć gęślą jaźń i koń", 12, units);
        CHECK((lines == std::vector<std::string>{"Zażółć gęślą", "jaźń i koń"}));
        // A word longer than a line is split between code points.
        const std::vector<std::string> split = wrap_text("Źdźbłożółtego", 5, units);
        CHECK((split == std::vector<std::string>{"Źdźbł", "ożółt", "ego"}));
        // '\n' starts a line; an empty paragraph stays an empty line.
        CHECK((wrap_text("a\n\nb", 10, units) == std::vector<std::string>{"a", "", "b"}));
    }
    {
        // Cut with "…" at the end, at a word when one fits; the middle for paths.
        CHECK(ellipsize("Gothic", 10, units) == "Gothic");
        CHECK(ellipsize("Animations Redone", 9, units) == "Animatio\xE2\x80\xA6");
        const std::vector<std::string> two = wrap_lines("Alternative Balance Uncapped Edition", 12, 2, units);
        CHECK(two.size() == 2 && two[0] == "Alternative" && two[1] == "Balance\xE2\x80\xA6");
        const std::string mid = ellipsize_middle("~/Documents/gothic 3/stage", 16, units);
        CHECK(utf8_length(mid) <= 16 && mid.find("\xE2\x80\xA6") != std::string::npos &&
              mid.substr(mid.size() - 5) == "stage");
    }
    // A real face when the system has one: measured, kerned, cached once per glyph.
    Font f;
    const char *faces[] = {"/System/Library/Fonts/Supplemental/Hoefler Text.ttc#0",
                           "/System/Library/Fonts/Supplemental/Georgia.ttf",
                           "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf"};
    for (const char *face : faces)
        if (f.load(face))
            break;
    if (f.ok()) {
        CHECK(f.measure("", 16) == 0);
        const int one = f.measure("Mody", 16), two = f.measure("Mody", 32);
        CHECK(one > 0 && two >= 2 * one - 2 && two <= 2 * one + 2);
        CHECK(f.has_glyph(0x105) && f.measure("\xC4\x85", 16) > 0); // ą
        CHECK(f.ascent(16) > 0 && f.descent(16) > 0 && f.line_height(16) >= f.ascent(16));
        Canvas c;
        c.resize(200, 40, false);
        c.clear({0, 0, 0, 255});
        const int w = f.draw(c, 4, 28, "Wróć", 20, {255, 255, 255, 255});
        CHECK(w > 0 && w == f.measure("Wróć", 20));
        const size_t cached = f.cached_glyphs();
        CHECK(cached >= 3);
        f.draw(c, 4, 28, "Wróć", 20, {255, 255, 255, 255});
        CHECK(f.cached_glyphs() == cached); // the same glyphs at the same phases
        // Anti-aliased: partly covered pixels, not only black and white.
        int partial = 0, lit = 0;
        for (int i = 0; i < c.width() * c.height(); ++i) {
            const uint8_t v = c.pixels()[i * 4];
            partial += v > 20 && v < 235;
            lit += v > 0;
        }
        CHECK(lit > 0 && partial > lit / 10);
    } else
        printf("  (no system font: TrueType checks skipped)\n");
    // Without a font file the bitmap font stands in, measured the same way it draws.
    Font none;
    CHECK(!none.load("/nonexistent/font.ttf"));
    Canvas c;
    c.resize(120, 20, true);
    CHECK(none.measure("abc", 9) == none.draw(c, 0, 12, "abc", 9, {255, 255, 255, 255}));
    // Rounded corners: the distance is negative inside, zero on the edge.
    const Rect r{10, 10, 40, 20};
    CHECK(round_rect_distance(30, 20, r, 6) < -5 && round_rect_distance(10, 20, r, 6) > -0.01f &&
          round_rect_distance(10, 20, r, 6) < 0.01f);
    CHECK(round_rect_distance(10.5f, 10.5f, r, 6) > 0); // the cut corner
}

void test_ui_text_fits() {
    // Paths wrap after a slash, not inside a folder name.
    const std::vector<std::string> lines =
        Canvas::wrap("/Users/player/Documents/profile", 12 * 6, 1);
    CHECK((lines == std::vector<std::string>{"/Users/", "player/", "Documents/", "profile"}));

    // A long title keeps the right margin clear, at any width.
    Spec s = spec();
    s.title = "Siege of Avalon: Anthology of Long Titles";
    FakePlatform fp;
    fp.pi.profile_dir = fresh("ui-title") + "/profile";
    Launcher l(s, fp);
    l.start("");
    Canvas c;
    for (int w : {640, 1280, 2420}) {
        const int scale = std::max(1, std::min(w / 480, 1668 / 300));
        c.resize(w, 1668, false);
        l.draw(c, scale);
        const int margin = 16 * scale;
        int lit = 0;
        for (int y = margin; y < margin + 24 * scale; ++y)
            for (int x = w - margin; x < w; ++x)
                lit += c.pixels()[(y * w + x) * 4] > 0x80;
        CHECK(lit == 0);
    }
}

namespace {

// ---------------------------------------------------------------------------
// The mod screen (Gothic 3, a Sefaris Mod Manager catalog)
// ---------------------------------------------------------------------------
std::string mod_json(const std::string &id, const std::string &category,
                     const std::string &deps = "", const std::string &incompatible = "") {
    return "{\n  \"id\": \"" + id + "\",\n  \"title\": \"" + id + " Mod\",\n" +
           (category.empty() ? "" : "  \"category\": \"" + category + "\",\n") +
           "  \"dependencies\": [" + deps + "],\n  \"incompatibles\": [" + incompatible +
           "],\n  \"authors\": [\"Someone\", \"Else\"],\n  \"version\": 1.5,\n  \"beta\": false\n}";
}

// A catalog laid out as G3_Mods is: Mods/<folder>/mod.json, images/, Readme/, presets/.
std::string make_mod_catalog(const char *name) {
    const std::string root = fresh(name);
    const std::string mods = root + "/Mods";
    write_file(mods + "/A_1_Base/mod.json", mod_json("Base", "Content"));
    write_file(mods + "/A_1_Base/images/315x312.jpg", "not really a jpeg");
    write_file(mods + "/A_1_Base/Readme/readme_gb.md", "English");
    write_file(mods + "/A_1_Base/Readme/readme_pl.md", "# Opis\n\nPolski **opis**.\n");
    write_file(mods + "/A_1_Base/Templates.m0x", "archive");
    // A BOM, and a dependency spelled in another case.
    write_file(mods + "/B_Addon/mod.json", "\xEF\xBB\xBF" + mod_json("Addon", "Gameplay", "\"base\""));
    write_file(mods + "/ZZ_Late/mod.json", mod_json("Late", ""));
    write_file(mods + "/ZZ_Late/images/Head.jpg", "x");
    write_file(mods + "/ZZ_Late/images/anim.gif", "x");
    write_file(mods + "/ZZ_Late/Readme/readme_de.md", "Deutsch");
    write_file(mods + "/Z_Last/mod.json", mod_json("Last", "Gameplay", "", "\"Addon\""));
    write_file(mods + "/Script_Auto/mod.json", mod_json("Auto", "Interface"));
    write_file(mods + "/Script_Auto/scripts/SCRIPT_Auto.DLL", "MZ");
    write_file(mods + "/S_Plain/mod.json", mod_json("Plain", "Content", "\"Missing\""));
    write_file(mods + "/Broken/readme.txt", "no mod.json here");
    write_file(mods + "/Bad/mod.json", "{\"id\": \"Bad\", ");
    write_file(root + "/presets/P1/preset.json",
               "{\"name\": \"P1\", \"modIds\": [\"Last\", \"addon\", \"Auto\", \"Nope\"]}");
    write_file(root + "/presets/P1/extra.ini", "x");
    write_file(root + "/presets/Empty/preset.json", "{\"name\": \"Empty\", \"modIds\": []}");
    write_file(root + "/presets/NoJson/readme.txt", "x");
    return root;
}

// DepotDownloader 3.4.0's console, as captured on 26.10.2026 (anonymous, and
// -qr before a scan), with a QR code shrunk to 5 modules.
void test_steam_output() {
    {
        SteamOutput o;
        o.feed("No username given. Using anonymous account with dedicated server subscription.\n"
               "Connecting to Steam3... Done!\nLogging anonymously into Steam3... Done!\n"
               "Using Steam3 suggested CellID: 183\nGot AppInfo for 39500\n");
        CHECK(o.state().phase == SteamPhase::Starting);
        o.feed("App 39500 (Gothic 3 Classic) is not available from this account.\nDisconnected "
               "from Steam\n");
        CHECK(o.state().phase == SteamPhase::NotOwned);
        o.finish(1);
        CHECK(o.state().phase == SteamPhase::NotOwned && o.state().exit_code == 1);
        CHECK(o.state().detail.find("not available") != std::string::npos);
    }
    {
        // The code arrives in pieces that split the block characters.
        const std::string B = "\xe2\x96\x88\xe2\x96\x88", W = "  ";
        std::string block = "Use the Steam Mobile App to sign in with this QR code:\n";
        const char *rows[] = {"X.X.X", ".X.X.", "XX.XX", ".....", "XXXXX"};
        for (const char *r : rows) {
            for (const char *c = r; *c; ++c)
                block += *c == 'X' ? B : W;
            block += "\n";
        }
        const std::string text =
            "Connecting to Steam3... Done!\nLogging in with QR code...\n" + block;
        SteamOutput o;
        for (size_t i = 0; i < text.size(); i += 7)
            o.feed(text.substr(i, 7));
        CHECK(o.state().phase == SteamPhase::Qr);
        CHECK(o.state().qr.size() == 5 && o.state().qr[0].size() == 5);
        CHECK(o.state().qr_serial == 1);
        if (o.state().qr.size() == 5) {
            CHECK(o.state().qr[0][0] && !o.state().qr[0][1] && o.state().qr[0][2]);
            CHECK(!o.state().qr[3][2] && o.state().qr[4][4]);
        }
        // Steam refreshes the code.
        o.feed("\nThe QR code has changed:\n" + block);
        CHECK(o.state().qr_serial == 2);
        o.feed("Success! Next time you can login with -username player_one -remember-password "
               "instead of -qr.\n");
        CHECK(o.state().account == "player_one");
        o.feed("Done!\nGot 12 licenses for account!\nGot AppInfo for 39500\nDownloading depot "
               "39501\n");
        CHECK(o.state().phase == SteamPhase::Downloading);
        o.feed("Pre-allocating /data/game/Engine.dll\n 12.50% /data/game/Engine.dll\n");
        CHECK(o.state().percent == 12.5 && o.state().file == "/data/game/Engine.dll");
        o.feed(" 99,75% /data/game/Data/gui.pak\n");
        CHECK(o.state().percent == 99.75);
        o.feed("Depot 39501 - Downloaded 10 bytes (20 bytes uncompressed)\n"
               "Total downloaded: 10 bytes (20 bytes uncompressed) from 1 depots\nDisconnected "
               "from Steam\n");
        o.finish(0);
        CHECK(o.state().phase == SteamPhase::Done && o.state().percent == 100);
    }
    {
        // A password login: the questions wait on their lines without a newline.
        SteamOutput o;
        o.feed("Enter account password for \"player\": ");
        CHECK(o.state().phase == SteamPhase::NeedPassword);
        o.answered();
        CHECK(o.state().phase == SteamPhase::Starting);
        o.feed("\nConnecting to Steam3... Done!\nLogging 'player' into Steam3...\n"
               "STEAM GUARD! Please enter your 2-factor auth code from your authenticator app: ");
        CHECK(o.state().phase == SteamPhase::NeedCode && !o.state().email);
        o.answered();
        o.feed("The previous 2-factor auth code you have provided is incorrect.\n"
               "STEAM GUARD! Please enter your 2-factor auth code from your authenticator app: ");
        CHECK(o.state().phase == SteamPhase::NeedCode && o.state().code_rejected);
        o.answered();
        CHECK(!o.state().code_rejected);
        o.feed("STEAM GUARD! Please enter the auth code sent to the email at p***@example.com: ");
        CHECK(o.state().phase == SteamPhase::NeedCode && o.state().email);
        o.answered();
        o.feed("STEAM GUARD! Use the Steam Mobile App to confirm your sign in...\n");
        CHECK(o.state().phase == SteamPhase::ConfirmInApp);
        o.feed("Failed to authenticate with Steam: InvalidPassword\n");
        CHECK(o.state().phase == SteamPhase::LoginFailed);
        CHECK(o.state().detail == "Failed to authenticate with Steam: InvalidPassword");
        o.finish(1);
        CHECK(o.state().phase == SteamPhase::LoginFailed);
    }
    {
        // Ended without finishing: the last line worth showing says why.
        SteamOutput o;
        o.feed("Connecting to Steam3... Done!\nEncountered 401 for depot manifest 39501 1. "
               "Aborting.\nmore\n");
        o.finish(1);
        CHECK(o.state().phase == SteamPhase::Failed);
        CHECK(o.state().detail.find("Encountered 401") == 0);
        SteamOutput quiet;
        quiet.finish(134);
        CHECK(quiet.state().phase == SteamPhase::Failed && !quiet.state().detail.empty());
        SteamOutput early; // a clean exit before "Total downloaded" is no download
        early.feed("Connecting to Steam3... Done!\n");
        early.finish(0);
        CHECK(early.state().phase == SteamPhase::Failed);
    }
}

void test_steam_arguments() {
    SteamSpec spec;
    spec.tool = "DepotDownloader";
    spec.app = 39500;
    SteamDepot d;
    d.id = 39501;
    d.manifest = "8013367546631285348";
    d.branch = "cpt_1.75_testbranch";
    const std::vector<std::string> qr = steam_arguments(spec, d, "/g", "/g/.files", "");
    const std::vector<std::string> want = {"-app",      "39500",
                                           "-depot",    "39501",
                                           "-manifest", "8013367546631285348",
                                           "-branch",   "cpt_1.75_testbranch",
                                           "-os",       "windows",
                                           "-dir",      "/g",
                                           "-filelist", "/g/.files",
                                           "-qr",       "-remember-password"};
    CHECK(qr == want);
    d.branch.clear();
    const std::vector<std::string> named = steam_arguments(spec, d, "/g", "", "player");
    CHECK(std::find(named.begin(), named.end(), "-branch") == named.end());
    CHECK(std::find(named.begin(), named.end(), "-filelist") == named.end());
    CHECK(named.size() >= 4 && named[named.size() - 3] == "-username" &&
          named[named.size() - 2] == "player");
}

void test_steam_accounts() {
    // AccountSettingsStore: 2 = penalties (a map), 4 = login tokens, 5 = guard data.
    auto entry = [](int field, const std::string &k, const std::string &v) {
        std::string inner;
        inner += char(0x0a);
        inner += char(k.size());
        inner += k;
        inner += char(0x12);
        inner += char(v.size());
        inner += v;
        std::string out;
        out += char(field << 3 | 2);
        out += char(inner.size());
        return out + inner;
    };
    const std::string msg = entry(2, "cm.steam", std::string(1, '\x08')) +
                            entry(4, "alice", "token-a") + entry(5, "alice", "guard") +
                            entry(4, "bob", "token-b");
    size_t packed = 0;
    void *raw = tdefl_compress_mem_to_heap(msg.data(), msg.size(), &packed, 128);
    CHECK(raw != nullptr);
    std::vector<uint8_t> file(static_cast<uint8_t *>(raw), static_cast<uint8_t *>(raw) + packed);
    mz_free(raw);
    const std::vector<std::string> names = steam_saved_accounts_from_bytes(file);
    CHECK(names == std::vector<std::string>({"alice", "bob"}));
    CHECK(steam_saved_accounts_from_bytes({1, 2, 3}).empty());
    // Found under the HOME the tool ran with, however deep .NET puts it.
    const std::string home = fresh("steam-home");
    const std::string dir =
        home + "/Library/Application Support/IsolatedStorage/ab/cd/Url.x/AssemFiles";
    write_file(dir + "/account.config", std::string(file.begin(), file.end()));
    CHECK(steam_saved_accounts(home) == std::vector<std::string>({"alice", "bob"}));
    CHECK(steam_saved_accounts(home + "/nowhere").empty());
}

// A PE32 of two sections, .text (a dword to fix at 0x1010) and .reloc.
std::vector<uint8_t> tiny_pe(uint32_t base, uint16_t reloc_type) {
    std::vector<uint8_t> b(0x600, 0);
    auto w16 = [&](size_t at, uint16_t v) { b[at] = uint8_t(v), b[at + 1] = uint8_t(v >> 8); };
    auto w32 = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i)
            b[at + size_t(i)] = uint8_t(v >> (8 * i));
    };
    w16(0, 0x5a4d);
    w32(0x3c, 0x40);
    w32(0x40, 0x00004550);
    w16(0x44, 0x14c);
    w16(0x46, 2);
    w16(0x54, 224);
    const size_t opt = 0x58;
    w16(opt, 0x10b);
    w32(opt + 28, base);
    w32(opt + 60, 0x200);
    w32(opt + 92, 16);
    w32(opt + 96 + 5 * 8, 0x2000);
    w32(opt + 96 + 5 * 8 + 4, 12);
    const size_t sec = opt + 224;
    memcpy(&b[sec], ".text", 5);
    w32(sec + 8, 0x100), w32(sec + 12, 0x1000), w32(sec + 16, 0x200), w32(sec + 20, 0x200);
    memcpy(&b[sec + 40], ".reloc", 6);
    w32(sec + 48, 12), w32(sec + 52, 0x2000), w32(sec + 56, 0x200), w32(sec + 60, 0x400);
    w32(0x200 + 0x10, base + 0x1234);
    w32(0x400, 0x1000);
    w32(0x404, 12);
    w16(0x408, uint16_t(reloc_type << 12 | 0x010));
    w16(0x40a, 0);
    return b;
}

std::string sha_of(const std::vector<uint8_t> &bytes, const std::string &scratch) {
    write_file(scratch, std::string(bytes.begin(), bytes.end()));
    return sha256_file(scratch);
}

void test_stage() {
    std::string why;
    std::vector<uint8_t> pe = tiny_pe(0x10000000, 3);
    CHECK(rebase_pe(&pe, 0x20000000, &why));
    auto rd = [&](size_t at) {
        return uint32_t(pe[at]) | uint32_t(pe[at + 1]) << 8 | uint32_t(pe[at + 2]) << 16 |
               uint32_t(pe[at + 3]) << 24;
    };
    CHECK(rd(0x210) == 0x20001234);
    CHECK(rd(0x58 + 28) == 0x20000000);
    std::vector<uint8_t> dir64 = tiny_pe(0x10000000, 10);
    CHECK(!rebase_pe(&dir64, 0x20000000, &why) && why.find("type 10") != std::string::npos);
    std::vector<uint8_t> junk(64, 0);
    CHECK(!rebase_pe(&junk, 0x20000000, &why));

    // A module made in place from the install's file, one already made, one from a subfolder.
    const std::string root = fresh("stage");
    const std::vector<uint8_t> a = tiny_pe(0x10000000, 3), b = tiny_pe(0x10000000, 3);
    std::vector<uint8_t> a_made = a, b_made = b;
    rebase_pe(&a_made, 0x11000000, &why);
    rebase_pe(&b_made, 0x12000000, &why);
    const std::string a_sha = sha_of(a, root + "/x"), a_made_sha = sha_of(a_made, root + "/x");
    const std::string b_made_sha = sha_of(b_made, root + "/x");
    os_unlink((root + "/x").c_str());
    write_file(root + "/A.dll", std::string(a.begin(), a.end()));
    write_file(root + "/scripts/B.dll", std::string(b_made.begin(), b_made.end()));
    std::vector<StageModule> mods = {
        {"A.dll", "A.dll", "a.DLL", a_sha, "", 0x11000000, a_made_sha},
        {"B.dll", "scripts/B.dll", "scripts/B.dll", a_sha, "", 0x12000000, b_made_sha},
    };
    CHECK(!modules_staged(root, mods));
    std::vector<std::string> heard;
    StageOutcome out = stage_modules(
        root, mods, [&](size_t, size_t, const std::string &n) { heard.push_back(n); });
    CHECK(out.ok && out.made == 1 && out.kept == 1);
    CHECK(heard.size() == 3 && heard[0] == "A.dll" && heard[2].empty());
    CHECK(sha256_file(root + "/A.dll") == a_made_sha);
    CHECK(modules_staged(root, mods));
    CHECK(!exists(root + "/A.dll.part"));
    // Again: nothing to do.
    out = stage_modules(root, mods);
    CHECK(out.ok && out.made == 0 && out.kept == 2);
    // A source that is not the pinned file is refused by name.
    write_file(root + "/A.dll", "something else");
    out = stage_modules(root, mods);
    CHECK(!out.ok && out.error.find("a.DLL has SHA-256") == 0);
    // A result that does not hash to its pin is not kept.
    write_file(root + "/A.dll", std::string(a.begin(), a.end()));
    mods[0].sha256 = std::string(64, '0');
    out = stage_modules(root, mods);
    CHECK(!out.ok && out.error.find("not the expected") != std::string::npos);
    CHECK(sha256_file(root + "/A.dll") == a_sha && !exists(root + "/A.dll.part"));
    mods[0].from = "missing.dll";
    out = stage_modules(root, mods);
    CHECK(!out.ok && out.error == "missing.dll is missing");

    // .ini edits keep the file's line endings and its Latin-1 bytes.
    write_file(root + "/Ini/ge3.ini",
               "; Gr\xfc\xdf"
               "e\r\n[Language]\r\nText=English\r\n\r\n; next\r\n[Engine]\r\nA=1\r\n");
    write_file(root + "/Ini/mountlist.ini", "Data/A=Data/A\r\nData/B=Data/B");
    CHECK(
        apply_ini_edits(root,
                        {"ini/GE3.ini|language|Text=Polish", "Ini/ge3.ini|Language|Audio=Polish",
                         "Ini/mountlist.ini||+Data/C=Data/C", "Ini/mountlist.ini||+Data/A=Data/A"},
                        &why));
    CHECK(read_file(root + "/Ini/ge3.ini") ==
          "; Gr\xfc\xdf"
          "e\r\n[Language]\r\nText=Polish\r\nAudio=Polish\r\n\r\n; next\r\n[Engine]\r\nA=1\r\n");
    CHECK(read_file(root + "/Ini/mountlist.ini") ==
          "Data/A=Data/A\r\nData/B=Data/B\r\nData/C=Data/C");
    CHECK(apply_ini_edits(root, {"Ini/mountlist.ini||-data/b=data/b"}, &why));
    CHECK(read_file(root + "/Ini/mountlist.ini") == "Data/A=Data/A\r\nData/C=Data/C");
    CHECK(!apply_ini_edits(root, {"Ini/ge3.ini|Missing|K=V"}, &why) &&
          why.find("cannot apply") == 0);
    CHECK(!apply_ini_edits(root, {"Ini/none.ini|S|K=V"}, &why) && why.find("cannot read") == 0);
    CHECK(!apply_ini_edits(root, {"no bars"}, &why));

    // A profile gets the game's file once; what the player already has stays.
    write_file(root + "/shader.cache", "compiled");
    const std::string profile = fresh("seed-profile");
    const std::vector<std::string> seeds = {"Shader.Cache|Documents/game/Shader.Cache",
                                            "missing.bin|x/missing.bin"};
    CHECK(seed_profile(root, profile, seeds) == 1);
    CHECK(read_file(profile + "/Documents/game/Shader.Cache") == "compiled");
    write_file(profile + "/Documents/game/Shader.Cache", "players own");
    CHECK(seed_profile(root, profile, seeds) == 0);
    CHECK(read_file(profile + "/Documents/game/Shader.Cache") == "players own");
    CHECK(!exists(profile + "/x"));
}

#ifndef _WIN32
// A stand-in for DepotDownloader: prints what 3.4.0 prints and copies $HOME/src-<depot> into -dir. $HOME/mode
// "notowned" makes it an account without the game. Each run's arguments land in $HOME/args-<depot>.txt.
const char kFakeDepotDownloader[] = R"SH(#!/bin/sh
dir=""; depot=""; qr=0
for a in "$@"; do echo "$a"; done > "$HOME/args-tmp.txt"
while [ $# -gt 0 ]; do
  case "$1" in -dir) dir="$2"; shift;; -depot) depot="$2"; shift;; -qr) qr=1;; esac
  shift
done
mv "$HOME/args-tmp.txt" "$HOME/args-$depot.txt"
echo "Connecting to Steam3... Done!"
if [ "$qr" = 1 ]; then
  echo "Logging in with QR code..."
  echo "Use the Steam Mobile App to sign in with this QR code:"
  printf '\342\226\210\342\226\210  \n  \342\226\210\342\226\210\n'
  sleep 0.2
  echo "Success! Next time you can login with -username tester -remember-password instead of -qr."
fi
echo "Got 3 licenses for account!"
echo "Got AppInfo for 7"
if [ "$(cat "$HOME/mode" 2>/dev/null)" = notowned ]; then
  echo "App 7 (Test Game) is not available from this account."
  exit 1
fi
echo "Downloading depot $depot"
cp -R "$HOME/src-$depot/." "$dir/"
echo " 50.00% $dir/x"
echo "Total downloaded: 10 bytes (20 bytes uncompressed) from 1 depots"
exit 0
)SH";

void test_ui_steam() {
    const std::string dir = fresh("ui-steam");
    const std::string home = dir + "/profile/steam", tool = dir + "/DepotDownloader";
    write_file(tool, kFakeDepotDownloader);
    chmod(tool.c_str(), 0755);
    // Depot 1 is the game with a module to rebase, depot 2 an optional voice pack.
    make_install(home + "/src-1");
    const std::vector<uint8_t> pe = tiny_pe(0x10000000, 3);
    std::vector<uint8_t> made = pe;
    std::string why;
    rebase_pe(&made, 0x11000000, &why);
    write_file(home + "/src-1/A.dll", std::string(pe.begin(), pe.end()));
    write_file(home + "/src-2/voice.pak", "voice");
    Spec s = spec();
    s.store_url = "https://example.test/buy";
    s.steam.tool = "DepotDownloader";
    s.steam.app = 7;
    s.steam.depots = {{1, "11", "beta", {"regex:^(?!redist/)"}, 30, "", {}, {}},
                      {2,
                       "22",
                       "",
                       {"voice.pak"},
                       10,
                       "Extra voice",
                       {"Ini/game.ini|Lang|Audio=Voice", "Ini/mount.ini||+mount voice"},
                       {"Ini/game.ini|Lang|Audio=Base", "Ini/mount.ini||-mount voice"}}};
    s.steam.ini = {"Ini/game.ini|Lang|Text=Polish"};
    s.profile_seed = {"readme.txt|Documents/game/seeded.txt"};
    write_file(home + "/src-1/Ini/game.ini", "[Lang]\r\nText=English\r\nAudio=Base\r\n[Other]\r\nx=1\r\n");
    write_file(home + "/src-1/Ini/mount.ini", "mount base");
    s.stage = {{"A.dll", "A.dll", "A.dll", sha_of(pe, dir + "/x"), "", 0x11000000,
                sha_of(made, dir + "/x")}};
    FakePlatform fp;
    fp.pi.plays_in_place = true;
    fp.pi.import_root = dir + "/profile/game";
    fp.pi.profile_dir = dir + "/profile";
    fp.pi.folders_file = dir + "/profile/launcher-folders.txt";
    fp.pi.cache_file = dir + "/profile/launcher-verified.txt";
    fp.pi.steam_tool = tool;
    fp.pi.steam_home = home;
    auto run = [](Launcher &l, const std::function<bool()> &until) {
        for (int i = 0; i < 500 && !until(); ++i) {
            l.tick();
            os_sleep_us(10000);
        }
        return until();
    };
    {
        // No game: the launcher goes to Steam by itself, shows the code, downloads both, plays.
        Launcher l(s, fp);
        l.start("");
        CHECK(l.screen() == Screen::Steam);
        CHECK(has_button(l, kSteamPassword) && has_button(l, kSteamCancel));
        CHECK(has_button(l, kSteamToggle + 1));
        CHECK(run(l, [&] { return l.finished() || l.screen() == Screen::Message; }));
        CHECK(l.finished());
        CHECK(l.exe() == fp.pi.import_root + "/game.exe");
        CHECK(sha256_file(fp.pi.import_root + "/A.dll") == s.stage[0].sha256);
        CHECK(read_file(fp.pi.import_root + "/voice.pak") == "voice");
        CHECK(read_file(fp.pi.import_root + "/Ini/game.ini") ==
              "[Lang]\r\nText=Polish\r\nAudio=Voice\r\n[Other]\r\nx=1\r\n");
        CHECK(read_file(fp.pi.import_root + "/Ini/mount.ini") == "mount base\nmount voice");
        CHECK(read_file(fp.pi.profile_dir + "/Documents/game/seeded.txt") == "keep");
        CHECK(read_file(home + "/account.txt") == "tester\n");
        const std::string first = read_file(home + "/args-1.txt"),
                          second = read_file(home + "/args-2.txt");
        CHECK(first.find("-qr\n") != std::string::npos &&
              first.find("-branch\nbeta\n") != std::string::npos);
        CHECK(first.find("-filelist\n" + home + "/filelist-1.txt\n") != std::string::npos);
        CHECK(read_file(home + "/filelist-1.txt") == "regex:^(?!redist/)\n");
        CHECK(second.find("-username\ntester\n") != std::string::npos &&
              second.find("-qr") == std::string::npos);
        CHECK(fp.activity_on >= 1 && fp.activity_off >= 1);
    }
    {
        // Downloading again (the game deleted) needs no login, and a part switched off is left out.
        remove_tree(fp.pi.import_root);
        os_unlink((home + "/args-2.txt").c_str());
        Launcher l(s, fp);
        l.start("");
        CHECK(l.screen() == Screen::Steam);
        l.activate(kSteamToggle + 1);
        CHECK(read_file(home + "/skipped.txt") == "Extra voice\n");
        CHECK(run(l, [&] { return l.finished() || l.screen() == Screen::Message; }));
        CHECK(l.finished());
        CHECK(read_file(home + "/args-1.txt").find("-username\ntester\n") != std::string::npos);
        CHECK(!exists(home + "/args-2.txt") && !exists(fp.pi.import_root + "/voice.pak"));
        CHECK(read_file(fp.pi.import_root + "/Ini/game.ini").find("Audio=Base\r\n") != std::string::npos);
        CHECK(read_file(fp.pi.import_root + "/Ini/mount.ini") == "mount base");
    }
    {
        // An account without the game: buy it, or log in to another one.
        remove_tree(fp.pi.import_root);
        write_file(home + "/mode", "notowned");
        Launcher l(s, fp);
        l.start("");
        CHECK(run(l, [&] { return has_button(l, kStore); }));
        CHECK(has_button(l, kSteamOtherAccount) && !l.finished());
        l.activate(kStore);
        CHECK(fp.opened_url == s.store_url);
        l.activate(kBack);
        CHECK(l.screen() == Screen::Main && has_button(l, kSteam));
    }
    {
        // The password form: typing goes to the fields, Enter moves on, and only ASCII is taken.
        os_unlink((home + "/account.txt").c_str());
        Launcher l(s, fp);
        l.start("");
        l.activate(kSteamPassword);
        CHECK(l.wants_text() && has_button(l, kSteamUserField) && has_button(l, kSteamLogin));
        l.type_text("player");
        l.key(Key::Activate);
        l.type_text("pa");
        l.type_text("ś");
        l.type_text("ss");
        l.erase_text();
        bool masked = false;
        for (const Button &b : l.buttons())
            masked = masked || (b.id == kSteamPassField && b.label == "***");
        CHECK(masked);
        l.activate(kSteamQr);
        CHECK(!has_button(l, kSteamUserField));
        l.key(Key::Back);
        run(l, [&] { return l.screen() != Screen::Steam; });
    }
    os_unlink((home + "/mode").c_str());
}
#endif

// A platform with a newer release on GitHub.
struct UpdatePlatform : FakePlatform {
    UpdateInfo offered;
    bool installs = true, install_ok = true;
    int relaunched = 0, checks = 0;
    bool check_update(const std::string &repo, const std::string &asset,
                      UpdateFound done) override {
        ++checks;
        if (repo != "owner/game" || asset != "Game-macOS.zip")
            return false;
        done(true, offered);
        return true;
    }
    bool install_update(const UpdateInfo &, UpdateProgress progress, UpdateDone done) override {
        if (!installs)
            return false;
        progress(0.5);
        done(install_ok, install_ok ? "" : "disk full");
        return true;
    }
    void relaunch() override {
        ++relaunched;
    }
};

void test_ui_update() {
    Spec s = spec();
    s.releases_repo = "owner/game";
    s.update_asset = "Game-macOS.zip";
    const std::string dir = fresh("ui-update");
    UpdatePlatform up;
    up.pi.import_root = dir + "/profile/game";
    up.pi.profile_dir = dir + "/profile";
    up.offered = {"v0.10.0", "https://github.com/owner/game/releases/download/v0.10.0/Game-macOS.zip",
                  "https://example.test/release", 100};
    {
        // 0.10.0 is newer than 0.9.2: offered, installed, started again.
        Launcher l(s, up);
        l.set_version("v0.9.2");
        l.start("");
        l.tick();
        CHECK(has_button(l, kUpdate));
        l.activate(kUpdate);
        l.tick();
        CHECK(up.relaunched == 1 && l.quit());
    }
    {
        // Not newer, or a development build: nothing offered.
        Launcher same(s, up);
        same.set_version("0.10.0");
        same.start("");
        same.tick();
        CHECK(!has_button(same, kUpdate));
        const int before = up.checks;
        Launcher dev(s, up);
        dev.set_version("7de9f5d-dirty");
        dev.start("");
        dev.tick();
        CHECK(!has_button(dev, kUpdate) && up.checks == before);
    }
    {
        // A failed install says why and opens the release page; a platform that cannot install opens it at once.
        up.install_ok = false;
        Launcher l(s, up);
        l.set_version("0.9.2");
        l.start("");
        l.tick();
        l.activate(kUpdate);
        l.tick();
        CHECK(l.screen() == Screen::Message && l.message().find("disk full") != std::string::npos);
        CHECK(up.opened_url == "https://example.test/release" && !l.quit());
        up.installs = false;
        up.opened_url.clear();
        l.activate(kOk);
        l.activate(kUpdate);
        CHECK(up.opened_url == "https://example.test/release" && l.screen() == Screen::Main);
    }
    {
        // An archive anywhere but the repository's releases is refused.
        up.installs = true;
        up.install_ok = true;
        up.offered.url = "https://evil.example/Game-macOS.zip";
        Launcher l(s, up);
        l.set_version("0.9.2");
        l.start("");
        l.tick();
        l.activate(kUpdate);
        CHECK(l.screen() == Screen::Message && l.message().find("evil.example") != std::string::npos);
        CHECK(!l.quit() && up.relaunched == 1);
    }
    {
        // Uninstall asks twice, then the platform removes everything and the launcher quits.
        write_file(up.pi.profile_dir + "/saves/one.sav", "save");
        Launcher l(s, up);
        l.start("");
        l.activate(kManage);
        l.activate(kUninstall);
        CHECK(exists(up.pi.profile_dir) && !l.quit());
        l.activate(kUninstall);
        CHECK(!exists(up.pi.profile_dir) && l.quit());
    }
}

void test_mod_order() {
    // NTFS order: upper-cased, then byte order, so '_' comes after the letters
    // and the digits before them.
    std::vector<std::string> dirs = {"Z_1_0_Armor", "ZZ_Alternative World", "S_Alternative",
                                     "Script_AutoSave", "A_QuP MiniQuests", "A_2_BaseMod",
                                     "A_1_QuestPaket", "a_0_lower", "M_10_0_X", "M_1_2H"};
    mod_load_order(&dirs);
    const std::vector<std::string> want = {"a_0_lower",       "A_1_QuestPaket", "A_2_BaseMod",
                                           "A_QuP MiniQuests", "M_10_0_X",       "M_1_2H",
                                           "Script_AutoSave", "S_Alternative",  "ZZ_Alternative World",
                                           "Z_1_0_Armor"};
    CHECK(dirs == want);
    CHECK(mod_dir_before("ZZ_A", "Z_A") && !mod_dir_before("Z_A", "ZZ_A"));
    CHECK(same_id("CM Content", "cm content") && !same_id("CM", "CMX"));
}

void test_mod_catalog() {
    const std::string root = make_mod_catalog("mod-catalog");
    const ModCatalog cat = load_mod_catalog(root);
    CHECK(cat.found);
    CHECK(cat.mods.size() == 6);
    CHECK(cat.errors.size() == 2); // Broken (no mod.json), Bad (does not parse)
    std::vector<std::string> order;
    for (const ModInfo &m : cat.mods)
        order.push_back(m.dir);
    CHECK((order == std::vector<std::string>{"A_1_Base", "B_Addon", "Script_Auto", "S_Plain",
                                             "ZZ_Late", "Z_Last"}));
    const ModInfo &base = cat.mods[size_t(cat.find("base"))];
    CHECK(base.id == "Base" && base.title == "Base Mod" && base.category == "Content");
    CHECK((base.authors == std::vector<std::string>{"Someone", "Else"}));
    CHECK(base.path == root + "/Mods/A_1_Base");
    CHECK(base.cover == root + "/Mods/A_1_Base/images/315x312.jpg");
    CHECK(base.readme == root + "/Mods/A_1_Base/Readme/readme_pl.md");
    CHECK(base.supported());
    const ModInfo &late = cat.mods[size_t(cat.find("Late"))];
    CHECK(late.cover == root + "/Mods/ZZ_Late/images/Head.jpg"); // the first .jpg
    CHECK(late.readme == root + "/Mods/ZZ_Late/Readme/readme_de.md");
    CHECK(late.category.empty());
    CHECK((cat.mods[size_t(cat.find("Addon"))].dependencies == std::vector<std::string>{"base"}));
    const ModInfo &autos = cat.mods[size_t(cat.find("Auto"))];
    CHECK(!autos.supported() && autos.script_dll == "scripts/SCRIPT_Auto.DLL");
    CHECK(cat.find("Bad") < 0 && cat.find("nothing") < 0);
    CHECK((cat.categories() ==
           std::vector<std::string>{"Content", "Gameplay", "Interface", "Uncategorized"}));
    CHECK(cat.presets.size() == 2 && cat.presets[0].name == "Empty" && cat.presets[1].name == "P1");
    CHECK(cat.presets[1].mod_ids.size() == 4);
    CHECK(!load_mod_catalog(root + "/nowhere").found);
}

void test_mod_rules() {
    const ModCatalog cat = load_mod_catalog(make_mod_catalog("mod-rules"));
    ModSelection sel;
    // Enabling pulls in dependencies.
    CHECK(mod_toggle(cat, &sel, "Addon").empty());
    CHECK(sel.enabled("Addon") && sel.enabled("BASE") && sel.ids.size() == 2);
    // A mod an enabled one needs cannot go.
    CHECK(!mod_required_by(cat, sel, "Base").empty());
    CHECK(!mod_toggle(cat, &sel, "Base").empty() && sel.enabled("Base"));
    // Incompatible with an enabled mod (declared on either side): locked.
    CHECK(mod_blocked(cat, sel, "Last").find("Addon Mod") != std::string::npos);
    CHECK(!mod_toggle(cat, &sel, "Last").empty() && !sel.enabled("Last"));
    // Not supported yet, or a dependency the catalog lacks: nothing changes.
    CHECK(!mod_toggle(cat, &sel, "Auto").empty() && !sel.enabled("Auto"));
    CHECK(mod_toggle(cat, &sel, "Plain").find("Missing") != std::string::npos);
    CHECK(!sel.enabled("Plain") && sel.ids.size() == 2);
    CHECK(mod_problems(cat, sel).empty());
    // Off again, the dependency last.
    CHECK(mod_toggle(cat, &sel, "Addon").empty() && mod_toggle(cat, &sel, "Base").empty());
    CHECK(sel.ids.empty());
    // A preset sets the selection without the checks; it only skips what
    // the catalog lacks and what the port cannot run.
    std::vector<std::string> missing, unsupported;
    mod_apply_preset(cat, &sel, cat.presets[1], &missing, &unsupported);
    CHECK(sel.preset == "P1");
    CHECK((sel.ids == std::vector<std::string>{"Last", "Addon"}));
    CHECK((missing == std::vector<std::string>{"Nope"}));
    CHECK(unsupported.size() == 1);
    const std::vector<ModProblem> problems = mod_problems(cat, sel);
    int conflicts = 0, needs = 0;
    for (const ModProblem &p : problems) {
        conflicts += p.conflict;
        needs += p.text.find("wymaga") != std::string::npos;
    }
    CHECK(conflicts == 1 && needs == 1);
    // Load order is the folders', not the preset's or the click order.
    const std::vector<const ModInfo *> active = mod_active(cat, sel);
    CHECK(active.size() == 2 && active[0]->id == "Addon" && active[1]->id == "Last");
    // A manual change keeps the preset, as Sefaris installs its files anyway.
    CHECK(mod_toggle(cat, &sel, "Base").empty() && sel.preset == "P1");
}

void test_mod_files() {
    const std::string root = make_mod_catalog("mod-files");
    const ModCatalog cat = load_mod_catalog(root);
    ModSelection sel;
    sel.ids = {"Last", "Base", "Auto"};
    sel.preset = "P \"1\"";
    const std::string file = root + "/profile/active-mods.json";
    os_mkdir((root + "/profile").c_str());
    CHECK(write_active_mods(file, cat, sel));
    CHECK(!exists(file + ".part"));
    const std::string text = read_file(file);
    CHECK(text == active_mods_json(cat, sel));
    CHECK(text.find("\"version\": 1") != std::string::npos);
    CHECK(text.find("\"preset\": \"P \\\"1\\\"\"") != std::string::npos);
    // In load order, with absolute paths; the unsupported one is left out.
    const size_t b = text.find("{\"id\": \"Base\", \"path\": \"" + root + "/Mods/A_1_Base\"}");
    const size_t l = text.find("{\"id\": \"Last\", \"path\": \"" + root + "/Mods/Z_Last\"}");
    CHECK(b != std::string::npos && l != std::string::npos && b < l);
    CHECK(text.find("Auto") == std::string::npos);
    sel.preset.clear();
    sel.ids.clear();
    CHECK(active_mods_json(cat, sel) == "{\n  \"version\": 1,\n  \"preset\": null,\n  \"mods\": []\n}\n");

    // The choice between runs.
    const std::string choice = root + "/profile/launcher-mods.json";
    ModSelection saved;
    saved.ids = {"Base", "Ad\\don"};
    saved.preset = "";
    CHECK(save_mod_choice(choice, root + "/\"odd\"", saved));
    ModSelection back;
    back.preset = "stale";
    std::string back_root;
    CHECK(load_mod_choice(choice, &back_root, &back));
    CHECK(back_root == root + "/\"odd\"" && back.ids == saved.ids && back.preset.empty());
    CHECK(!load_mod_choice(root + "/profile/none.json", &back_root, &back));
}

void test_mod_text() {
    CHECK(ascii_text("Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87 g\xC4\x99\xC5\x9Bl\xC4\x85 ja\xC5\xBA\xC5\x84") ==
          "Zazolc gesla jazn");
    CHECK(ascii_text("\xE2\x80\xA2\t\xE2\x80\x9Ex\xE2\x80\x9D \xE2\x80\x93 \xC3\x9C" "ber \xC3\xA9") ==
          "- \"x\" - Ueber e");
    CHECK(ascii_text("\xE4\xB8\xAD") == "?");
    const std::vector<ReadmeLine> r = simplify_markdown(
        "\xEF\xBB\xBF# Tytul\r\n\r\n\r\nTekst **gruby** i [link](http://x) ![obraz](a.png)\n"
        "- jeden\n\xE2\x80\xA2\tdwa<br>trzy\n## Dalej\n`kod`");
    CHECK(r.size() == 8);
    if (r.size() == 8) {
        CHECK(r[0].heading && r[0].text == "Tytul");
        CHECK(r[1].text.empty());
        CHECK(r[2].text.find("Tekst gruby i link") == 0 && r[2].text.find("obraz") == std::string::npos);
        CHECK(r[3].text == "- jeden" && r[4].text == "- dwa" && r[5].text == "trzy");
        CHECK(r[6].heading && r[6].text == "Dalej" && r[7].text == "kod");
    }
    const std::vector<ReadmeLine> tail = simplify_markdown("raz\n\n\n");
    CHECK(tail.size() == 1);

    // A JPEG's size from its frame header, past an APP0 segment.
    const uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x4A, 0x46, 0xFF, 0xC2,
                            0x00, 0x11, 0x08, 0x01, 0x38, 0x01, 0x3B, 0x03};
    int w = 0, h = 0;
    CHECK(jpeg_size(jpeg, sizeof jpeg, &w, &h) && w == 315 && h == 312);
    CHECK(!jpeg_size(jpeg + 2, sizeof jpeg - 2, &w, &h));
    // A cover is cropped to the tile's aspect and averaged down.
    Image src;
    src.w = 4;
    src.h = 2;
    for (int x = 0; x < 8; ++x) {
        const uint8_t v = uint8_t((x % 4) * 40);
        src.rgba.insert(src.rgba.end(), {v, v, v, 255});
    }
    const Image out = scale_cover(src, 1, 1); // the middle 2x2: values 40 and 80
    CHECK(out.w == 1 && out.h == 1 && out.rgba.size() == 4 && out.rgba[0] == 60);
}

// A platform that "decodes" every cover to a flat colour.
struct CoverPlatform : FakePlatform {
    int decoded = 0;
    bool decode_image(const std::string &, Image *out) override {
        ++decoded;
        out->w = 8;
        out->h = 8;
        out->rgba.assign(8 * 8 * 4, 200);
        return true;
    }
};

const Button *find_button(Launcher &l, int id) {
    for (const Button &b : l.buttons())
        if (b.id == id)
            return &b;
    return nullptr;
}

void test_mod_screen() {
    const std::string root = make_mod_catalog("mod-screen");
    Spec s = spec();
    s.mod_root = root;
    CoverPlatform fp;
    fp.pi.plays_in_place = true;
    fp.pi.profile_dir = root + "/profile";
    fp.pi.import_root = root + "/profile/game";
    fp.pi.folders_file = root + "/profile/launcher-folders.txt";
    fp.pi.cache_file = root + "/profile/launcher-verified.txt";
    os_mkdir(fp.pi.profile_dir.c_str());
    make_install(root + "/game");
    const std::string active = fp.pi.profile_dir + "/active-mods.json";
    {
        // A game without a catalog shows no mod screen.
        Spec plain = spec();
        Launcher l(plain, fp);
        l.start(root + "/game");
        CHECK(!has_button(l, kMods));
        l.activate(kPlay);
        CHECK(l.finished() && !exists(active)); // nothing chosen, nothing written
    }
    {
        // A profile that never chose starts with the game's defaults, minus what the catalog lacks.
        Spec d = s;
        d.default_mods = {"Late", "Not In This Catalog"};
        Launcher l(d, fp);
        l.start(root + "/game");
        l.activate(kMods);
        CHECK(l.mod_selection().enabled("Late") &&
              !l.mod_selection().enabled("Not In This Catalog"));
        CHECK(l.mod_selection().ids.size() == 1);
        CHECK(!exists(fp.pi.profile_dir +
                      "/launcher-mods.json")); // still not the player's choice on disk
        CHECK(!has_button(l, kModsGet)); // no mods_url, no button
        d.mods_url = "https://example.test/mods";
        Launcher with_url(d, fp);
        with_url.start(root + "/game");
        with_url.activate(kMods);
        with_url.activate(kModsGet);
        CHECK(fp.opened_url == "https://example.test/mods");
    }
    Launcher l(s, fp);
    l.start(root + "/game");
    CHECK(l.status().state == State::Ready && has_button(l, kMods));
    l.activate(kMods);
    CHECK(l.screen() == Screen::Mods && l.mod_catalog().mods.size() == 6);
    CHECK(has_button(l, kModsPlay) && has_button(l, kModsPresetPick) && has_button(l, kModToggle));
    Canvas c;
    c.resize(2560, 1600, true);
    l.draw(c, 2);
    CHECK(fp.decoded >= 2); // the covers in view: Base and Late have one
    // A click on a card's switch switches the mod; on the rest of it, selects it.
    const int base = l.mod_catalog().find("Base");
    const Button *tile = find_button(l, kModTile + base);
    CHECK(tile && tile->rect.w > 0);
    if (tile) {
        const Rect r = tile->rect;
        l.pointer(r.x + r.w / 2, r.y + r.h - 2, true);
        l.pointer(r.x + r.w / 2, r.y + r.h - 2, false);
        CHECK(l.mod_selected() == base && !l.mod_selection().enabled("Base"));
        l.draw(c, 2);
        tile = find_button(l, kModTile + base);
        const Rect sw = l.tile_toggle(tile->rect);
        CHECK(sw.y > tile->rect.y + tile->rect.w / 2); // under the cover, not on it
        l.pointer(sw.x + sw.w / 2, sw.y + sw.h / 2, true);
        l.pointer(sw.x + sw.w / 2, sw.y + sw.h / 2, false);
        CHECK(l.mod_selection().enabled("Base"));
    }
    // The category filter and scrolling keep working on a small canvas.
    l.activate(kModsCategory + 1); // enabled only
    l.draw(c, 2);
    int tiles = 0;
    for (const Button &b : l.buttons())
        tiles += b.id >= kModTile;
    CHECK(tiles == 1);
    l.activate(kModsCategory);
    c.resize(640, 400, true);
    l.draw(c, 1);
    l.wheel(c.width() / 3, c.height() / 2, 1);
    l.advance(1.0);
    l.draw(c, 1);
    for (const Button &b : l.buttons())
        CHECK((b.rect.x >= 0 && b.rect.x + b.rect.w <= c.width()) || b.id >= kModTile);
    // Typing into the search field filters the cards; Esc clears it.
    l.activate(kModsSearch);
    CHECK(l.wants_text());
    l.type_text("lat");
    tiles = 0;
    for (const Button &b : l.buttons())
        tiles += b.id >= kModTile;
    CHECK(tiles == 1);
    l.key(Key::Back);
    CHECK(!l.wants_text() && l.screen() == Screen::Mods);
    // A preset with a conflict: Play asks once more, then writes the file.
    int p1 = -1;
    for (size_t i = 0; i < l.mod_catalog().presets.size(); ++i)
        if (l.mod_catalog().presets[i].name == "P1")
            p1 = int(i);
    CHECK(p1 >= 0);
    l.activate(kModsPresetPick + p1);
    CHECK(l.mod_selection().preset == "P1");
    CHECK(l.mod_notice().find("Nope") != std::string::npos);
    l.activate(kModsPlay);
    CHECK(!l.finished() && !exists(active));
    l.activate(kModsPlay);
    CHECK(l.finished() && exists(active));
    CHECK(read_file(active).find("\"preset\": \"P1\"") != std::string::npos);
    CHECK(exists(fp.pi.profile_dir + "/launcher-mods.json"));
    // The next start remembers it, and Play on the main screen writes it again.
    os_unlink(active.c_str());
    Launcher again(s, fp);
    again.start(root + "/game");
    CHECK(again.mod_selection().preset == "P1" && again.mod_selection().ids.size() == 2);
    again.activate(kMods);
    again.key(Key::Back);
    CHECK(again.screen() == Screen::Main);
    again.activate(kPlay);
    CHECK(again.finished() && read_file(active).find("Z_Last") != std::string::npos);
}

// tools/g3mods.py selftest, the same tree and the same expectations.
void test_mod_view() {
    const std::string t = fresh("mod-view");
    const std::string game = t + "/game/Data";
    for (const char *n : {"_compiledImage.pak", "_compiledMesh.pak", "_compiledMesh.p00", "Infos.pak",
                          "Infos.mod", "Infos.m00"})
        write_file(game + "/" + n, "");
    // UTF-16LE of ASCII text, with or without a BOM.
    auto u16 = [](const std::string &s, bool bom) {
        std::string out = bom ? std::string("\xff\xfe", 2) : std::string();
        for (char c : s) {
            out.push_back(c);
            out.push_back('\0');
        }
        return out;
    };
    // The game's string table as CP 1.75's Strings.pak holds it: BOM, revisions header last.
    const std::string base_table = u16("[LocAdmin_Strings]\r\nK=g\r\n[LocAdmin_Revisions]\r\n", true);
    CHECK(g3pak_write(game + "/Strings.pak", {{"stringtable.ini", &base_table}}, 0));
    const std::string a = t + "/A/ModFiles", b = t + "/B/ModFiles";
    for (const char *n : {"_compiledImage.m0x", "_compiledMesh.m0x", "Infos.m0x", "readme.txt",
                          "Projects_compiled.p0x", "ge3.ini", "Camera.ini", "stringtableExpress.ini"})
        write_file(a + "/" + n, "a");
    write_file(a + "/stringtable.ini", u16("[LocAdmin_Strings]\r\nA=a", true)); // no last line end
    write_file(a + "/scripts/Script_X.dll", "MZ");
    write_file(b + "/Static/G3_World_01.wrldatasc", "[Sector.List]\r\nSector=b\r\n");
    write_file(b + "/StringTable.ini", u16("[LocAdmin_Strings]\r\nB=b\r\n", true));
    for (const char *n : {"_compiledImage.m0x", "_CompiledImage.m01", "Templates.nod", "camera.ini"})
        write_file(b + "/" + n, "b");
    const std::string prof = t + "/profile";
    os_mkdir(prof.c_str());
    const std::string contract = prof + "/active-mods.json";
    write_file(contract, "{\"version\": 1, \"preset\": null, \"mods\": [{\"id\": \"a\", \"path\": \"" + t +
                             "/A\"}, {\"id\": \"missing\", \"path\": \"" + t +
                             "/nope\"}, {\"id\": \"b\", \"path\": \"" + t + "/B\"}]}");
    ModView rec = build_mod_view(prof, game);
    auto name_of = [](const std::string &p) { return p.substr(p.find_last_of('/') + 1); };
    std::vector<std::string> got;
    for (const ModViewLink &l : rec.links)
        got.push_back(l.path + " " + l.mod + " " + name_of(l.target));
    const std::vector<std::string> want = {
        "Data/_compiledimage.mod a _compiledImage.m0x", "Data/_compiledimage.m00 b _CompiledImage.m01",
        "Data/_compiledimage.m01 b _compiledImage.m0x", "Data/_compiledmesh.mod a _compiledMesh.m0x",
        "Data/infos.m01 a Infos.m0x",                   "Data/strings.mod (merged) strings.mod",
        "Data/templates.nod b Templates.nod",           "ini/camera.ini b camera.ini",
    };
    CHECK(got == want);
    if (got != want)
        for (const std::string &g : got)
            fprintf(stderr, "  got %s\n", g.c_str());
    for (const ModViewLink &l : rec.links) {
        OsStat st{};
        CHECK(os_lstat((prof + "/" + l.path).c_str(), &st) == 0 && st.is_symlink);
        CHECK(read_file(prof + "/" + l.path) == read_file(l.target));
    }
    std::vector<std::string> skipped;
    for (const ModViewSkip &s : rec.skipped)
        skipped.push_back(name_of(s.file));
    std::sort(skipped.begin(), skipped.end());
    CHECK((skipped == std::vector<std::string>{"", "G3_World_01.wrldatasc", "Projects_compiled.p0x",
                                               "Script_X.dll", "ge3.ini", "readme.txt",
                                               "stringtableExpress.ini"})); // "" is "Static/"
    CHECK(rec.errors.size() == 1 && rec.errors[0].find("missing") != std::string::npos);
    const std::string record = read_file(prof + "/mods-view.json");
    CHECK(record == mod_view_json(rec));
    CHECK(record.find("\"links\": [") != std::string::npos &&
          record.find("\"target\": \"") != std::string::npos);
    std::string merged;
    CHECK(g3pak_read(prof + "/Data/strings.mod", "STRINGTABLE.INI", &merged));
    CHECK(merged == u16("[LocAdmin_Strings]\r\nK=g\r\n\r\n\r\nA=a\r\n\r\nB=b\r\n[LocAdmin_Revisions]\r\n", true));
    // The same inputs again: strings.mod is not rewritten.
    const std::string strings_mod = prof + "/mods-generated/strings.mod";
    const std::string packed = read_file(strings_mod);
    write_file(strings_mod, "kept");
    build_mod_view(prof, game);
    CHECK(read_file(strings_mod) == "kept");
    write_file(strings_mod, packed);
    // With the catalog's Static/: sectors merged and packed, the cache-hiding archives last.
    const std::string stat = t + "/Static";
    write_file(stat + "/G3_World_01.wrldatasc", "[Sector.List]\r\nSector=base\r\n");
    CHECK(g3pak_write(stat + "/Projects_compiled.m0x", {{"stringtable.bin", nullptr}}, 0));
    CHECK(g3pak_write(stat + "/Projects_compiled.n0x", {{"stringtable.bin", nullptr}}, 0));
    write_file(b + "/Projects_compiled.m0x", "b");
    write_file(contract, "{\"version\": 1, \"preset\": null, \"static\": \"" + stat +
                             "\", \"mods\": [{\"id\": \"a\", \"path\": \"" + t +
                             "/A\"}, {\"id\": \"missing\", \"path\": \"" + t +
                             "/nope\"}, {\"id\": \"b\", \"path\": \"" + t + "/B\"}]}");
    rec = build_mod_view(prof, game);
    got.clear();
    for (const ModViewLink &l : rec.links)
        if (l.path.find("projects") != std::string::npos)
            got.push_back(l.path + " " + l.mod + " " + name_of(l.target));
    CHECK((got == std::vector<std::string>{"Data/projects_compiled.mod b Projects_compiled.m0x",
                                           "Data/projects_compiled.m00 (merged) projects_compiled.mod",
                                           "Data/projects_compiled.m01 (static) Projects_compiled.m0x",
                                           "Data/projects_compiled.nod (static) Projects_compiled.n0x"}));
    std::string sectors;
    CHECK(g3pak_read(prof + "/Data/projects_compiled.m00", "g3_world_01/g3_world_01.wrldatasc", &sectors));
    CHECK(sectors == "[Sector.List]\r\nSector=base\r\nSector=b\r\n");
    CHECK(rec.errors.size() == 1);
    os_unlink((b + "/Projects_compiled.m0x").c_str());
    write_file(contract, "{\"version\": 1, \"preset\": null, \"mods\": [{\"id\": \"a\", \"path\": \"" + t +
                             "/A\"}, {\"id\": \"missing\", \"path\": \"" + t +
                             "/nope\"}, {\"id\": \"b\", \"path\": \"" + t + "/B\"}]}");
    // A regular file in the way stays and is reported; a rebuild replaces only links.
    os_unlink((prof + "/Data/_compiledimage.mod").c_str());
    write_file(prof + "/Data/_compiledimage.mod", "player");
    rec = build_mod_view(prof, game);
    CHECK(read_file(prof + "/Data/_compiledimage.mod") == "player");
    bool reported = false;
    for (const std::string &e : rec.errors)
        reported = reported || e.find("not a link") != std::string::npos;
    CHECK(reported);
    // No contract: the view goes away, the player's file stays.
    os_unlink(contract.c_str());
    build_mod_view(prof, game);
    CHECK(!exists(prof + "/Data/_compiledmesh.mod"));
    CHECK(read_file(prof + "/Data/_compiledimage.mod") == "player");
    CHECK(!exists(prof + "/mods-view.json") && !exists(prof + "/mods-generated"));
    // A profile that never had a view gets no record either.
    const std::string empty = t + "/fresh";
    os_mkdir(empty.c_str());
    build_mod_view(empty, game);
    int entries = 0;
    os_listdir(
        empty.c_str(),
        [](const char *n, void *user) -> int {
            if (strcmp(n, ".") && strcmp(n, ".."))
                ++*static_cast<int *>(user);
            return 0;
        },
        &entries);
    CHECK(entries == 0);
    // A contract that is not version 1 is an error, nothing linked.
    write_file(contract, "{\"version\": 2, \"mods\": []}");
    rec = plan_mod_view(prof, game);
    CHECK(rec.links.empty() && rec.errors.size() == 1);
}
} // namespace

int main(int argc, char **argv) {
    // For comparing with tools/g3mods.py on a real catalog:
    //   launcher_tests --active <catalog> <preset> <profile>   writes <profile>/active-mods.json
    //   launcher_tests --plan-view <profile> <game Data>       prints what build would link
    //   launcher_tests --build-view <profile> <game Data>      builds it and prints the record
    if (argc == 5 && !strcmp(argv[1], "--active")) {
        const ModCatalog cat = load_mod_catalog(argv[2]);
        ModSelection sel;
        for (const ModPreset &p : cat.presets)
            if (p.name == argv[3])
                mod_apply_preset(cat, &sel, p, nullptr, nullptr);
        return write_active_mods(std::string(argv[4]) + "/active-mods.json", cat, sel) ? 0 : 1;
    }
    if (argc == 4 && !strcmp(argv[1], "--plan-view")) {
        fputs(mod_view_json(plan_mod_view(argv[2], argv[3])).c_str(), stdout);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "--build-view")) {
        fputs(mod_view_json(build_mod_view(argv[2], argv[3])).c_str(), stdout);
        return 0;
    }
    // launcher_tests --stage <game folder> <spec>: stage_modules over a real install, one module a line
    // (name, dest, from, from_sha256, prepare, base in hex, sha256; tab separated, "-" for no prepare).
    if (argc == 4 && !strcmp(argv[1], "--stage")) {
        std::vector<StageModule> mods;
        std::istringstream lines(read_file(argv[3]));
        for (std::string line; std::getline(lines, line);) {
            std::vector<std::string> f;
            std::istringstream cols(line);
            for (std::string c; std::getline(cols, c, '\t');)
                f.push_back(c);
            if (f.size() != 7)
                continue;
            mods.push_back({f[0], f[1], f[2], f[3], f[4] == "-" ? "" : f[4],
                            uint32_t(strtoul(f[5].c_str(), nullptr, 16)), f[6]});
        }
        const StageOutcome out =
            stage_modules(argv[2], mods, [](size_t done, size_t total, const std::string &n) {
                printf("%zu/%zu %s\n", done, total, n.c_str());
            });
        printf("%s: %d made, %d kept%s%s\n", out.ok ? "ok" : "FAILED", out.made, out.kept, out.ok ? "" : ": ",
               out.error.c_str());
        return out.ok && modules_staged(argv[2], mods) ? 0 : 1;
    }
    // launcher_tests --ini <game folder> <edit>...: apply_ini_edits over real files.
    if (argc >= 4 && !strcmp(argv[1], "--ini")) {
        std::string why;
        const bool ok = apply_ini_edits(argv[2], std::vector<std::string>(argv + 3, argv + argc), &why);
        printf("%s%s\n", ok ? "ok" : "FAILED: ", why.c_str());
        return ok ? 0 : 1;
    }
    const char *base = getenv("RECOMP_LAUNCHER_TEST_DIR");
    g_scratch = std::string(base && *base ? base : "build/recomp") + "/launcher-test";
    // Absolute: active-mods.json names mod folders by absolute path.
    char cwd[4096];
    if (g_scratch[0] != '/' && os_getcwd(cwd, sizeof cwd) == 0)
        g_scratch = std::string(cwd) + "/" + g_scratch;
    for (size_t i = g_scratch.find('/'); i != std::string::npos; i = g_scratch.find('/', i + 1))
        os_mkdir(g_scratch.substr(0, i).c_str());
    remove_tree(g_scratch);
    os_mkdir(g_scratch.c_str());
    write_file(g_scratch + "/exe-bytes", kExeBytes);

    struct Test {
        const char *name;
        void (*run)();
    } tests[] = {
        {"sha256", test_sha256},
        {"wildcards and exclusion", test_wildcards},
        {"find and check", test_find_and_check},
        {"folder import", test_folder_import},
        {"cancel and resume", test_cancel_and_resume},
        {"move import", test_move_import},
        {"failures", test_failures},
        {"zip import", test_zip_import},
        {"folders and profile", test_folders_and_profile},
        {"detection", test_detection},
        {"launcher screen, desktop", test_ui_desktop},
        {"launcher screen, mobile", test_ui_mobile},
        {"launcher text fits", test_ui_text_fits},
        {"truetype text", test_text},
        {"mod load order", test_mod_order},
        {"mod catalog", test_mod_catalog},
        {"mod rules", test_mod_rules},
        {"mod files", test_mod_files},
        {"mod text and covers", test_mod_text},
        {"mod screen", test_mod_screen},
        {"mod view (g3mods.py)", test_mod_view},
        {"steam output", test_steam_output},
        {"steam arguments", test_steam_arguments},
        {"steam saved accounts", test_steam_accounts},
        {"stage modules", test_stage},
#ifndef _WIN32
        {"steam screen", test_ui_steam},
#endif
        {"updates and uninstall", test_ui_update},
    };
    for (const Test &t : tests) {
        const int before = g_failures;
        t.run();
        printf("%-28s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }
    printf("%d checks, %d failures\n", g_checks, g_failures);
    remove_tree(g_scratch);
    return g_failures ? 1 : 0;
}
