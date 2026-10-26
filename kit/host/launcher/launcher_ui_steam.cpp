// launcher_ui_steam.cpp - see launcher_ui.h. The Steam screen: log in to the
// player's Steam account (a QR code for the Steam app, or a name and a
// password with Steam Guard), download each depot of [launcher.steam] in
// turn through DepotDownloader, make the modules (launcher_stage.h) and start
// the game. A stopped download continues where it stopped the next time.
#include "launcher_look.h"
#include "launcher_ui.h"

#include "../../platform/os.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace launcher {

namespace {

std::string read_line_file(const std::string &path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

void write_text_file(const std::string &path, const std::string &text) {
    std::ofstream out(path + ".part", std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    if (out)
        os_rename((path + ".part").c_str(), path.c_str());
}

std::vector<std::string> read_lines(const std::string &path) {
    std::vector<std::string> out;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);)
        if (!line.empty())
            out.push_back(line);
    return out;
}

void make_dirs(const std::string &path) {
    for (size_t i = path.find('/', 1); i != std::string::npos; i = path.find('/', i + 1))
        os_mkdir(path.substr(0, i).c_str());
    os_mkdir(path.c_str());
}

} // namespace

// A file under root, its components found without case; "" when it is not there.
static std::string find_root_file(const std::string &root, const std::string &relative) {
    std::string at = root;
    for (size_t from = 0; from <= relative.size();) {
        size_t slash = relative.find('/', from);
        if (slash == std::string::npos)
            slash = relative.size();
        const std::string found = find_name(at, relative.substr(from, slash - from));
        if (found.empty())
            return "";
        at += "/" + found;
        from = slash + 1;
    }
    return at;
}

bool Launcher::steam_offered() const {
    return !spec_.steam.tool.empty() && !spec_.steam.depots.empty() && !info_.steam_tool.empty() &&
           !info_.steam_home.empty() && !info_.import_root.empty();
}

bool Launcher::depot_wanted(size_t i) const {
    const SteamDepot &d = spec_.steam.depots[i];
    return d.label.empty() ||
           std::find(steam_skipped_.begin(), steam_skipped_.end(), d.label) == steam_skipped_.end();
}

double Launcher::steam_overall() const {
    double total = 0, done = 0;
    for (size_t i = 0; i < spec_.steam.depots.size(); ++i) {
        if (!depot_wanted(i))
            continue;
        const double mb = double(std::max<uint64_t>(1, spec_.steam.depots[i].mb));
        total += mb;
        if (i < steam_depot_ || steam_staging_)
            done += mb;
        else if (i == steam_depot_)
            done += mb * steam_state_.percent / 100.0;
    }
    return total > 0 ? done / total : 0;
}

void Launcher::begin_steam() {
    countdown_ = 0;
    if (!steam_offered()) {
        show(L("This app cannot download the game from Steam.",
               "Ta aplikacja nie umie pobrać gry ze Steama."));
        return;
    }
    make_dirs(info_.steam_home);
    make_dirs(info_.import_root);
    steam_skipped_ = read_lines(info_.steam_home + "/skipped.txt");
    if (steam_account_.empty())
        steam_account_ = read_line_file(info_.steam_home + "/account.txt");
    if (steam_account_.empty()) {
        const std::vector<std::string> saved = steam_saved_accounts(info_.steam_home);
        if (!saved.empty())
            steam_account_ = saved.front();
    }
    steam_depot_ = 0;
    while (steam_depot_ < spec_.steam.depots.size() && !depot_wanted(steam_depot_))
        ++steam_depot_;
    steam_cancelled_ = steam_staging_ = false;
    screen_ = Screen::Steam;
    focus_ = 0;
    // A name typed but no session yet: the form waits for its password.
    if (steam_password_mode_ && steam_account_.empty() &&
        (steam_user_.empty() || steam_pass_.empty())) {
        steam_.reset();
        steam_state_ = SteamState{};
        steam_field_ = steam_user_.empty() ? kSteamUserField : kSteamPassField;
        rebuild();
        return;
    }
    launch_depot();
}

