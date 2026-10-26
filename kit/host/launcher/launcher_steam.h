// launcher_steam.h - the game from the player's own Steam account, through
// DepotDownloader (SteamRE, GPL-2.0) run as a separate program beside the
// app's executable (Gothic 3, D-037). Portable: no SDL, no UI. The launcher
// screen (launcher_ui.cpp) shows what SteamOutput reads and answers its
// prompts through SteamSession.
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace launcher {

// One depot to download ([[launcher.steam.depots]] in game.toml).
struct SteamDepot {
    uint32_t id = 0;
    std::string manifest, branch;
    std::vector<std::string> files; // -filelist lines; empty: the whole depot
    uint64_t mb = 0;                // its size, which weighs its share of the progress bar
    std::string label;              // "" always downloaded, else a part the player may leave out
    // Edits of the game's .ini files when this part is downloaded, or left out (apply_ini_edits).
    std::vector<std::string> ini_on, ini_off;
};
struct SteamSpec {
    std::string tool; // DepotDownloader's file name beside the executable; "" = no Steam download
    uint32_t app = 0;
    std::vector<SteamDepot> depots;
    std::vector<std::string> ini; // edits after every download, whatever was left out
};
const SteamSpec &steam_spec_from_config();

enum class SteamPhase {
    Starting,     // connecting, logging in, asking for app info
    Qr,           // a code for the Steam app to scan (SteamState::qr)
    NeedPassword, // the account's password
    NeedCode,     // a Steam Guard code: from the authenticator, or mailed (SteamState::email)
    ConfirmInApp, // waiting for the sign-in to be approved in the Steam app
    Downloading,
    Done,        // the depot is on disk
    NotOwned,    // the account has no licence for the app or the depot
    LoginFailed, // wrong password, a rejected code or token, too many attempts
    Failed,      // anything else (no connection, a missing manifest, the tool would not start)
};
const char *steam_phase_name(SteamPhase p);

struct SteamState {
    SteamPhase phase = SteamPhase::Starting;
    // The QR code, row by row, true = a dark module; it changes as Steam refreshes it.
    std::vector<std::vector<bool>> qr;
    int qr_serial = 0;          // counts the codes shown
    bool email = false;         // NeedCode: the code was mailed (else: the authenticator's)
    bool code_rejected = false; // the previous code was wrong
    double percent = 0;         // of this depot's download, 0..100
    std::string file;           // the file the last progress line named
    std::string account;        // the account a QR login signed in to, when the tool said
    std::string detail;         // why it failed, or the last line worth showing
    int exit_code = -1;         // the tool's, once it ended
};

// DepotDownloader's console as states. Feed it whatever the tool prints, in
// any pieces: whole lines are read as they complete, and a prompt the tool
// leaves on an unfinished line (the password, a code) is read without
// waiting for its newline.
class SteamOutput {
  public:
    void feed(const std::string &bytes);
    // The tool ended with `exit_code`: Done or a failure, unless a line already said.
    void finish(int exit_code);
    // The player sent what was asked: back to waiting until the tool says more.
    void answered();
    const SteamState &state() const {
        return state_;
    }
    // The last lines it printed, for a bug report.
    const std::vector<std::string> &tail() const {
        return tail_;
    }

  private:
    void line(const std::string &text);
    void prompt(const std::string &text);
    std::string pending_;
    std::vector<std::vector<bool>> qr_rows_;
    size_t qr_width_ = 0;
    bool in_qr_ = false;
    bool total_seen_ = false;
    std::vector<std::string> tail_;
    SteamState state_;
};

// The arguments for one depot. `username` with remember: a stored login (or a
// password the tool asks for); empty: a QR login.
std::vector<std::string> steam_arguments(const SteamSpec &spec, const SteamDepot &depot,
                                         const std::string &dir, const std::string &filelist,
                                         const std::string &username);

// The account names DepotDownloader keeps a login token for, read from its
// account.config under `home` (the HOME it ran with): a raw DEFLATE stream of
// a protobuf whose field 4 maps account names to tokens. Empty when there is
// none or it cannot be read.
std::vector<std::string> steam_saved_accounts(const std::string &home);
std::vector<std::string> steam_saved_accounts_from_bytes(const std::vector<uint8_t> &file);

// DepotDownloader running for one depot. Its output is read on a thread of its
// own; state() is safe from any thread.
class SteamSession {
  public:
    SteamSession() = default;
    ~SteamSession();
    SteamSession(const SteamSession &) = delete;
    SteamSession &operator=(const SteamSession &) = delete;
    // tool: the executable's path; home: HOME for it, where it keeps the
    // login; dir: where the files go. false with *error when it cannot start.
    bool start(const std::string &tool, const std::vector<std::string> &args,
               const std::string &home, std::string *error);
    // A password or a code, as a line on the tool's input.
    void answer(const std::string &text);
    void cancel();
    bool running() const;
    SteamState state() const;
    std::vector<std::string> tail() const;

  private:
    struct Shared;
    std::shared_ptr<Shared> shared_;
};

} // namespace launcher
