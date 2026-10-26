// launcher_ui.cpp - see launcher_ui.h. The main screen and the screens that
// share its layout (manage, import, messages); the mod screen is in
// launcher_ui_mods.cpp, the look (fonts, pictures, icons) in launcher_look.cpp.
#include "launcher_ui.h"

#include "launcher_look.h"

#include "../../platform/os.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace launcher {

std::unique_ptr<Source> Platform::open(const Picked &p, std::string *error) {
    OsStat st{};
    if (os_stat(p.path.c_str(), &st) != 0) {
        if (error)
            *error = "cannot open " + p.path;
        return nullptr;
    }
    if (st.is_dir)
        return folder_source(p.path);
    return zip_source(p.path, error);
}

bool Platform::uninstall(std::string *error) {
    const PlatformInfo i = info();
    if (!i.profile_dir.empty() && !remove_tree(i.profile_dir)) {
        if (error)
            *error = "cannot remove " + i.profile_dir;
        return false;
    }
    return true;
}

void Platform::run_in_background(std::function<void()> work) {
    auto *heap = new std::function<void()>(std::move(work));
    OsThread *t = os_thread_create(
        [](void *arg) -> void * {
            auto *fn = static_cast<std::function<void()> *>(arg);
            (*fn)();
            delete fn;
            return nullptr;
        },
        heap, 0);
    if (t)
        os_thread_detach(t);
    else {
        (*heap)();
        delete heap;
    }
}

std::string human_bytes(uint64_t bytes) {
    char buf[32];
    if (bytes >= (uint64_t(1) << 30))
        snprintf(buf, sizeof buf, "%.1f GB", double(bytes) / double(uint64_t(1) << 30));
    else if (bytes >= (uint64_t(1) << 20))
        snprintf(buf, sizeof buf, "%.0f MB", double(bytes) / double(uint64_t(1) << 20));
    else
        snprintf(buf, sizeof buf, "%.0f KB", double(bytes) / 1024.0);
    return buf;
}

namespace {
// A path as the player would say it: under the home folder as "~/...".
std::string display(const std::string &path) {
    const char *env = getenv("HOME");
    if (!env || strlen(env) < 2)
        return path;
    // iOS spells the same container /private/var/... and /var/....
    std::string home = env;
    const std::vector<std::string> spellings = {
        home, home.compare(0, 9, "/private/") == 0 ? home.substr(8) : "/private" + home};
    for (const std::string &h : spellings)
        if (path.compare(0, h.size(), h) == 0 && (path.size() == h.size() || path[h.size()] == '/'))
            return "~" + path.substr(h.size());
    return path;
}

std::string upper_utf8(const std::string &s) {
    // ASCII and the Polish letters, which is what the launcher's labels hold.
    static const char *const pairs[][2] = {{"ą", "Ą"}, {"ć", "Ć"}, {"ę", "Ę"}, {"ł", "Ł"}, {"ń", "Ń"},
                                           {"ó", "Ó"}, {"ś", "Ś"}, {"ź", "Ź"}, {"ż", "Ż"}};
    std::string out;
    for (size_t i = 0; i < s.size();) {
        bool done = false;
        for (const auto &p : pairs)
            if (s.compare(i, strlen(p[0]), p[0]) == 0) {
                out += p[1];
                i += strlen(p[0]);
                done = true;
                break;
            }
        if (!done)
            out += char(toupper((unsigned char)s[i++]));
    }
    return out;
}

constexpr float kAnimSeconds = 0.15f;
} // namespace

Launcher::Launcher(const Spec &spec, Platform &platform)
    : spec_(spec), platform_(platform), shared_(std::make_shared<Shared>()),
      cancel_(std::make_shared<std::atomic<bool>>(false)) {
    look_ = std::make_unique<Look>();
    info_ = platform_.info();
    // Modules made from the player's files (Gothic 3) are made in the app's own copy: an install
    // elsewhere (Wine, CrossOver, a copied folder) is something to import, never played where it is.
    if (!spec_.stage.empty())
        info_.plays_in_place = false;
    if (!spec_.mod_root.empty()) {
        std::string root;
        have_choice_ = load_mod_choice(choice_file(), &root, &sel_);
        mod_root_ = root.empty() ? default_mod_root(spec_.mod_root) : root;
        // A profile that never chose starts with the game's default mods ([launcher] default_mods).
        if (!have_choice_ && !spec_.default_mods.empty()) {
            sel_.ids = spec_.default_mods;
            have_choice_ = default_choice_ = true;
        }
    }
}

Launcher::~Launcher() {
    cancel_->store(true);
}

void Launcher::post(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(shared_->m);
    shared_->posted.push_back(std::move(fn));
}

void Launcher::evaluate(const std::string &root) {
    status_ = check(spec_, root, info_.plays_in_place ? info_.cache_file : "");
    if (status_.state == State::Ready && !playable_here(status_.root)) {
        status_.state = State::Incomplete;
        status_.missing = {L("the prepared modules", "przygotowane moduły")};
    }
    dirty_ = true;
}

void Launcher::mark_started() {
    if (info_.profile_dir.empty())
        return;
    const std::string marker = info_.profile_dir + "/launcher-started";
    OsStat st{};
    first_start_ = os_stat(marker.c_str(), &st) != 0;
    if (first_start_)
        if (FILE *f = fopen(marker.c_str(), "wb"))
            fclose(f);
}

bool Launcher::playable_here(const std::string &root) const {
    return spec_.stage.empty() || (root == info_.import_root && modules_staged(root, spec_.stage));
}

void Launcher::start(const std::string &known) {
    check_for_update();
    Status best;
    auto consider = [&](const std::string &root) {
        if (root.empty() || best.state == State::Ready)
            return;
        Status s = check(spec_, root, info_.plays_in_place ? info_.cache_file : "");
        if (s.state == State::Ready && !playable_here(s.root)) {
            s.state = State::Incomplete;
            s.missing = {L("the prepared modules", "przygotowane moduły")};
        }
        if (s.state == State::Ready ||
            (best.state == State::NotFound && s.state != State::NotFound))
            best = s;
    };
    consider(find_root(spec_, known));
    consider(info_.import_root);
    if (info_.plays_in_place)
        for (const std::string &f : load_folders(info_.folders_file))
            consider(f);
    found_.clear();
    for (const std::string &c : platform_.candidates(spec_))
        if (std::find(found_.begin(), found_.end(), c) == found_.end())
            found_.push_back(c);
    // Desktop plays a found install in place; elsewhere a found folder is only
    // something to import.
    if (info_.plays_in_place && best.state != State::Ready)
        for (const std::string &c : found_)
            consider(c);
    status_ = best;
    screen_ = Screen::Main;
    rebuild();
    Picked opened;
    if (platform_.initial_pick(&opened)) {
        begin_import(opened);
        return;
    }
    if (status_.state == State::NotFound && !info_.auto_import.empty() &&
        !find_root(spec_, info_.auto_import).empty()) {
        Picked p;
        p.path = info_.auto_import;
        p.name = "the bundled game";
        begin_import(p);
        return;
    }
    // No game here: it comes from the player's Steam account at once.
    if (status_.state == State::NotFound && steam_offered()) {
        begin_steam();
        return;
    }
    // The app's own copy, downloaded or imported, whose preparing stopped halfway: finish it.
    if (status_.state == State::Incomplete && !spec_.stage.empty() &&
        status_.root == info_.import_root && check(spec_, status_.root).state == State::Ready)
        prepare_then_play(steam_edits());
}

void Launcher::set_unplayable(const std::string &reason) {
    unplayable_ = reason;
    countdown_ = 0;
    dirty_ = true;
}

void Launcher::set_auto_play(double seconds) {
    if (!unplayable_.empty())
        return;
    countdown_ = status_.state == State::Ready && screen_ == Screen::Main ? seconds : 0;
    dirty_ = true;
}

void Launcher::advance(double seconds) {
    // Highlights fade towards what the pointer and the focus say; scrolling glides.
    const float step = float(seconds) / kAnimSeconds;
    for (size_t i = 0; i < buttons_.size(); ++i) {
        const Button &b = buttons_[i];
        const bool on = int(i) == hover_ || int(i) == pressed_ ||
                        (keyboard_ && int(i) == focus_ && b.kind != kKindTile);
        float &v = anim_[b.id];
        const float target = on ? 1.0f : 0.0f;
        if (v != target) {
            v = v < target ? std::min(target, v + step) : std::max(target, v - step);
            dirty_ = true;
        }
    }
    const float k = 1.0f - std::exp(-float(seconds) * 16.0f);
    for (auto *pair : {&grid_scroll_, &detail_scroll_}) {
        float &pos = *pair;
        const float target = pair == &grid_scroll_ ? grid_target_ : detail_target_;
        if (pos != target) {
            pos += (target - pos) * k;
            if (std::fabs(target - pos) < 0.5f)
                pos = target;
            dirty_ = true;
        }
    }
    if (countdown_ <= 0)
        return;
    const int before = int(countdown_ + 0.999);
    countdown_ -= seconds;
    if (countdown_ <= 0) {
        countdown_ = 0;
        activate(kPlay);
    } else if (int(countdown_ + 0.999) != before)
        dirty_ = true;
}