void Launcher::launch_depot() {
    const SteamDepot &d = spec_.steam.depots[steam_depot_];
    std::string filelist;
    if (!d.files.empty()) {
        filelist = info_.steam_home + "/filelist-" + std::to_string(d.id) + ".txt";
        std::string text;
        for (const std::string &f : d.files)
            text += f + "\n";
        write_text_file(filelist, text);
    }
    const std::string user = !steam_account_.empty() ? steam_account_
                             : steam_password_mode_  ? steam_user_
                                                     : "";
    steam_ = std::make_unique<SteamSession>();
    steam_state_ = SteamState{};
    steam_password_sent_ = false;
    steam_field_ = 0;
    std::string error;
    if (!steam_->start(info_.steam_tool,
                       steam_arguments(spec_.steam, d, info_.import_root, filelist, user),
                       info_.steam_home, &error)) {
        steam_.reset();
        show(L("Could not start the Steam download: ",
               "Nie udało się uruchomić pobierania ze Steama: ") +
             error);
        return;
    }
    fprintf(stderr, "[launcher] steam: depot %u %s, %s\n", d.id, d.manifest.c_str(),
            user.empty() ? "QR login" : ("as " + user).c_str());
    platform_.import_activity(true, nullptr);
    rebuild();
}

void Launcher::steam_tick() {
    if (!steam_ || screen_ != Screen::Steam)
        return;
    const SteamState st = steam_->state();
    const bool changed = st.phase != steam_state_.phase || st.qr_serial != steam_state_.qr_serial ||
                         st.percent != steam_state_.percent || st.file != steam_state_.file ||
                         st.code_rejected != steam_state_.code_rejected ||
                         st.email != steam_state_.email;
    const SteamPhase before = steam_state_.phase;
    steam_state_ = st;
    if (!changed)
        return;
    dirty_ = true;
    switch (st.phase) {
    case SteamPhase::NeedPassword:
        // The form's password goes as soon as the tool asks; without one (a stored login that
        // stopped working) the form asks the player.
        if (!steam_pass_.empty() && !steam_password_sent_) {
            steam_->answer(steam_pass_);
            steam_password_sent_ = true;
            steam_pass_.clear();
        } else {
            steam_password_mode_ = true;
            steam_field_ = kSteamPassField;
        }
        break;
    case SteamPhase::NeedCode:
        steam_code_.clear();
        steam_field_ = kSteamCodeField;
        break;
    case SteamPhase::Done: {
        // The account the login was for, so the next depot and the next start need no login.
        std::string account = st.account;
        if (account.empty() && steam_account_.empty()) {
            const std::vector<std::string> saved = steam_saved_accounts(info_.steam_home);
            if (!saved.empty())
                account = saved.front();
            else if (steam_password_mode_)
                account = steam_user_;
        }
        if (!account.empty()) {
            steam_account_ = account;
            write_text_file(info_.steam_home + "/account.txt", account + "\n");
        }
        steam_pass_.clear();
        do
            ++steam_depot_;
        while (steam_depot_ < spec_.steam.depots.size() && !depot_wanted(steam_depot_));
        if (steam_depot_ < spec_.steam.depots.size()) {
            launch_depot();
            return;
        }
        // Everything is down: the .ini settings for what was downloaded, the modules, then the game.
        steam_.reset();
        prepare_then_play(steam_edits());
        return;
    }
    case SteamPhase::Failed:
        if (steam_cancelled_) {
            platform_.import_activity(false, nullptr);
            steam_.reset();
            steam_cancelled_ = false;
            screen_ = Screen::Main;
            rebuild();
            show(
                L("Download stopped. Downloading again continues where it stopped.",
                  "Pobieranie zatrzymane. Następne pobieranie zacznie tam, gdzie się zatrzymało."));
            return;
        }
        platform_.import_activity(false, nullptr);
        break;
    case SteamPhase::NotOwned:
    case SteamPhase::LoginFailed:
        platform_.import_activity(false, nullptr);
        break;
    default:
        break;
    }
    if (st.phase != before)
        rebuild();
}

std::vector<std::string> Launcher::steam_edits() {
    if (!info_.steam_home.empty())
        steam_skipped_ = read_lines(info_.steam_home + "/skipped.txt");
    std::vector<std::string> edits = spec_.steam.ini;
    for (size_t i = 0; i < spec_.steam.depots.size(); ++i) {
        const SteamDepot &d = spec_.steam.depots[i];
        // A part not on disk (left out, or an import without it) gets its "off" settings.
        const bool on =
            depot_wanted(i) && (d.files.size() != 1 || d.files[0].compare(0, 6, "regex:") == 0 ||
                                !find_root_file(info_.import_root, d.files[0]).empty());
        const std::vector<std::string> &these = on ? d.ini_on : d.ini_off;
        edits.insert(edits.end(), these.begin(), these.end());
    }
    return edits;
}

void Launcher::prepare_then_play(const std::vector<std::string> &edits) {
    screen_ = Screen::Steam;
    steam_staging_ = true;
    {
        std::lock_guard<std::mutex> lock(shared_->m);
        shared_->progress = Progress{};
    }
    auto weak = std::weak_ptr<Shared>(shared_);
    Launcher *self = this;
    const std::string root = info_.import_root;
    const std::vector<StageModule> modules = spec_.stage;
    rebuild();
    platform_.run_in_background([weak, self, root, modules, edits]() {
        StageOutcome out;
        if (!apply_ini_edits(root, edits, &out.error))
            out.ok = false;
        else
            out = stage_modules(root, modules, [&](size_t done, size_t total, const std::string &) {
                auto shared = weak.lock();
                if (!shared)
                    return;
                std::lock_guard<std::mutex> lock(shared->m);
                shared->progress.files_done = done;
                shared->progress.files_total = total;
            });
        auto shared = weak.lock();
        if (!shared)
            return;
        std::lock_guard<std::mutex> lock(shared->m);
        shared->posted.push_back([self, out]() {
            self->steam_staging_ = false;
            self->platform_.import_activity(false, nullptr);
            self->screen_ = Screen::Main;
            if (!out.ok) {
                self->rebuild();
                self->show(
                    self->L("The game files are here, but preparing them failed: ",
                            "Pliki gry są na miejscu, ale ich przygotowanie się nie udało: ") +
                    out.error);
                return;
            }
            fprintf(stderr, "[launcher] prepared: %d modules made, %d kept\n", out.made, out.kept);
            self->evaluate(self->info_.import_root);
            if (self->status_.state != State::Ready) {
                self->rebuild();
                self->show(self->L("The game files are prepared, but the game is ",
                                   "Pliki gry są przygotowane, ale stan gry: ") +
                           state_name(self->status_.state) + ".");
                return;
            }
            remember_folder(self->info_.folders_file, self->info_.import_root);
            self->rebuild();
            // Straight into the game: that is what the download was for.
            self->activate(kPlay);
        });
    });
}