bool Launcher::animating() const {
    if (grid_scroll_ != grid_target_ || detail_scroll_ != detail_target_)
        return true;
    for (size_t i = 0; i < buttons_.size(); ++i) {
        const Button &b = buttons_[i];
        const bool on = int(i) == hover_ || int(i) == pressed_ ||
                        (keyboard_ && int(i) == focus_ && b.kind != kKindTile);
        auto it = anim_.find(b.id);
        const float v = it == anim_.end() ? 0.0f : it->second;
        if (v != (on ? 1.0f : 0.0f))
            return true;
    }
    return false;
}

float Launcher::hot(int index) const {
    if (index < 0 || index >= int(buttons_.size()))
        return 0;
    auto it = anim_.find(buttons_[size_t(index)].id);
    return it == anim_.end() ? 0.0f : it->second;
}

bool Launcher::focus_visible(int index) const {
    return keyboard_ && !info_.touch && index == focus_;
}

void Launcher::drop(const std::string &path) {
    countdown_ = 0;
    if (screen_ == Screen::Importing)
        return;
    OsStat st{};
    if (os_stat(path.c_str(), &st) != 0)
        return;
    Picked p;
    p.path = path;
    if (st.is_dir && info_.plays_in_place) {
        const std::string root = find_root(spec_, path);
        if (root.empty()) {
            show(L("No ", "Nie ma ") + spec_.executable + L(" in ", " w ") + path +
                 L(" or the two folder levels below it.", " ani dwa poziomy folderów niżej."));
            return;
        }
        evaluate(root);
        if (status_.state == State::Ready)
            remember_folder(info_.folders_file, root);
        screen_ = Screen::Main;
        rebuild();
        return;
    }
    begin_import(p);
}

void Launcher::show(const std::string &text) {
    message_ = text;
    screen_ = Screen::Message;
    rebuild();
}

void Launcher::rebuild() {
    buttons_.clear();
    auto add = [&](int id, const std::string &label, bool enabled = true, int kind = kKindNormal,
                   Icon icon = Icon(-1)) {
        Button b{id, label, enabled, {}};
        b.kind = kind;
        b.icon = int(icon);
        buttons_.push_back(b);
    };
    switch (screen_) {
    case Screen::Main: {
        const bool ready = status_.state == State::Ready;
        if (ready)
            add(kPlay, L("Play", "Uruchom grę"), true, kKindPrimary, Icon::Play);
        if (!ready && steam_offered())
            add(kSteam, L("Download from Steam", "Pobierz ze Steama"), true, kKindPrimary,
                Icon::Play);
        if (info_.plays_in_place) {
            if (!ready)
                add(kLocate, L("Locate game folder…", "Wskaż folder gry…"), info_.can_pick_folder,
                    steam_offered() ? kKindBar : kKindPrimary, Icon::Folder);
        } else if (!(ready && steam_offered())) { // a downloaded game needs no import
            const bool have = status_.state != State::NotFound;
            if (info_.can_pick_folder)
                add(kImportFolder,
                    have ? L("Import again from a folder…", "Importuj ponownie z folderu…")
                         : L("Import game folder…", "Importuj folder gry…"),
                    true, have || steam_offered() ? kKindNormal : kKindPrimary, Icon::Folder);
            if (info_.can_pick_zip)
                add(kImportZip,
                    have ? L("Import again from a ZIP…", "Importuj ponownie z ZIP…")
                         : L("Import game ZIP…", "Importuj ZIP z grą…"),
                    true, kKindNormal, Icon::Document);
        }
        if (update_known_)
            add(kUpdate, L("Update to ", "Aktualizuj do ") + update_.version, true, kKindNormal,
                Icon::Check);
        if (!spec_.mod_root.empty())
            add(kMods, L("Mods", "Mody"), true, kKindNormal, Icon::Puzzle);
        add(kQuit, L("Quit", "Wyjdź"), true, kKindNormal, Icon::Exit);
        if (info_.plays_in_place) {
            if (ready && info_.can_open_folder)
                add(kOpenFolder, L("Show in Finder", "Pokaż w Finderze"), true, kKindIcon, Icon::Folder);
            if (ready)
                add(kLocate, L("Change folder…", "Zmień folder…"), info_.can_pick_folder, kKindBar,
                    Icon::Folder);
            for (size_t i = 0; i < found_.size() && i < 4; ++i)
                if (found_[i] != status_.root)
                    add(kUseCandidate + int(i), L("Use ", "Użyj ") + display(found_[i]), true, kKindBar,
                        Icon::Folder);
            if (ready && status_.root != info_.import_root)
                add(kCopyIntoApp, L("Copy into the app", "Kopiuj do aplikacji"), true, kKindBar,
                    Icon::Document);
        } else {
            for (size_t i = 0; i < found_.size() && i < 3; ++i)
                if (found_[i] != status_.root)
                    add(kUseCandidate + int(i), L("Import ", "Importuj ") + display(found_[i]), true,
                        kKindBar, Icon::Folder);
        }
        add(kManage, L("Manage files…", "Zarządzaj plikami…"), true, kKindBar, Icon::Database);
        if (!spec_.store_url.empty() && status_.state == State::NotFound)
            add(kStore, L("Where to get the game", "Skąd wziąć grę"), true, kKindBar, Icon::Document);
        break;
    }
    case Screen::Manage:
        add(kExportSaves, L("Export saves…", "Eksportuj zapisy…"), true, kKindNormal, Icon::Document);
        add(kImportSaves, L("Import saves…", "Importuj zapisy…"), true, kKindNormal, Icon::Document);
        if (!info_.import_root.empty() && status_.root == info_.import_root)
            add(kDeleteData,
                confirm_delete_ ? L("Delete game data - press again", "Usuń dane gry: naciśnij jeszcze raz")
                                : L("Delete game data", "Usuń dane gry"),
                true, kKindNormal, Icon::Cross);
        if (info_.can_open_folder && !status_.root.empty())
            add(kOpenFolder, L("Open game folder", "Otwórz folder gry"), true, kKindNormal, Icon::Folder);
        if (info_.can_open_folder)
            add(kOpenFolder + 1000, L("Open saves folder", "Otwórz folder zapisów"), true, kKindNormal,
                Icon::Folder);
        if (info_.can_open_folder && !info_.profile_dir.empty()) // NFSMW Remaster run 19 (D-132): player texture mods
            add(kOpenFolder + 2000, L("Open mods folder", "Otwórz folder modów"), true, kKindNormal,
                Icon::Folder);
        if (info_.can_open_folder && !info_.profile_dir.empty()) // NFSMW run 31: changed game files (DataMods)
            add(kOpenFolder + 3000, L("Open DataMods folder", "Otwórz folder DataMods"), true,
                kKindNormal, Icon::Folder);
        add(kUninstall,
            confirm_uninstall_ ? L("Confirm: remove everything", "Potwierdź: usuń wszystko")
                               : L("Uninstall from this Mac", "Odinstaluj z tego Maca"),
            true, kKindNormal, Icon::Cross);
        add(kBack, L("Back", "Wróć"), true, kKindNormal, Icon::Back);
        break;
    case Screen::Update:
        break;
    case Screen::Importing:
        add(kCancel, L("Cancel", "Anuluj"), true, kKindNormal, Icon::Cross);
        break;
    case Screen::Message:
        add(kOk, "OK", true, kKindNormal, Icon::Check);
        break;
    case Screen::Mods:
        mods_buttons();
        break;
    case Screen::Steam:
        steam_buttons();
        break;
    }
    focus_ = std::clamp(focus_, 0, std::max(0, int(buttons_.size()) - 1));
    if (!buttons_.empty() && !buttons_[size_t(focus_)].enabled)
        focus_ = 0;
    if (hover_ >= int(buttons_.size()))
        hover_ = -1;
    dirty_ = true;
}

const std::vector<Button> &Launcher::buttons() {
    return buttons_;
}

void Launcher::pick_then(bool folder, bool play_in_place) {
    auto weak = std::weak_ptr<Shared>(shared_);
    Launcher *self = this;
    auto done = [weak, self, play_in_place](std::vector<Picked> picked, std::string error) {
        auto shared = weak.lock();
        if (!shared)
            return;
        std::lock_guard<std::mutex> lock(shared->m);
        shared->posted.push_back([self, picked, error, play_in_place]() {
            if (picked.empty()) {
                if (!error.empty())
                    self->show(self->L("The picker failed: ", "Wybór się nie powiódł: ") + error);
                return;
            }
            if (play_in_place && !picked[0].path.empty()) {
                const std::string root = find_root(self->spec_, picked[0].path);
                if (root.empty()) {
                    self->show(self->L("No ", "Nie ma ") + self->spec_.executable +
                               self->L(" in ", " w ") + picked[0].path +
                               self->L(" or the two folder levels below it.",
                                       " ani dwa poziomy folderów niżej."));
                    return;
                }
                self->evaluate(root);
                if (self->status_.state == State::Ready)
                    remember_folder(self->info_.folders_file, root);
                self->screen_ = Screen::Main;
                self->rebuild();
                return;
            }
            self->begin_import(picked[0]);
        });
    };
    if (folder)
        platform_.pick_folder(done);
    else
        platform_.pick_zip(done);
}