void Launcher::steam_buttons() {
    auto add = [&](int id, const std::string &label, int kind = kKindNormal, Icon icon = Icon(-1)) {
        Button b{id, label, true, {}};
        b.kind = kind;
        b.icon = int(icon);
        buttons_.push_back(b);
    };
    const SteamPhase p = steam_ ? steam_state_.phase : SteamPhase::Starting;
    if (steam_staging_)
        return;
    if (!steam_) {
        // The name-and-password form, before a session.
        add(kSteamUserField, steam_user_, kKindField);
        add(kSteamPassField, std::string(steam_pass_.size(), '*'), kKindField);
        add(kSteamLogin, L("Log in", "Zaloguj"), kKindPrimary, Icon::Play);
        add(kSteamQr, L("Use a QR code instead", "Kod QR zamiast hasła"), kKindNormal, Icon::Back);
        add(kBack, L("Back", "Wróć"), kKindNormal, Icon::Back);
        return;
    }
    switch (p) {
    case SteamPhase::NeedPassword:
        add(kSteamPassField, std::string(steam_pass_.size(), '*'), kKindField);
        add(kSteamSend, L("Log in", "Zaloguj"), kKindPrimary, Icon::Play);
        add(kSteamCancel, L("Cancel", "Anuluj"), kKindNormal, Icon::Cross);
        break;
    case SteamPhase::NeedCode:
        add(kSteamCodeField, steam_code_, kKindField);
        add(kSteamSend, L("Send code", "Wyślij kod"), kKindPrimary, Icon::Check);
        add(kSteamCancel, L("Cancel", "Anuluj"), kKindNormal, Icon::Cross);
        break;
    case SteamPhase::NotOwned:
        if (!spec_.store_url.empty())
            add(kStore, L("Get ", "Kup ") + spec_.title + L(" on Steam", " na Steamie"),
                kKindPrimary, Icon::Document);
        add(kSteamOtherAccount, L("Another Steam account", "Inne konto Steam"), kKindNormal,
            Icon::Folder);
        add(kBack, L("Back", "Wróć"), kKindNormal, Icon::Back);
        break;
    case SteamPhase::LoginFailed:
        add(kSteamOtherAccount, L("Log in again", "Zaloguj ponownie"), kKindPrimary, Icon::Play);
        add(kBack, L("Back", "Wróć"), kKindNormal, Icon::Back);
        break;
    case SteamPhase::Failed:
        add(kSteamRetry, L("Try again", "Spróbuj ponownie"), kKindPrimary, Icon::Play);
        add(kBack, L("Back", "Wróć"), kKindNormal, Icon::Back);
        break;
    case SteamPhase::Done:
        break;
    default:
        if (p == SteamPhase::Qr ||
            (p == SteamPhase::Starting && steam_account_.empty() && !steam_password_mode_))
            add(kSteamPassword, L("Log in with a password", "Zaloguj loginem i hasłem"),
                kKindNormal, Icon::Document);
        if (p == SteamPhase::Downloading && !steam_account_.empty())
            add(kSteamOtherAccount, L("Another Steam account", "Inne konto Steam"), kKindBar,
                Icon::Folder);
        add(kSteamCancel,
            p == SteamPhase::Downloading ? L("Stop", "Zatrzymaj") : L("Cancel", "Anuluj"),
            kKindNormal, Icon::Cross);
        break;
    }
    // Optional parts (the Polish speech) can be switched before their turn comes.
    for (size_t i = 0; i < spec_.steam.depots.size(); ++i) {
        const SteamDepot &d = spec_.steam.depots[i];
        if (d.label.empty() || i <= steam_depot_)
            continue;
        add(kSteamToggle + int(i),
            d.label + ": " + (depot_wanted(i) ? L("yes", "tak") : L("no", "nie")), kKindBar,
            depot_wanted(i) ? Icon::Check : Icon::Cross);
    }
}

void Launcher::activate_steam(int id) {
    if (id >= kSteamToggle && id < kSteamToggle + 100) {
        const size_t i = size_t(id - kSteamToggle);
        if (i >= spec_.steam.depots.size())
            return;
        const std::string &label = spec_.steam.depots[i].label;
        auto it = std::find(steam_skipped_.begin(), steam_skipped_.end(), label);
        if (it == steam_skipped_.end())
            steam_skipped_.push_back(label);
        else
            steam_skipped_.erase(it);
        std::string text;
        for (const std::string &s : steam_skipped_)
            text += s + "\n";
        write_text_file(info_.steam_home + "/skipped.txt", text);
        rebuild();
        return;
    }
    switch (id) {
    case kSteam:
        begin_steam();
        break;
    case kSteamPassword:
        steam_.reset();
        steam_state_ = SteamState{};
        steam_password_mode_ = true;
        steam_account_.clear();
        steam_field_ = kSteamUserField;
        rebuild();
        break;
    case kSteamQr:
        steam_password_mode_ = false;
        steam_pass_.clear();
        begin_steam();
        break;
    case kSteamUserField:
    case kSteamPassField:
    case kSteamCodeField:
        steam_field_ = id;
        rebuild();
        break;
    case kSteamLogin:
        if (steam_user_.empty())
            steam_field_ = kSteamUserField;
        else if (steam_pass_.empty())
            steam_field_ = kSteamPassField;
        else {
            steam_account_.clear();
            begin_steam();
            return;
        }
        rebuild();
        break;
    case kSteamSend:
        if (!steam_)
            break;
        if (steam_state_.phase == SteamPhase::NeedPassword && !steam_pass_.empty()) {
            steam_->answer(steam_pass_);
            steam_password_sent_ = true;
            steam_pass_.clear();
        } else if (steam_state_.phase == SteamPhase::NeedCode && !steam_code_.empty()) {
            steam_->answer(steam_code_);
            steam_code_.clear();
        } else
            break;
        steam_field_ = 0;
        steam_state_.phase = SteamPhase::Starting;
        rebuild();
        break;
    case kSteamCancel:
        if (steam_ && steam_->running()) {
            steam_cancelled_ = true;
            steam_->cancel();
        } else {
            steam_.reset();
            screen_ = Screen::Main;
            rebuild();
        }
        break;
    case kSteamRetry:
        if (steam_depot_ < spec_.steam.depots.size())
            launch_depot();
        break;
    case kSteamOtherAccount:
        // Forget who was logged in: the next login is a fresh QR code.
        if (steam_)
            steam_->cancel();
        steam_.reset();
        steam_account_.clear();
        os_unlink((info_.steam_home + "/account.txt").c_str());
        steam_password_mode_ = false;
        begin_steam();
        break;
    default:
        break;
    }
}

void Launcher::steam_text(std::string *headline, std::string *sub, std::string *detail,
                          Color *accent) const {
    *accent = pal::gold;
    sub->clear();
    const SteamPhase p = steam_ ? steam_state_.phase : SteamPhase::Starting;
    if (steam_staging_) {
        *headline = L("Preparing the game…", "Przygotowanie gry…");
        *detail = L("The game files are on this Mac. A moment more and the game starts.",
                    "Pliki gry są już na tym Macu. Jeszcze chwila i gra wystartuje.");
        return;
    }
    if (!steam_) {
        *headline = L("Log in to Steam", "Logowanie do Steama");
        *detail = L(
            "Your Steam account name and password. They go to Steam only, through DepotDownloader; "
            "the app keeps neither, only Steam's login token.",
            "Nazwa konta Steam i hasło. Trafiają tylko do Steama, przez DepotDownloader; aplikacja "
            "nie "
            "zapisuje żadnego z nich, tylko token logowania Steama.");
        return;
    }
    switch (p) {
    case SteamPhase::Starting:
        *headline = L("Connecting to Steam…", "Łączenie ze Steamem…");
        *detail = steam_account_.empty() ? L("A QR code to scan appears in a moment.",
                                             "Za chwilę pojawi się kod QR do zeskanowania.")
                                         : L("Logged in as ", "Konto ") + steam_account_ + ".";
        break;
    case SteamPhase::Qr:
        *headline = L("Scan the code with the Steam app", "Zeskanuj kod aplikacją Steam");
        *detail =
            L("In the Steam app on your phone: Steam Guard (the shield), then the QR code scanner. "
              "The game downloads from your account; without ",
              "W aplikacji Steam na telefonie: Steam Guard (tarcza), potem skaner kodów QR. "
              "Gra pobierze się z twojego konta; bez ") +
            spec_.title +
            L(" on it there is nothing to download.", " na koncie nie ma czego pobrać.");
        break;
    case SteamPhase::NeedPassword:
        *headline = L("Steam password", "Hasło do Steama");
        *detail = L("The stored login has expired. Type the password of ",
                    "Zapisane logowanie wygasło. Wpisz hasło konta ") +
                  (steam_account_.empty() ? steam_user_ : steam_account_) + ".";
        break;
    case SteamPhase::NeedCode:
        *headline = L("Steam Guard code", "Kod Steam Guard");
        *detail = steam_state_.email ? L("Steam has mailed you a code. Type it here.",
                                         "Steam wysłał ci kod mailem. Wpisz go tutaj.")
                                     : L("The code from the Steam app (Steam Guard).",
                                         "Kod z aplikacji Steam (Steam Guard).");
        if (steam_state_.code_rejected)
            *detail = L("That code was wrong. ", "Ten kod był błędny. ") + *detail;
        break;
    case SteamPhase::ConfirmInApp:
        *headline =
            L("Approve the login in the Steam app", "Potwierdź logowanie w aplikacji Steam");
        *detail =
            L("Steam has sent a request to your phone.", "Steam wysłał prośbę na twój telefon.");
        break;
    case SteamPhase::Downloading: {
        *headline = L("Downloading ", "Pobieranie ") + spec_.title + "…";
        const SteamDepot &d = spec_.steam.depots[steam_depot_];
        *detail = (d.label.empty() ? L("Game files", "Pliki gry") : d.label) + ": " +
                  std::to_string(int(steam_state_.percent)) + "%";
        if (!steam_state_.file.empty()) {
            std::string f = steam_state_.file;
            const size_t at = f.rfind('/');
            *detail += ", " + (at == std::string::npos ? f : f.substr(at + 1));
        }
        *detail += L("\nStopping is safe: the next download continues where it stopped.",
                     "\nMożna przerwać: następne pobieranie zacznie tam, gdzie się zatrzymało.");
        break;
    }
    case SteamPhase::Done:
        *headline = L("Downloaded", "Pobrane");
        break;
    case SteamPhase::NotOwned:
        *accent = pal::red;
        *headline = L("This account has no ", "To konto nie ma ") + spec_.title;
        *detail = (steam_account_.empty() ? L("Steam says the game is not on this account",
                                              "Steam mówi, że gry nie ma na tym koncie")
                                          : L("Steam says the game is not on ",
                                              "Steam mówi, że gry nie ma na koncie ") +
                                                steam_account_) +
                  L(". Buy it on Steam, or log in to the account that has it.",
                    ". Kup ją na Steamie albo zaloguj się na konto, które ją ma.");
        break;
    case SteamPhase::LoginFailed:
        *accent = pal::red;
        *headline = L("Steam did not let you in", "Steam nie wpuścił");
        *detail = steam_state_.detail.find("InvalidPassword") != std::string::npos
                      ? L("Wrong account name or password.", "Zła nazwa konta albo hasło.")
                      : steam_state_.detail;
        break;
    case SteamPhase::Failed:
        *accent = pal::amber;
        *headline = L("The download stopped", "Pobieranie się przerwało");
        *detail = steam_state_.detail + L("\nTrying again continues where it stopped.",
                                          "\nPonowna próba zacznie tam, gdzie się zatrzymało.");
        break;
    }
}