void Launcher::begin_import(const Picked &picked) {
    std::string error;
    // The app's own copy is not a source: copying it onto itself proves nothing
    // and a folder that contains it would be copied into itself.
    if (!picked.path.empty() && !info_.import_root.empty()) {
        const std::string root = find_root(spec_, picked.path);
        if (root == info_.import_root) {
            show(L("That is the game this app already imported. Choose the folder the game is "
                   "installed in.",
                   "To gra, którą ta aplikacja już zaimportowała. Wskaż folder, "
                   "w którym gra jest zainstalowana."));
            return;
        }
    }
    std::shared_ptr<Source> source(platform_.open(picked, &error).release());
    if (!source) {
        show(L("Cannot read ", "Nie można odczytać ") + (picked.name.empty() ? picked.path : picked.name) +
             ": " + error);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(shared_->m);
        shared_->progress = Progress{};
        shared_->importing = true;
        shared_->import_done = false;
    }
    cancel_->store(false);
    importing_ = picked;
    screen_ = Screen::Importing;
    rebuild();
    platform_.import_activity(true, nullptr);
    const std::string dest = info_.import_root;
    auto shared = shared_;
    auto cancel = cancel_;
    Platform *platform = &platform_;
    const Spec spec = spec_;
    const bool move = platform_.movable(picked);
    const std::string leftover = move ? picked.path : std::string();
    platform_.run_in_background([shared, cancel, platform, spec, source, dest, move, leftover]() {
        ImportOutcome out = import_game(
            spec, *source, dest,
            [&](const Progress &p) {
                {
                    std::lock_guard<std::mutex> lock(shared->m);
                    shared->progress = p;
                }
                platform->import_activity(true, &p);
                return !cancel->load();
            },
            move);
        // What a move leaves behind (excluded files, empty folders) goes too.
        if (move && out.result == ImportResult::Done && !leftover.empty())
            remove_tree(leftover);
        std::lock_guard<std::mutex> lock(shared->m);
        shared->outcome = out;
        shared->importing = false;
        shared->import_done = true;
    });
}

void Launcher::tick() {
    std::vector<std::function<void()>> posted;
    {
        std::lock_guard<std::mutex> lock(shared_->m);
        posted.swap(shared_->posted);
    }
    // A picker result may start (and, run inline, finish) an import.
    for (auto &fn : posted)
        fn();
    steam_tick();
    if ((screen_ == Screen::Steam && steam_staging_) || screen_ == Screen::Update)
        dirty_ = true;
    bool done = false;
    ImportOutcome outcome;
    {
        std::lock_guard<std::mutex> lock(shared_->m);
        if (shared_->import_done) {
            done = true;
            shared_->import_done = false;
            outcome = shared_->outcome;
        }
        if (shared_->importing)
            dirty_ = true;
    }
    if (!done)
        return;
    platform_.import_activity(false, nullptr);
    platform_.release(importing_);
    importing_ = Picked{};
    evaluate(info_.import_root);
    // A moved folder is gone; a new one may have appeared.
    found_.clear();
    for (const std::string &c : platform_.candidates(spec_))
        if (std::find(found_.begin(), found_.end(), c) == found_.end())
            found_.push_back(c);
    switch (outcome.result) {
    case ImportResult::Done:
        platform_.protect_import(info_.import_root);
        if (!spec_.stage.empty()) {
            prepare_then_play(steam_edits());
            break;
        }
        if (info_.plays_in_place && status_.state == State::Ready)
            remember_folder(info_.folders_file, info_.import_root);
        screen_ = Screen::Main;
        rebuild();
        if (status_.state != State::Ready)
            show(L("The import finished, but the game is ", "Import się skończył, ale stan gry: ") +
                 state_name(status_.state) + ".");
        break;
    case ImportResult::Cancelled:
        show(L("Import cancelled. Importing again continues where it stopped.",
               "Import przerwany. Ponowny import zacznie tam, gdzie się zatrzymał."));
        break;
    case ImportResult::NoSpace:
        show(L("Not enough free space: the import needs ", "Za mało miejsca: import potrzebuje ") +
             human_bytes(outcome.bytes_needed) + L(" and the device has ", ", a wolne jest ") +
             human_bytes(outcome.bytes_free) +
             L(". Free some space and import again; copied files are kept.",
               ". Zwolnij miejsce i zaimportuj ponownie; skopiowane pliki zostają."));
        break;
    case ImportResult::NoExecutable:
        show(L("The chosen files do not contain ", "Wybrane pliki nie zawierają ") +
             spec_.executable +
             L(". Choose the folder the game is installed in (or a ZIP of it).",
               ". Wskaż folder, w którym gra jest zainstalowana (albo jego ZIP)."));
        break;
    case ImportResult::WrongVersion:
        show(L("This is not the supported version of ", "To nie jest obsługiwana wersja ") + spec_.title +
             ". " + outcome.error);
        break;
    case ImportResult::Incomplete:
        show(L("The chosen files are incomplete: ", "Wybrane pliki są niekompletne: ") + outcome.error + ".");
        break;
    case ImportResult::Failed:
        show(L("The import failed: ", "Import się nie powiódł: ") + outcome.error);
        break;
    }
}