// The fields and the progress bar under the text.
float Launcher::steam_extras_height(float s) const {
    float h = 0;
    for (const Button &b : buttons_)
        if (b.kind == kKindField)
            h += 56 * s;
    if (steam_staging_ || screen_ == Screen::Update ||
        (steam_ && steam_state_.phase == SteamPhase::Downloading))
        h += 30 * s;
    return h;
}

void Launcher::draw_steam_extras(Canvas &c, float x0, float col_w, float s, float *y) {
    Look &k = *look_;
    const int px = P(16 * s);
    for (size_t i = 0; i < buttons_.size(); ++i) {
        Button &b = buttons_[i];
        if (b.kind != kKindField)
            continue;
        *y += 8 * s;
        b.rect = {P(x0 + 20 * s), P(*y), P(col_w - 20 * s), P(42 * s)};
        const bool active = steam_field_ == b.id;
        c.fill_round(b.rect, P(4), hex(0x0a0807, 210));
        c.stroke_round(b.rect, P(4), std::max(1, P(active ? 1.5f : 1)),
                       active ? pal::gold
                              : mix(pal::border_hi, pal::gold_deep, 0.5f + 0.5f * hot(int(i))));
        const int baseline = b.rect.y + b.rect.h / 2 + (k.body.ascent(px) - k.body.descent(px)) / 2;
        const char *hint = b.id == kSteamUserField   ? "Steam account name|Nazwa konta Steam"
                           : b.id == kSteamPassField ? "Password|Hasło"
                                                     : "Code|Kod";
        const std::string h = hint;
        const std::string placeholder =
            L(h.substr(0, h.find('|')).c_str(), h.substr(h.find('|') + 1).c_str());
        const std::string text = b.label + (active ? "|" : "");
        if (b.label.empty() && !active)
            k.body.draw(c, b.rect.x + P(14 * s), baseline, placeholder, px, pal::dim);
        else
            k.body.draw(c, b.rect.x + P(14 * s), baseline, text, px, pal::text);
        *y += 48 * s;
    }
    if (steam_staging_ || screen_ == Screen::Update ||
        (steam_ && steam_state_.phase == SteamPhase::Downloading)) {
        *y += 8 * s;
        const Rect bar{P(x0 + 20 * s), P(*y), P(col_w - 20 * s), P(10 * s)};
        c.fill_round(bar, bar.h / 2, hex(0x000000, 160));
        c.stroke_round(bar, bar.h / 2, std::max(1, P(1)), fade(pal::gold_deep, 0.7f));
        double frac = steam_overall();
        if (screen_ == Screen::Update) {
            std::lock_guard<std::mutex> lock(shared_->m);
            frac = shared_->progress.bytes_total ? double(shared_->progress.bytes_done) /
                                                       double(shared_->progress.bytes_total)
                                                 : 0.0;
        } else if (steam_staging_) {
            std::lock_guard<std::mutex> lock(shared_->m);
            frac = shared_->progress.files_total ? double(shared_->progress.files_done) /
                                                       double(shared_->progress.files_total)
                                                 : 0.0;
        }
        c.fill_round({bar.x, bar.y, std::max(bar.h, int(bar.w * std::min(1.0, frac))), bar.h},
                     bar.h / 2, pal::gold);
        *y += 22 * s;
    }
}