void Launcher::activate(int id) {
    if (id != kPlay)
        countdown_ = 0;
    auto it =
        std::find_if(buttons_.begin(), buttons_.end(), [&](const Button &b) { return b.id == id; });
    if (it == buttons_.end() || !it->enabled)
        return;
    if (id != kDeleteData)
        confirm_delete_ = false;
    if (id != kUninstall)
        confirm_uninstall_ = false;
    if (id != kModsSearch)
        search_active_ = false;
    dirty_ = true;
    if (id >= kModsCategory && id < kModsPresetPick) {
        mod_category_ = id - kModsCategory;
        grid_scroll_ = grid_target_ = 0;
        const std::vector<int> v = visible_mods();
        if (!v.empty() && std::find(v.begin(), v.end(), mod_selected_) == v.end())
            select_tile(v.front());
        rebuild();
        return;
    }
    if (id >= kModsPresetPick && id < kModTile) {
        const size_t p = size_t(id - kModsPresetPick);
        if (p < mods_.presets.size()) {
            std::vector<std::string> missing, unsupported;
            mod_apply_preset(mods_, &sel_, mods_.presets[p], &missing, &unsupported);
            const size_t n = sel_.ids.size();
            mod_notice_ = "Preset " + mods_.presets[p].name + ": " + std::to_string(n) + " " +
                          (n == 1 ? "mod" : (n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 12 || n % 100 > 14)) ? "mody" : "modów") + ".";
            if (!missing.empty())
                mod_notice_ += " Nie znaleziono modyfikacji z presetu: " + std::to_string(missing.size()) +
                               " (" + missing.front() + (missing.size() > 1 ? ", …)." : ").");
            if (!unsupported.empty())
                mod_notice_ += " Pominięte nieobsługiwane: " + std::to_string(unsupported.size()) + ".";
            confirm_play_ = false;
            save_mod_choice(choice_file(), mod_root_, sel_);
            have_choice_ = true;
        }
        rebuild();
        return;
    }
    if (id >= kModTile) {
        const int idx = id - kModTile;
        if (idx < 0 || idx >= int(mods_.mods.size()))
            return;
        // A click on the card's switch switches the mod; anywhere else (and a key) selects it.
        if (pointer_activation_ && tile_toggle(it->rect).contains(pointer_x_, pointer_y_)) {
            mod_notice_ = mod_toggle(mods_, &sel_, mods_.mods[size_t(idx)].id);
            confirm_play_ = false;
            save_mod_choice(choice_file(), mod_root_, sel_);
            have_choice_ = true;
        }
        if (mod_selected_ != idx)
            select_tile(idx);
        rebuild();
        return;
    }
    if (id >= kSteam && id < kSteamToggle + 100) {
        activate_steam(id);
        return;
    }
    if (id >= kUseCandidate && id < kUseCandidate + 100) {
        const std::string root = found_[size_t(id - kUseCandidate)];
        if (info_.plays_in_place) {
            evaluate(root);
            if (status_.state == State::Ready)
                remember_folder(info_.folders_file, root);
            rebuild();
        } else {
            Picked p;
            p.path = root;
            begin_import(p);
        }
        return;
    }
    switch (id) {
    case kPlay:
        if (!unplayable_.empty()) {
            show(unplayable_);
            break;
        }
        if (status_.state == State::Ready) {
            if (info_.plays_in_place)
                remember_folder(info_.folders_file, status_.root);
            write_mods_for_play();
            mark_started();
            finished_ = true;
        }
        break;
    case kMods:
        open_mods();
        break;
    case kModsBack:
        confirm_play_ = false;
        screen_ = Screen::Main;
        focus_ = 0;
        rebuild();
        break;
    case kModsPlay:
        if (!unplayable_.empty()) {
            show(unplayable_);
            break;
        }
        if (status_.state == State::Ready && write_mods_for_play()) {
            if (info_.plays_in_place)
                remember_folder(info_.folders_file, status_.root);
            mark_started();
            finished_ = true;
        } else
            rebuild();
        break;
    case kModToggle:
        if (mod_selected_ >= 0) {
            mod_notice_ = mod_toggle(mods_, &sel_, mods_.mods[size_t(mod_selected_)].id);
            confirm_play_ = false;
            save_mod_choice(choice_file(), mod_root_, sel_);
            have_choice_ = true;
            rebuild();
        }
        break;
    case kModsSearch:
        search_active_ = !search_active_ || !pointer_activation_;
        rebuild();
        break;
    case kModsPresetNone:
        // No preset: the selection stays, the preset's own files no longer apply.
        sel_.preset.clear();
        mod_notice_ = "Bez presetu: zaznaczenie zostaje.";
        confirm_play_ = false;
        save_mod_choice(choice_file(), mod_root_, sel_);
        have_choice_ = true;
        rebuild();
        break;
    case kModsGet:
        platform_.open_url(spec_.mods_url);
        break;
    case kModsRoot: {
        auto weak = std::weak_ptr<Shared>(shared_);
        Launcher *self = this;
        platform_.pick_folder([weak, self](std::vector<Picked> p, std::string error) {
            auto shared = weak.lock();
            if (!shared)
                return;
            std::lock_guard<std::mutex> lock(shared->m);
            shared->posted.push_back([self, p, error]() {
                if (p.empty() || p[0].path.empty()) {
                    if (!error.empty())
                        self->mod_notice_ = "Wybór katalogu nie powiódł się: " + error;
                    self->rebuild();
                    return;
                }
                self->load_mods(p[0].path);
                save_mod_choice(self->choice_file(), self->mod_root_, self->sel_);
                self->have_choice_ = true;
                self->rebuild();
            });
        });
        break;
    }
    case kLocate:
        pick_then(true, true);
        break;
    case kImportFolder:
        pick_then(true, false);
        break;
    case kImportZip:
        pick_then(false, false);
        break;
    case kCopyIntoApp: {
        Picked p;
        p.path = status_.root;
        begin_import(p);
        break;
    }
    case kManage:
        screen_ = Screen::Manage;
        focus_ = 0;
        rebuild();
        break;
    case kExportSaves: {
        auto weak = std::weak_ptr<Shared>(shared_);
        Launcher *self = this;
        platform_.pick_export(
            spec_.title + " saves.zip", [weak, self](std::vector<Picked> p, std::string error) {
                auto shared = weak.lock();
                if (!shared)
                    return;
                std::lock_guard<std::mutex> lock(shared->m);
                shared->posted.push_back([self, p, error]() {
                    if (p.empty()) {
                        if (!error.empty())
                            self->show(self->L("The picker failed: ", "Wybór się nie powiódł: ") + error);
                        return;
                    }
                    std::string err;
                    if (export_profile(self->info_.profile_dir, p[0].path,
                                       {"game", "logs", "*.log", "launcher-*"}, &err)) {
                        self->show(self->L("Saves exported to ", "Zapisy wyeksportowane do ") +
                                   (p[0].name.empty() ? p[0].path : p[0].name) + ".");
                        self->platform_.export_ready(p[0]);
                    } else
                        self->show(self->L("Could not export saves: ", "Nie udało się wyeksportować zapisów: ") +
                                   err);
                });
            });
        break;
    }
    case kImportSaves: {
        auto weak = std::weak_ptr<Shared>(shared_);
        Launcher *self = this;
        platform_.pick_saves([weak, self](std::vector<Picked> p, std::string error) {
            auto shared = weak.lock();
            if (!shared)
                return;
            std::lock_guard<std::mutex> lock(shared->m);
            shared->posted.push_back([self, p, error]() {
                if (p.empty()) {
                    if (!error.empty())
                        self->show(self->L("The picker failed: ", "Wybór się nie powiódł: ") + error);
                    return;
                }
                std::string err;
                if (import_profile(p[0].path, self->info_.profile_dir, &err))
                    self->show(self->L("Saves imported.", "Zapisy zaimportowane."));
                else
                    self->show(self->L("Could not import saves: ", "Nie udało się zaimportować zapisów: ") + err);
            });
        });
        break;
    }
    case kDeleteData:
        if (!confirm_delete_) {
            confirm_delete_ = true;
            rebuild();
            break;
        }
        confirm_delete_ = false;
        if (remove_tree(info_.import_root)) {
            evaluate("");
            screen_ = Screen::Main;
            rebuild();
            show(L("Game data deleted. Saves are kept.", "Dane gry usunięte. Zapisy zostały."));
        } else
            show(L("Could not delete ", "Nie udało się usunąć ") + info_.import_root + ".");
        break;
    case kUpdate:
        begin_update();
        break;
    case kUninstall: {
        if (!confirm_uninstall_) {
            confirm_uninstall_ = true;
            rebuild();
            break;
        }
        confirm_uninstall_ = false;
        std::string error;
        if (steam_)
            steam_->cancel();
        if (platform_.uninstall(&error))
            quit_ = true;
        else
            show(L("Uninstalling stopped: ", "Odinstalowanie się nie udało: ") + error);
        break;
    }
    case kOpenFolder:
        platform_.open_folder(status_.root);
        break;
    case kOpenFolder + 1000:
        platform_.open_folder(info_.profile_dir);
        break;
    case kOpenFolder + 2000: // <profile>/Mods, created here when the game has not made it yet
        os_mkdir((info_.profile_dir + "/Mods").c_str());
        platform_.open_folder(info_.profile_dir + "/Mods");
        break;
    case kOpenFolder + 3000: // <profile>/DataMods, the same way
        os_mkdir((info_.profile_dir + "/DataMods").c_str());
        platform_.open_folder(info_.profile_dir + "/DataMods");
        break;
    case kStore:
        platform_.open_url(spec_.store_url);
        break;
    case kBack:
    case kOk:
        if (screen_ == Screen::Steam)
            steam_.reset();
        screen_ = Screen::Main;
        focus_ = 0;
        rebuild();
        break;
    case kCancel:
        cancel_->store(true);
        break;
    case kQuit:
        quit_ = true;
        break;
    default:
        break;
    }
}

// Arrows move to the nearest enabled button in that direction (the buttons'
// rects from the last draw); with none there, along the list.
void Launcher::move_focus(int dx, int dy) {
    const int n = int(buttons_.size());
    if (n == 0)
        return;
    const Rect &from = buttons_[size_t(std::clamp(focus_, 0, n - 1))].rect;
    const float fx = float(from.x) + float(from.w) / 2, fy = float(from.y) + float(from.h) / 2;
    int best = -1;
    float best_score = 1e30f;
    for (int i = 0; i < n; ++i) {
        const Button &b = buttons_[size_t(i)];
        if (i == focus_ || !b.enabled || b.rect.empty())
            continue;
        const float bx = float(b.rect.x) + float(b.rect.w) / 2, by = float(b.rect.y) + float(b.rect.h) / 2;
        // Distance along the direction, and off it (rects that overlap the
        // source's span count as straight ahead).
        float along, across;
        if (dx != 0) {
            along = (bx - fx) * float(dx);
            const bool overlap = b.rect.y < from.y + from.h && b.rect.y + b.rect.h > from.y;
            across = overlap ? 0.0f : std::fabs(by - fy);
        } else {
            along = (by - fy) * float(dy);
            const bool overlap = b.rect.x < from.x + from.w && b.rect.x + b.rect.w > from.x;
            across = overlap ? 0.0f : std::fabs(bx - fx);
        }
        if (along <= 1.0f)
            continue;
        const float score = along + across * 2.0f;
        if (score < best_score) {
            best_score = score;
            best = i;
        }
    }
    if (best >= 0) {
        focus_ = best;
        return;
    }
    const int step = dx + dy > 0 ? 1 : -1;
    for (int i = 1; i <= n; ++i) {
        const int j = ((focus_ + step * i) % n + n) % n;
        if (buttons_[size_t(j)].enabled) {
            focus_ = j;
            break;
        }
    }
}