// The QR code on the right of the column, on a light square as phones expect.
void Launcher::draw_steam_qr(Canvas &c, float W, float H, float bar_h, float col_w, float Lm) {
    if (!steam_ || steam_state_.phase != SteamPhase::Qr || steam_state_.qr.empty())
        return;
    const auto &qr = steam_state_.qr;
    const int n = int(qr.size());
    const float room_w = W - col_w - 3 * Lm, room_h = H - bar_h - 2 * Lm;
    float q = std::min({room_w, room_h, 420.0f});
    float x = W - Lm - q, y = (H - bar_h - q) / 2;
    if (q < 150) { // no room beside the column: over the backdrop's top right instead
        q = std::min(W, H) * 0.42f;
        x = W - q - 16;
        y = 16;
    }
    const int side = P(q), module = std::max(1, side / n), used = module * n;
    const int ox = P(x) + (side - used) / 2, oy = P(y) + (side - used) / 2;
    c.fill_round({ox - module, oy - module, used + 2 * module, used + 2 * module}, P(8),
                 hex(0xfaf6ec));
    for (int r = 0; r < n; ++r)
        for (int col = 0; col < int(qr[size_t(r)].size()); ++col)
            if (qr[size_t(r)][size_t(col)])
                c.fill({ox + col * module, oy + r * module, module, module}, hex(0x101010));
}

// Versions as dotted numbers ("0.2.1"); anything else (a development build's git hash) is never updated.
static bool numeric_version(const std::string &v) {
    if (v.empty() || !isdigit((unsigned char)v[0]))
        return false;
    for (char ch : v)
        if (!isdigit((unsigned char)ch) && ch != '.')
            return false;
    return true;
}
static int compare_versions(const std::string &a, const std::string &b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        unsigned long x = 0, y = 0;
        while (i < a.size() && a[i] != '.')
            x = x * 10 + unsigned(a[i++] - '0');
        while (j < b.size() && b[j] != '.')
            y = y * 10 + unsigned(b[j++] - '0');
        if (x != y)
            return x < y ? -1 : 1;
        ++i, ++j;
    }
    return 0;
}