void Launcher::key(Key k) {
    if (countdown_ > 0) {
        countdown_ = 0;
        dirty_ = true;
        return;
    }
    if (buttons_.empty())
        return;
    const int n = int(buttons_.size());
    dirty_ = true;
    keyboard_ = true;
    hover_ = -1;
    const auto linear = [&](int step) {
        for (int i = 1; i <= n; ++i) {
            const int j = ((focus_ + step * i) % n + n) % n;
            if (buttons_[size_t(j)].enabled) {
                focus_ = j;
                break;
            }
        }
    };
    if (search_active_ && k == Key::Activate) { // Enter ends typing
        search_active_ = false;
        return;
    }
    if (screen_ == Screen::Steam && steam_field_ != 0 && k == Key::Activate) {
        // Enter in the name goes on to the password; anywhere else it sends.
        if (steam_field_ == kSteamUserField && !steam_)
            activate(kSteamPassField);
        else
            activate(steam_ ? kSteamSend : kSteamLogin);
        return;
    }
    switch (k) {
    case Key::Up:
        move_focus(0, -1);
        break;
    case Key::Down:
        move_focus(0, 1);
        break;
    case Key::Left:
        move_focus(-1, 0);
        break;
    case Key::Right:
        move_focus(1, 0);
        break;
    case Key::Previous:
        linear(-1);
        break;
    case Key::Next:
        linear(1);
        break;
    case Key::Activate:
        activate(buttons_[size_t(focus_)].id);
        break;
    case Key::Back:
        if (search_active_ || (screen_ == Screen::Mods && !search_.empty())) {
            search_active_ = false;
            search_.clear();
            rebuild();
        } else if (screen_ == Screen::Importing)
            activate(kCancel);
        else if (screen_ == Screen::Steam)
            activate(std::any_of(buttons_.begin(), buttons_.end(),
                                 [](const Button &b) { return b.id == kSteamCancel; })
                         ? kSteamCancel
                         : kBack);
        else if (screen_ == Screen::Mods)
            activate(kModsBack);
        else if (screen_ != Screen::Main)
            activate(screen_ == Screen::Message ? kOk : kBack);
        else
            activate(kQuit);
        break;
    }
    if (screen_ == Screen::Mods)
        reveal_focus();
}

bool Launcher::clickable(const Button &b, int x, int y) const {
    if (!b.rect.contains(x, y))
        return false;
    if (screen_ != Screen::Mods)
        return true;
    // Cards and the description scroll under the bars: only their visible part counts.
    if (b.kind == kKindTile)
        return grid_rect_.contains(x, y);
    if (b.id == kModToggle)
        return detail_rect_.contains(x, y);
    return true;
}

void Launcher::pointer(int x, int y, bool pressed) {
    if (countdown_ > 0) {
        // The touch that stops the countdown does nothing else.
        countdown_ = 0;
        pressed_ = -2;
        dirty_ = true;
        return;
    }
    if (pressed_ == -2) {
        if (!pressed)
            pressed_ = -1;
        return;
    }
    keyboard_ = false;
    for (size_t i = 0; i < buttons_.size(); ++i) {
        if (!clickable(buttons_[i], x, y))
            continue;
        if (pressed) {
            pressed_ = int(i);
            focus_ = int(i);
            dirty_ = true;
        } else if (pressed_ == int(i)) {
            pressed_ = -1;
            pointer_x_ = x;
            pointer_y_ = y;
            pointer_activation_ = true;
            activate(buttons_[i].id);
            pointer_activation_ = false;
        }
        return;
    }
    if (!pressed) {
        pressed_ = -1;
        dirty_ = true;
    } else if (search_active_) {
        search_active_ = false;
        dirty_ = true;
    }
}

void Launcher::hover(int x, int y) {
    int over = -1;
    for (size_t i = 0; i < buttons_.size(); ++i)
        if (buttons_[i].enabled && clickable(buttons_[i], x, y)) {
            over = int(i);
            break;
        }
    pointer_x_ = x;
    pointer_y_ = y;
    if (over != hover_ || screen_ == Screen::Mods) {
        hover_ = over;
        dirty_ = true;
    }
}

int Launcher::P(float points) const {
    return int(std::lround(points * d_));
}

// Fonts and pictures, loaded once (fonts again when the game folder changes:
// the display font lives there).
void Launcher::ensure_look() {
    Look &k = *look_;
    if (!k.fonts_done || k.fonts_for != status_.root) {
        k.fonts_done = true;
        k.fonts_for = status_.root;
        auto load_first = [&](Font &f, const std::vector<std::string> &list) {
            for (const std::string &p : list) {
                const std::string path = !p.empty() && p[0] != '/' && !status_.root.empty()
                                             ? status_.root + "/" + p
                                             : p;
                if (!p.empty() && (p[0] == '/' || !status_.root.empty()) && f.load(path))
                    return true;
            }
            return false;
        };
        std::vector<std::string> body = spec_.font_body, strong = spec_.font_strong;
        if (body.empty())
            body = {"/System/Library/Fonts/Supplemental/Georgia.ttf", "/System/Library/Fonts/Helvetica.ttc#0"};
        if (strong.empty())
            strong = {"/System/Library/Fonts/Supplemental/Georgia Bold.ttf", "/System/Library/Fonts/Helvetica.ttc#1"};
        if (!k.body.ok() || k.body.path().empty())
            load_first(k.body, body);
        if (!k.strong.ok())
            load_first(k.strong, strong);
        if (!load_first(k.display, spec_.font_display))
            k.display.set_fallback(&k.strong);
        k.display.set_fallback(k.strong.ok() ? &k.strong : &k.body);
        k.display.defer(0x2026); // Gothic3.ttf maps "…" to nothing visible
        k.strong.set_fallback(&k.body);
        if (!k.strong.ok())
            k.strong.set_fallback(&k.body);
    }
    if (!k.art_done) {
        k.art_done = true;
        if (!info_.art_dir.empty()) {
            Image img;
            if (platform_.decode_image(info_.art_dir + "/backdrop.jpg", &img))
                k.art_backdrop = std::move(img);
            if (platform_.decode_image(info_.art_dir + "/logo.jpg", &img)) {
                Image a = alpha_from_black(img, 2.6f);
                k.logo = crop(a, opaque_bounds(a, 24));
            }
            if (platform_.decode_image(info_.art_dir + "/frame.jpg", &img))
                k.frame = std::move(img);
            if (platform_.decode_image(info_.art_dir + "/rosette.jpg", &img))
                k.rosette = alpha_from_black(img, 1.8f);
        }
    }
    // Without the game's art, a frame of its intro (decoded once, then kept in the profile).
    if (k.art_backdrop.rgba.empty() && !spec_.backdrop_video.empty() && !status_.root.empty() &&
        (!k.video_done || k.video_for != status_.root)) {
        k.video_done = true;
        k.video_for = status_.root;
        const std::string cache = info_.profile_dir.empty() ? "" : info_.profile_dir + "/launcher-backdrop.rgba";
        Image img;
        if (!cache.empty()) {
            if (FILE *f = fopen(cache.c_str(), "rb")) {
                int wh[2] = {0, 0};
                if (fread(wh, sizeof wh, 1, f) == 1 && wh[0] > 0 && wh[1] > 0 && wh[0] < 8192 && wh[1] < 8192) {
                    img.w = wh[0];
                    img.h = wh[1];
                    img.rgba.resize(size_t(img.w) * size_t(img.h) * 4);
                    if (fread(img.rgba.data(), 1, img.rgba.size(), f) != img.rgba.size())
                        img = Image{};
                }
                fclose(f);
            }
        }
        if (img.rgba.empty()) {
            const std::string video = status_.root + "/" + spec_.backdrop_video;
            if (platform_.decode_video_frame(video, spec_.backdrop_seconds, &img) && !cache.empty()) {
                if (FILE *f = fopen((cache + ".part").c_str(), "wb")) {
                    const int wh[2] = {img.w, img.h};
                    const bool ok = fwrite(wh, sizeof wh, 1, f) == 1 &&
                                    fwrite(img.rgba.data(), 1, img.rgba.size(), f) == img.rgba.size();
                    fclose(f);
                    if (ok)
                        os_rename((cache + ".part").c_str(), cache.c_str());
                }
            }
        }
        k.video_frame = std::move(img);
    }
}

// Polish counts: 1 mod, 2-4 mody, 5+ modów (12-14: modów).
std::string mods_word(size_t n) {
    if (n == 1)
        return "mod";
    if (n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 12 || n % 100 > 14))
        return "mody";
    return "modów";
}

std::string Launcher::mods_summary() {
    if (spec_.mod_root.empty())
        return "";
    if (!have_choice_)
        return L("No mods", "Bez modów");
    if (!mods_loaded_)
        load_mods(mod_root_);
    const std::vector<const ModInfo *> active = mod_active(mods_, sel_);
    if (active.empty())
        return L("No mods", "Bez modów");
    std::string s = L("Mods: ", "Mody: ") + active.front()->title;
    const size_t more = active.size() - 1;
    if (more == 1)
        s += L(" and 1 more", " i 1 inny");
    else if (more > 1)
        s += L(" and ", " i ") + std::to_string(more) +
             (spec_.language != "pl" ? std::string(" more")
              : (more % 10 >= 2 && more % 10 <= 4 && (more % 100 < 12 || more % 100 > 14)) ? " inne"
                                                                                           : " innych");
    if (!sel_.preset.empty())
        s += "  ·  preset " + sel_.preset;
    return s;
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------
void Launcher::draw_button(Canvas &c, const Button &b, int index) {
    if (b.rect.empty())
        return;
    Look &k = *look_;
    const float h = hot(index);
    const bool down = pressed_ == index;
    const Rect r = b.rect;
    const float alpha = b.enabled ? 1.0f : 0.45f;
    auto framed = [&](Rect fr, float corner_pt, float bright) {
        c.shadow(fr, P(6), P(14), hex(0x000000, 150));
        if (!k.frame.rgba.empty()) {
            const int cp = std::max(4, std::min({P(corner_pt), fr.w / 3, fr.h / 2}));
            const std::string key = "frame:" + std::to_string(fr.w) + ":" + std::to_string(fr.h) + ":" +
                                    std::to_string(cp);
            const Image &img = k.cached(key, [&] {
                const int si[4] = {60, 60, 60, 60}, di[4] = {cp, cp, cp, cp};
                return nine_slice(k.frame, si, fr.w, fr.h, di);
            });
            ImageStyle st;
            st.radius = P(4);
            st.brightness = bright;
            st.opacity = alpha;
            c.image(fr.x, fr.y, fr.w, fr.h, img.rgba.data(), st);
        } else {
            const int rad = P(6);
            c.fill_round(fr, rad, fade(mix(hex(0x2a221a), hex(0x3a2e22), (bright - 1.0f) * 3), alpha));
            c.gradient(fr.inset(P(3)), fade(hex(0x000000, 0), alpha), fade(hex(0x000000, 90), alpha));
            c.stroke_round(fr, rad, std::max(1, P(2)), fade(mix(hex(0x8a6a3a), pal::gold_hot, (bright - 1.0f) * 2), alpha));
            c.stroke_round(fr.inset(P(3)), std::max(1, rad - P(3)), std::max(1, P(1)), fade(hex(0x000000, 160), alpha));
        }
    };
    const Measure strong_m = [&](const std::string &s) { return k.strong.measure(s, P(18)); };
    switch (b.kind) {
    case kKindPrimary: {
        framed(r, std::min(28.0f, float(r.h) / d_ * 0.32f), 1.0f + 0.22f * h + (down ? -0.1f : 0.0f));
        // Rosettes at both ends, the play mark and the label in warm gold capitals.
        const float rh = float(r.h);
        const int ros = int(rh * 0.42f);
        if (!k.rosette.rgba.empty() && float(r.w) > rh * 4.2f) {
            const Image &img = k.cached("rosette:" + std::to_string(ros),
                                        [&] { return resample(k.rosette, ros, ros); });
            ImageStyle st;
            st.opacity = 0.85f * alpha;
            st.brightness = 1.0f + 0.2f * h;
            const int pad = int(rh * 0.24f);
            c.image(r.x + pad, r.y + (r.h - ros) / 2, ros, ros, img.rgba.data(), st);
            c.image(r.x + r.w - pad - ros, r.y + (r.h - ros) / 2, ros, ros, img.rgba.data(), st);
        }
        const int px = std::max(P(12), int(rh * 0.30f));
        const std::string label = upper_utf8(b.label);
        const float track = float(px) * 0.06f;
        Font &f = k.strong.ok() ? k.strong : k.display;
        int lw = f.measure(label, px, track);
        const int icon = b.icon >= 0 ? int(float(px) * 0.9f) : 0, gap = icon ? px / 2 : 0;
        int size = px;
        const int room = r.w - (float(r.w) > rh * 4.2f ? 2 * int(rh * 0.7f) : P(16));
        if (lw + icon + gap > room) { // a narrow screen: smaller letters
            size = std::max(P(9), px * room / std::max(1, lw + icon + gap));
            lw = f.measure(label, size, track);
        }
        const int total = lw + (icon ? int(float(size) * 0.9f) + size / 2 : 0);
        int x = r.x + (r.w - total) / 2;
        const Color col = fade(mix(pal::gold, pal::gold_hot, h), alpha);
        const int baseline = r.y + r.h / 2 + (f.ascent(size) - f.descent(size)) / 2;
        if (icon) {
            const float is = float(size) * 0.9f;
            draw_icon(c, Icon(b.icon), float(x) + is / 2, float(r.y) + rh / 2, is * 0.8f, col);
            x += int(is) + size / 2;
        }
        f.draw(c, x + P(1), baseline + P(2), label, size, hex(0x000000, uint8_t(160 * alpha)), track);
        f.draw(c, x, baseline, label, size, col, track);
        break;
    }
    case kKindNormal: {
        // Back on the mod screen steps aside for Play: a darker frame.
        const float base = b.id == kModsBack ? 0.72f : 1.0f;
        framed(r, std::min(16.0f, float(r.h) / d_ * 0.3f), base + 0.25f * h + (down ? -0.1f : 0.0f));
        const int px = std::clamp(int(float(r.h) * 0.34f), P(11), P(22));
        Font &f = k.strong;
        const int icon = b.icon >= 0 ? int(float(px) * 1.15f) : 0, gap = icon ? px / 2 : 0;
        const std::string label = ellipsize(b.label, r.w - P(24) - icon - gap,
                                            [&](const std::string &s) { return f.measure(s, px); });
        const int lw = f.measure(label, px);
        int x = r.x + (r.w - lw - icon - gap) / 2;
        const Color col = fade(mix(pal::text, pal::gold_hot, h), alpha);
        const int baseline = r.y + r.h / 2 + (f.ascent(px) - f.descent(px)) / 2;
        if (icon) {
            draw_icon(c, Icon(b.icon), float(x + icon / 2), float(r.y) + float(r.h) / 2, float(icon),
                      fade(mix(pal::gold, pal::gold_hot, h), alpha));
            x += icon + gap;
        }
        f.draw(c, x, baseline, label, px, col);
        break;
    }
    case kKindIcon: {
        framed(r, std::min(12.0f, float(r.h) / d_ * 0.3f), 1.0f + 0.25f * h);
        draw_icon(c, Icon(std::max(0, b.icon)), float(r.x) + float(r.w) / 2, float(r.y) + float(r.h) / 2,
                  float(r.h) * 0.46f, fade(mix(pal::gold, pal::gold_hot, h), alpha));
        break;
    }
    case kKindBar: {
        const int px = std::clamp(int(float(r.h) * 0.45f), P(11), P(17));
        const Color col = fade(mix(pal::dim, pal::text, h), alpha);
        int x = r.x;
        if (b.icon >= 0 && r.w > px * 3) {
            draw_icon(c, Icon(b.icon), float(x) + float(px) * 0.6f, float(r.y) + float(r.h) / 2, float(px) * 1.1f,
                      fade(mix(pal::gold_deep, pal::gold, h), alpha));
            x += int(float(px) * 1.6f);
        }
        const std::string label =
            ellipsize(b.label, r.x + r.w - x, [&](const std::string &s) { return k.body.measure(s, px); });
        k.body.draw(c, x, r.y + r.h / 2 + (k.body.ascent(px) - k.body.descent(px)) / 2, label, px, col);
        break;
    }
    default:
        break;
    }
    (void)strong_m;
    if (focus_visible(index))
        c.stroke_round({r.x - P(4), r.y - P(4), r.w + P(8), r.h + P(8)}, P(8), std::max(1, P(2)), pal::gold_hot);
}

// ---------------------------------------------------------------------------
// The main screen (and manage, import, messages)
// ---------------------------------------------------------------------------
void Launcher::draw(Canvas &c, float density) {
    d_ = std::max(0.5f, density);
    dirty_ = false;
    ensure_look();
    if (screen_ == Screen::Mods) {
        draw_mods(c);
        return;
    }
    draw_main(c);
}

void Launcher::draw_starting(Canvas &c, float density) {
    d_ = std::max(0.5f, density);
    ensure_look();
    Look &k = *look_;
    const int cw = c.width(), ch = c.height();
    c.reset_clip();
    k.backdrop(c, 0);
    c.fill({0, 0, cw, ch}, hex(0x0b0907, 120));
    const std::string title = L("Starting the game…", "Uruchamianie gry…");
    const std::string sub = spec_.title;
    const int tpx = P(34), spx = P(16);
    const int tw = k.display.measure(title, tpx), sw = k.body.measure(sub, spx);
    const int y = ch / 2;
    k.display.draw(c, (cw - tw) / 2, y, title, tpx, pal::gold_hot);
    if (!sub.empty())
        k.body.draw(c, (cw - sw) / 2, y + P(34), sub, spx, pal::dim);
    // The first start prepares what later starts reuse (Gothic 3: its shaders): say so, or it looks hung.
    if (first_start_) {
        const std::string note =
            L("The first start prepares the game and can take a few minutes.",
              "Pierwsze uruchomienie przygotowuje grę i może potrwać kilka minut.");
        const int npx = P(16);
        k.body.draw(c, (cw - k.body.measure(note, npx)) / 2, y + P(64), note, npx, pal::text);
    }
}

void Launcher::draw_main(Canvas &c) {
    Look &k = *look_;
    const int cw = c.width(), ch = c.height();
    const float W = float(cw) / d_, H = float(ch) / d_;
    c.reset_clip();
    k.backdrop(c, 0);

    // The bottom bar: links on the left, the version on the right.
    const float bar_h = H < 360 ? 40 : 60;
    const float Lm = std::clamp(W * 0.055f, 16.0f, 92.0f);
    const float T = std::clamp(H * 0.06f, 10.0f, 64.0f);
    const int bar_y = ch - P(bar_h);
    c.gradient({0, bar_y, cw, P(bar_h)}, hex(0x0b0907, 150), hex(0x0b0907, 220));
    c.fill({0, bar_y, cw, std::max(1, P(1))}, fade(pal::border, 0.9f));
    {
        std::string right = version_.empty() ? std::string() : L("version ", "wersja ") + version_;
        const int rpx = P(13);
        const int rw = right.empty() ? 0 : k.body.measure(right, rpx);
        if (!right.empty())
            k.body.draw(c, cw - P(Lm) - rw, bar_y + P(bar_h) / 2 + k.body.ascent(rpx) / 2 - P(1), right, rpx,
                        hex(0xbfb39c));
        // The links in a row, separated by thin rules; smaller when they do not fit.
        std::vector<int> items;
        for (size_t i = 0; i < buttons_.size(); ++i)
            if (buttons_[i].kind == kKindBar)
                items.push_back(int(i));
        float px = 17;
        const float avail = W - 2 * Lm - (rw ? float(rw) / d_ + 24 : 0);
        std::vector<float> widths;
        for (int attempt = 0; attempt < 6; ++attempt) {
            widths.clear();
            float total = 0;
            for (int i : items) {
                const float wpt = float(k.body.measure(buttons_[size_t(i)].label, P(px))) / d_ + px * 1.6f;
                widths.push_back(wpt);
                total += wpt + 40;
            }
            if (total - 40 <= avail || px <= 11)
                break;
            px -= 1.5f;
        }
        float x = Lm, row = 0;
        const float item_h = std::min(bar_h - 12, 36.0f);
        for (size_t n = 0; n < items.size(); ++n) {
            float wpt = std::min(widths[n], avail);
            if (x + wpt > Lm + avail && x > Lm) { // a second row, above the first
                x = Lm;
                row += 1;
            }
            Button &b = buttons_[size_t(items[n])];
            const float y = bar_h / 2 - item_h / 2 + row * -item_h;
            b.rect = {P(x), bar_y + P(y), P(wpt), P(item_h)};
            if (x > Lm)
                c.fill({P(x - 20), bar_y + P(bar_h / 2 - 10 + row * -item_h), std::max(1, P(1.5f)), P(20)},
                       fade(pal::gold_deep, 0.75f));
            x += wpt + 40;
        }
    }

    // The column on the left: logo, state, folder, Play, mods, the other buttons.
    const bool ready = status_.state == State::Ready;
    const float col_w = std::min(W - 2 * Lm, std::max(std::min(340.0f, W - 2 * Lm), std::min(660.0f, W * 0.46f)));
    Color accent = pal::red;
    std::string headline, sub, detail;
    switch (status_.state) {
    case State::Ready:
        accent = pal::green;
        headline = L("Ready to play", "Gotowe do gry");
        sub = spec_.subtitle;
        if (countdown_ > 0) {
            headline = L("Starting in ", "Start za ") + std::to_string(int(countdown_ + 0.999)) + "…";
            sub = info_.touch ? L("Touch the screen to open the launcher instead.",
                                  "Dotknij ekranu, żeby zostać w launcherze.")
                              : L("Press any key to open the launcher instead.",
                                  "Naciśnij dowolny klawisz, żeby zostać w launcherze.");
        }
        break;
    case State::WrongVersion:
        accent = pal::amber;
        headline = L("This ", "Tego pliku ") + spec_.executable + L(" cannot be used", " nie da się użyć");
        detail = L("It is not the version of ", "To nie jest wersja ") + spec_.title +
                 L(" this port is made for. Nothing was changed.", ", dla której jest ten port. Nic nie zostało zmienione.") +
                 "\nSHA-256 " + status_.digest;
        break;
    case State::Incomplete: {
        accent = pal::amber;
        headline = L("Game folder is incomplete", "Folder gry jest niekompletny");
        std::string missing;
        for (const std::string &m : status_.missing)
            missing += (missing.empty() ? "" : ", ") + m;
        detail = L("Missing: ", "Brakuje: ") + missing;
        break;
    }
    case State::NotFound:
        headline = L("Game not found", "Nie znaleziono gry");
        detail = info_.plays_in_place
                     ? L("Locate the folder ", "Wskaż folder, w którym jest zainstalowany ") + spec_.title +
                           L(" is installed in (it holds ", " (z plikiem ") + spec_.executable + ")."
                     : L("Import your copy of ", "Zaimportuj swoją kopię ") + spec_.title +
                           L(": the installed game folder, or a ZIP of it.", ": folder z zainstalowaną grą albo jego ZIP.");
        if (!info_.drop_hint.empty())
            detail += "\n" + info_.drop_hint;
        if (!spec_.store_url.empty())
            detail += "\n" + L("Get the game: ", "Gdzie kupić: ") + spec_.store_url;
        break;
    }
    if (screen_ == Screen::Manage) {
        accent = pal::gold;
        headline = L("Manage files", "Zarządzaj plikami");
        detail = L("Saves and settings: ", "Zapisy i ustawienia: ") + display(info_.profile_dir) +
                 L("\nThey are kept when the game data is deleted or imported again.",
                   "\nZostają, gdy dane gry zostaną usunięte albo zaimportowane ponownie.");
        if (confirm_uninstall_)
            detail = L("Uninstalling removes the downloaded game, the Steam login, the saves and "
                       "the settings, and "
                       "moves the app to the Trash. Export the saves first to keep them.",
                       "Odinstalowanie usuwa pobraną grę, logowanie do Steama, zapisy i "
                       "ustawienia, a aplikację "
                       "przenosi do Kosza. Żeby zachować zapisy, najpierw je wyeksportuj.");
        sub.clear();
    } else if (screen_ == Screen::Update) {
        accent = pal::gold;
        headline = L("Updating to ", "Aktualizacja do ") + update_.version + "…";
        detail = L("The game and the saves stay. The app starts again by itself.",
                   "Gra i zapisy zostają. Aplikacja uruchomi się ponownie sama.");
        sub.clear();
    } else if (screen_ == Screen::Message) {
        accent = pal::gold;
        headline = L("Notice", "Komunikat");
        detail = message_;
        sub.clear();
    } else if (screen_ == Screen::Steam) {
        steam_text(&headline, &sub, &detail, &accent);
    } else if (screen_ == Screen::Importing) {
        accent = pal::gold;
        headline = L("Importing…", "Import…");
        detail = L("Keep the app open. If it stops, importing again continues where it left off.",
                   "Nie zamykaj aplikacji. Jeśli import się przerwie, ponowny zacznie tam, gdzie skończył.");
        sub.clear();
    }

    // Natural sizes (points); the whole column shrinks on a small screen.
    std::vector<int> primary, normal, icons;
    for (size_t i = 0; i < buttons_.size(); ++i) {
        const int kind = buttons_[i].kind;
        if (kind == kKindPrimary)
            primary.push_back(int(i));
        else if (kind == kKindNormal)
            normal.push_back(int(i));
        else if (kind == kKindIcon)
            icons.push_back(int(i));
    }
    const bool main_ready = screen_ == Screen::Main && ready;
    const bool show_path = screen_ == Screen::Main && !status_.root.empty();
    const std::string summary = main_ready ? mods_summary() : std::string();
    const float avail = H - bar_h - T - 14;
    const int normal_cols = normal.size() >= 3 || screen_ == Screen::Main ? 2 : 1;
    const int normal_rows = int((normal.size() + size_t(normal_cols) - 1) / size_t(normal_cols));
    float logo_w = std::min(col_w * 0.8f, 500.0f);
    const float logo_aspect = k.logo.rgba.empty() ? 0.0f : float(k.logo.h) / float(k.logo.w);
    float logo_h = k.logo.rgba.empty() ? 76.0f : logo_w * logo_aspect;
    const float detail_lines_h = detail.empty() ? 0.0f : 3 * 22.0f;
    auto base_height = [&](float s) {
        float y = 34 * s + 26 * s; // tagline, gap
        y += 32 * s;              // headline
        if (!sub.empty())
            y += 24 * s;
        y += detail.empty() ? 0 : detail_lines_h * s + 8 * s;
        if (screen_ == Screen::Importing)
            y += 40 * s;
        if (screen_ == Screen::Steam || screen_ == Screen::Update)
            y += steam_extras_height(s);
        if (show_path)
            y += 14 * s + 46 * s;
        if (!primary.empty())
            y += 22 * s + std::clamp(col_w * 0.18f, 60.0f, 118.0f) * s;
        if (!summary.empty())
            y += 32 * s;
        if (!normal.empty())
            y += 14 * s + float(normal_rows) * 62 * s - 8 * s;
        return y;
    };
    float s = 1.0f;
    float base = base_height(1.0f);
    if (base + logo_h > avail) {
        logo_h = std::max(0.0f, avail - base);
        if (logo_h < 70) {
            logo_h = std::min(70.0f, std::max(0.0f, avail - base));
            s = std::clamp((avail - logo_h) / base, 0.4f, 1.0f);
        }
        if (!k.logo.rgba.empty())
            logo_w = logo_h / logo_aspect;
    }
    // The column sits a little above the middle of the room it has.
    float y = T + std::max(0.0f, (avail - base_height(s) - logo_h) * 0.5f);
    const float x0 = Lm;
    // The logo (the game's art, or the banner in its font, metal-like).
    if (!k.logo.rgba.empty() && logo_h >= 40) {
        const int lw = P(logo_w), lh = P(logo_h);
        const Image &img = k.cached("logo:" + std::to_string(lw), [&] { return resample(k.logo, lw, lh); });
        c.image(P(x0) - P(6), P(y), lw, lh, img.rgba.data(), ImageStyle{});
    } else {
        const std::string banner = spec_.banner.empty() ? spec_.title : spec_.banner;
        int px = P(std::min(logo_h, 76.0f) * 0.9f);
        px = std::max(P(14), std::min(px, px * P(col_w) / std::max(1, k.display.measure(banner, px))));
        const int baseline = P(y) + k.display.ascent(px);
        k.display.draw(c, P(x0) + P(2), baseline + P(3), banner, px, hex(0x000000, 170));
        // A metal gradient: bright at the top, gold below.
        const int top = baseline - k.display.ascent(px), bot = baseline + k.display.descent(px);
        const int bands = 8;
        for (int i = 0; i < bands; ++i) {
            const int y0 = top + (bot - top) * i / bands, y1 = top + (bot - top) * (i + 1) / bands;
            c.set_clip({0, y0, cw, y1 - y0});
            k.display.draw(c, P(x0), baseline, banner, px,
                           mix(hex(0xf0dca0), pal::gold_deep, float(i) / float(bands - 1)));
        }
        c.reset_clip();
        logo_w = float(k.display.measure(banner, px)) / d_;
    }
    y += logo_h;
    // The tagline, letters spaced, centred under the logo.
    {
        const std::string tag = upper_utf8(L("Native port for macOS", "Natywny port na macOS"));
        const int px = P(13 * std::max(s, 0.8f));
        const float track = 5.0f * d_ * s;
        const int tw = k.body.measure(tag, px, track);
        const int tx = P(x0) + std::max(0, (P(logo_w) - tw) / 2) - P(6);
        y += 8 * s;
        k.body.draw(c, tx, P(y) + k.body.ascent(px), tag, px, hex(0xbfb39c), track);
        y += 26 * s + 26 * s;
    }
    // The state: a dot and the headline, the edition or the details under it.
    {
        const int px = P(22 * s);
        const int base_y = P(y) + k.body.ascent(px);
        c.disc(float(P(x0)) + float(P(6 * s)), float(base_y - k.body.ascent(px) / 2 + P(2)), float(P(5.5f * s)),
               accent);
        c.shadow({P(x0), base_y - k.body.ascent(px) / 2 - P(4), P(12 * s), P(8 * s)}, P(6), P(8), fade(accent, 0.25f));
        k.body.draw(c, P(x0 + 20 * s), base_y, headline, px, pal::text);
        y += 32 * s;
        if (!sub.empty()) {
            const int spx = P(16 * s);
            k.body.draw(c, P(x0 + 20 * s), P(y) + k.body.ascent(spx), sub, spx, pal::dim);
            y += 24 * s;
        }
    }
    // Where the buttons go: below the text, or lower when the text needs the room.
    float buttons_h = 0;
    if (show_path)
        buttons_h += 14 * s + 46 * s;
    if (!primary.empty())
        buttons_h += 22 * s + std::clamp(col_w * 0.18f, 60.0f, 118.0f) * s;
    if (!summary.empty())
        buttons_h += 32 * s;
    if (!normal.empty())
        buttons_h += 14 * s + float(normal_rows) * 62 * s - 8 * s;
    float buttons_y = H - bar_h - 14 - buttons_h;
    if (!detail.empty() || screen_ == Screen::Importing) {
        const float text_top = y + 4 * s;
        float text_bottom = std::max(text_top, buttons_y - 10 * s);
        const int px = P(15 * s);
        const Measure m = [&](const std::string &t) { return k.body.measure(t, px); };
        const std::vector<std::string> lines = wrap_text(detail, P(col_w - 20 * s), m);
        float ty = text_top;
        if (screen_ == Screen::Importing) {
            Progress p;
            {
                std::lock_guard<std::mutex> lock(shared_->m);
                p = shared_->progress;
            }
            const Rect bar{P(x0 + 20 * s), P(ty + 4 * s), P(col_w - 20 * s), P(10 * s)};
            c.fill_round(bar, bar.h / 2, hex(0x000000, 140));
            const double frac = p.bytes_total ? double(p.bytes_done) / double(p.bytes_total) : 0.0;
            c.fill_round({bar.x, bar.y, std::max(bar.h, int(bar.w * std::min(1.0, frac))), bar.h}, bar.h / 2,
                         pal::gold);
            const std::string counts = std::to_string(p.files_done) + " / " + std::to_string(p.files_total) +
                                       L(" files, ", " plików, ") + human_bytes(p.bytes_done) +
                                       L(" of ", " z ") + human_bytes(p.bytes_total);
            k.body.draw(c, P(x0 + 20 * s), P(ty + 34 * s), counts, px, pal::dim);
            ty += 40 * s;
            text_bottom = std::max(text_bottom, ty);
        }
        c.set_clip({0, P(ty), cw, std::max(0, P(text_bottom) - P(ty))});
        for (const std::string &line : lines) {
            if (ty + 22 * s > text_bottom + 22 * s)
                break;
            k.body.draw(c, P(x0 + 20 * s), P(ty) + k.body.ascent(px), line, px, pal::dim);
            ty += 22 * s;
        }
        c.reset_clip();
        y = std::max(y, std::min(ty + 8 * s, buttons_y));
    }
    if (screen_ == Screen::Steam || screen_ == Screen::Update) {
        draw_steam_extras(c, x0, col_w, s, &y);
        draw_steam_qr(c, W, H, bar_h, col_w, Lm);
    }
    buttons_y = std::max(std::min(y, buttons_y), std::min(y, H - bar_h - 8 - buttons_h));
    if (buttons_y + buttons_h > H - bar_h - 4)
        buttons_y = std::max(2.0f, H - bar_h - 4 - buttons_h);
    y = buttons_y;
    // The game folder, and the icon button beside it.
    if (show_path) {
        y += 14 * s;
        const float fh = 46 * s, icon_w = icons.empty() ? 0.0f : fh + 8 * s;
        const Rect field{P(x0), P(y), P(col_w - icon_w), P(fh)};
        c.fill_round(field, P(4), hex(0x0a0807, 195));
        c.stroke_round(field, P(4), std::max(1, P(1)), mix(pal::border_hi, pal::gold_deep, 0.5f));
        const int px = P(16 * s);
        const std::string path =
            ellipsize_middle(display(status_.root), field.w - P(28 * s),
                             [&](const std::string &t) { return k.body.measure(t, px); });
        k.body.draw(c, field.x + P(14 * s), field.y + field.h / 2 + (k.body.ascent(px) - k.body.descent(px)) / 2,
                    path, px, hex(0xd8ccb4));
        for (int i : icons)
            buttons_[size_t(i)].rect = {P(x0 + col_w - fh), P(y), P(fh), P(fh)};
        y += fh;
    } else
        for (int i : icons)
            buttons_[size_t(i)].rect = {0, 0, 0, 0};
    if (!primary.empty()) {
        y += 22 * s;
        const float ph = std::clamp(col_w * 0.18f, 60.0f, 118.0f) * s;
        for (int i : primary)
            buttons_[size_t(i)].rect = {P(x0), P(y), P(col_w), P(ph)};
        y += ph;
    }
    if (!summary.empty()) {
        const int px = P(17 * s);
        const int bx = P(x0 + 4 * s);
        draw_icon(c, Icon::Puzzle, float(bx) + float(P(9 * s)), float(P(y + 21 * s)), float(P(18 * s)), pal::gold);
        const std::string t = ellipsize(summary, P(col_w - 34 * s),
                                        [&](const std::string &x) { return k.body.measure(x, px); });
        k.body.draw(c, bx + P(28 * s), P(y + 21 * s) + k.body.ascent(px) / 2 - P(1), t, px, hex(0xd8ccb4));
        y += 32 * s;
    }
    if (!normal.empty()) {
        y += 14 * s;
        const float gap = 12 * s, bh = 54 * s;
        const float bw = (col_w - gap * float(normal_cols - 1)) / float(normal_cols);
        for (size_t n = 0; n < normal.size(); ++n) {
            const int col = int(n) % normal_cols, row = int(n) / normal_cols;
            // A last button alone in its row spans it.
            const bool alone = n + 1 == normal.size() && col == 0 && normal_cols > 1;
            buttons_[size_t(normal[n])].rect = {P(x0 + float(col) * (bw + gap)), P(y + float(row) * 62 * s),
                                                P(alone ? col_w : bw), P(bh)};
        }
    }
    // Every button stays on the canvas, whatever the text above it.
    for (Button &b : buttons_) {
        if (b.rect.empty())
            continue;
        b.rect.w = std::min(b.rect.w, cw);
        b.rect.h = std::min(b.rect.h, ch);
        b.rect.x = std::clamp(b.rect.x, 0, cw - b.rect.w);
        b.rect.y = std::clamp(b.rect.y, 0, ch - b.rect.h);
    }
    for (size_t i = 0; i < buttons_.size(); ++i)
        if (buttons_[i].kind != kKindField) // the Steam screen's fields are drawn with their text
            draw_button(c, buttons_[i], int(i));
}

} // namespace launcher