void Launcher::check_for_update() {
    std::string current = version_;
    if (!current.empty() && (current[0] == 'v' || current[0] == 'V'))
        current.erase(0, 1);
    if (spec_.releases_repo.empty() || spec_.update_asset.empty() || !numeric_version(current))
        return;
    auto weak = std::weak_ptr<Shared>(shared_);
    Launcher *self = this;
    platform_.check_update(
        spec_.releases_repo, spec_.update_asset, [weak, self, current](bool ok, UpdateInfo info) {
            auto shared = weak.lock();
            if (!shared)
                return;
            if (!info.version.empty() && (info.version[0] == 'v' || info.version[0] == 'V'))
                info.version.erase(0, 1);
            if (!ok || !numeric_version(info.version) ||
                compare_versions(info.version, current) <= 0)
                return;
            std::lock_guard<std::mutex> lock(shared->m);
            shared->posted.push_back([self, info]() {
                fprintf(stderr, "[launcher] update: %s is out\n", info.version.c_str());
                self->update_ = info;
                self->update_known_ = true;
                if (self->screen_ == Screen::Main)
                    self->rebuild();
            });
        });
}

void Launcher::begin_update() {
    if (!update_known_)
        return;
    // Only this repository's own release files, over HTTPS: the app is signed ad hoc, so its signature
    // proves the archive whole, not who made it.
    const std::string from = "https://github.com/" + spec_.releases_repo + "/releases/download/";
    if (update_.url.compare(0, from.size(), from) != 0) {
        show(L("The update is not where this app's releases are: ",
               "Aktualizacja nie leży tam, gdzie wydania tej "
               "aplikacji: ") +
             update_.url);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(shared_->m);
        shared_->progress = Progress{};
        shared_->progress.bytes_total = update_.size;
    }
    auto weak = std::weak_ptr<Shared>(shared_);
    Launcher *self = this;
    const uint64_t size = update_.size;
    const bool started = platform_.install_update(
        update_,
        [weak, size](double fraction) {
            auto shared = weak.lock();
            if (!shared)
                return;
            std::lock_guard<std::mutex> lock(shared->m);
            shared->progress.bytes_total = size ? size : 1000;
            shared->progress.bytes_done = uint64_t(fraction * double(shared->progress.bytes_total));
        },
        [weak, self](bool ok, std::string error) {
            auto shared = weak.lock();
            if (!shared)
                return;
            std::lock_guard<std::mutex> lock(shared->m);
            shared->posted.push_back([self, ok, error]() {
                if (ok) {
                    self->platform_.relaunch();
                    self->quit_ = true;
                    return;
                }
                self->screen_ = Screen::Main;
                self->rebuild();
                self->show(self->L("The update could not be installed: ",
                                   "Nie udało się zainstalować aktualizacji: ") +
                           error +
                           self->L("\nThe release page is open in the browser.",
                                   "\nStrona wydania jest otwarta w przeglądarce."));
                self->platform_.open_url(self->update_.page);
            });
        });
    if (!started) {
        platform_.open_url(update_.page);
        return;
    }
    screen_ = Screen::Update;
    rebuild();
}

std::string *Launcher::steam_field_text() {
    switch (steam_field_) {
    case kSteamUserField:
        return &steam_user_;
    case kSteamPassField:
        return &steam_pass_;
    case kSteamCodeField:
        return &steam_code_;
    default:
        return nullptr;
    }
}

bool Launcher::steam_type(const std::string &utf8) {
    std::string *t = screen_ == Screen::Steam ? steam_field_text() : nullptr;
    if (!t)
        return false;
    for (char ch : utf8)
        if ((unsigned char)ch < 0x20 || (unsigned char)ch >= 0x7f)
            return true; // Steam takes ASCII names, passwords and codes only
    *t += steam_field_ == kSteamCodeField ? [&] {
        std::string up = utf8;
        for (char &ch : up)
            ch = char(toupper((unsigned char)ch));
        return up;
    }()
                                          : utf8;
    rebuild();
    return true;
}

bool Launcher::steam_erase() {
    std::string *t = screen_ == Screen::Steam ? steam_field_text() : nullptr;
    if (!t)
        return false;
    if (!t->empty())
        t->pop_back();
    rebuild();
    return true;
}

} // namespace launcher
