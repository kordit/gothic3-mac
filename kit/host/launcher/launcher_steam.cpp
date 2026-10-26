// launcher_steam.cpp - see launcher_steam.h. What the parser expects is what
// DepotDownloader 3.4.0 prints (Program.cs, Steam3Session.cs,
// ConsoleAuthenticator.cs, ContentDownloader.cs at tag DepotDownloader_3.4.0).
#include "launcher_steam.h"

#include "../../platform/os.h"
#include "../../third_party/miniz/miniz.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace launcher {

const char *steam_phase_name(SteamPhase p) {
    switch (p) {
    case SteamPhase::Starting:
        return "starting";
    case SteamPhase::Qr:
        return "qr";
    case SteamPhase::NeedPassword:
        return "need-password";
    case SteamPhase::NeedCode:
        return "need-code";
    case SteamPhase::ConfirmInApp:
        return "confirm-in-app";
    case SteamPhase::Downloading:
        return "downloading";
    case SteamPhase::Done:
        return "done";
    case SteamPhase::NotOwned:
        return "not-owned";
    case SteamPhase::LoginFailed:
        return "login-failed";
    case SteamPhase::Failed:
        return "failed";
    }
    return "?";
}

namespace {

bool starts_with(const std::string &s, const char *prefix) {
    return s.compare(0, strlen(prefix), prefix) == 0;
}
bool contains(const std::string &s, const char *part) {
    return s.find(part) != std::string::npos;
}
std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
        --b;
    return s.substr(a, b - a);
}
bool terminal(SteamPhase p) {
    return p == SteamPhase::Done || p == SteamPhase::NotOwned || p == SteamPhase::LoginFailed ||
           p == SteamPhase::Failed;
}

// One line of QRCoder's AsciiQRCode (two characters a module, "██" dark and
// two spaces light) as modules; false when the line is something else.
const char kBlock[] = "\xe2\x96\x88"; // U+2588 FULL BLOCK
bool qr_row(const std::string &text, std::vector<bool> *row) {
    std::vector<bool> half;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == ' ') {
            half.push_back(false);
            ++i;
        } else if (text.compare(i, 3, kBlock) == 0) {
            half.push_back(true);
            i += 3;
        } else
            return false;
    }
    if (half.empty() || half.size() % 2)
        return false;
    row->clear();
    for (size_t i = 0; i < half.size(); i += 2) {
        if (half[i] != half[i + 1])
            return false;
        row->push_back(half[i]);
    }
    return true;
}

// "  45.67% path" (the tool formats with the current culture: "45,67" too).
bool progress_line(const std::string &text, double *percent, std::string *file) {
    size_t i = 0;
    while (i < text.size() && text[i] == ' ')
        ++i;
    const size_t start = i;
    while (i < text.size() &&
           ((text[i] >= '0' && text[i] <= '9') || text[i] == '.' || text[i] == ','))
        ++i;
    if (i == start || i + 1 >= text.size() || text[i] != '%' || text[i + 1] != ' ')
        return false;
    std::string number = text.substr(start, i - start);
    std::replace(number.begin(), number.end(), ',', '.');
    char *end = nullptr;
    const double v = strtod(number.c_str(), &end);
    if (!end || *end)
        return false;
    *percent = std::clamp(v, 0.0, 100.0);
    *file = text.substr(i + 2);
    return true;
}

} // namespace

void SteamOutput::feed(const std::string &bytes) {
    pending_ += bytes;
    for (size_t nl; (nl = pending_.find('\n')) != std::string::npos;) {
        std::string text = pending_.substr(0, nl);
        pending_.erase(0, nl + 1);
        if (!text.empty() && text.back() == '\r')
            text.pop_back();
        line(text);
    }
    // A question waits on its line for the answer.
    if (!pending_.empty())
        prompt(pending_);
}

void SteamOutput::prompt(const std::string &text) {
    if (terminal(state_.phase))
        return;
    if (starts_with(text, "Enter account password for")) {
        state_.phase = SteamPhase::NeedPassword;
    } else if (contains(text, "2-factor auth code from your authenticator app") ||
               contains(text, "2 factor auth code from your authenticator app")) {
        state_.phase = SteamPhase::NeedCode;
        state_.email = false;
    } else if (contains(text, "auth code sent to the email") ||
               contains(text, "authentication code sent to your email")) {
        state_.phase = SteamPhase::NeedCode;
        state_.email = true;
    }
}

void SteamOutput::line(const std::string &raw) {
    if (in_qr_) {
        std::vector<bool> row;
        if (qr_row(raw, &row) && (qr_width_ == 0 || row.size() == qr_width_)) {
            qr_width_ = row.size();
            qr_rows_.push_back(row);
            if (qr_rows_.size() == qr_width_) {
                in_qr_ = false;
                state_.qr = qr_rows_;
                ++state_.qr_serial;
                if (!terminal(state_.phase))
                    state_.phase = SteamPhase::Qr;
            }
            return;
        }
        in_qr_ = false; // not a code after all
    }
    const std::string text = trim(raw);
    if (text.empty())
        return;
    tail_.push_back(text);
    if (tail_.size() > 40)
        tail_.erase(tail_.begin());
    if (terminal(state_.phase))
        return;
    double percent = 0;
    std::string file;
    if (progress_line(raw, &percent, &file)) {
        state_.phase = SteamPhase::Downloading;
        state_.percent = percent;
        state_.file = file;
        return;
    }
    if (contains(text, "sign in with this QR code")) {
        in_qr_ = true;
        qr_rows_.clear();
        qr_width_ = 0;
        return;
    }
    if (contains(text, "is not available from this account")) {
        state_.phase = SteamPhase::NotOwned;
        state_.detail = text;
        return;
    }
    if (starts_with(text, "Failed to authenticate with Steam:") ||
        starts_with(text, "Unable to login to Steam3:") ||
        starts_with(text, "Access token was rejected")) {
        state_.phase = SteamPhase::LoginFailed;
        state_.detail = text;
        return;
    }
    if (contains(text, "previous 2-factor auth code you have provided is incorrect")) {
        state_.code_rejected = true;
        return;
    }
    if (contains(text, "Use the Steam Mobile App to confirm your sign in")) {
        state_.phase = SteamPhase::ConfirmInApp;
        return;
    }
    const char *const success = "Success! Next time you can login with -username ";
    if (starts_with(text, success)) {
        const std::string rest = text.substr(strlen(success));
        state_.account = rest.substr(0, rest.find(' '));
        return;
    }
    if (contains(text, "licenses for account")) {
        // Signed in: whatever was asked has been answered.
        state_.phase = SteamPhase::Starting;
        state_.code_rejected = false;
        return;
    }
    if (starts_with(text, "Downloading depot") || starts_with(text, "Pre-allocating ") ||
        starts_with(text, "Validating ")) {
        state_.phase = SteamPhase::Downloading;
        return;
    }
    if (starts_with(text, "Total downloaded:")) {
        total_seen_ = true;
        state_.percent = 100;
        return;
    }
    if (starts_with(text, "Encountered") || starts_with(text, "Error") ||
        starts_with(text, "Download failed") || starts_with(text, "Could not connect") ||
        starts_with(text, "Unable to") || starts_with(text, "No manifest request code") ||
        contains(text, "was not found") || starts_with(text, "Warning: Unable to load filelist") ||
        starts_with(text, "Failed to")) {
        state_.detail = text;
        return;
    }
    // A question the tool answered itself or finished with a newline.
    prompt(text);
}

void SteamOutput::finish(int exit_code) {
    if (!pending_.empty()) {
        const std::string rest = pending_;
        pending_.clear();
        line(rest);
    }
    state_.exit_code = exit_code;
    if (terminal(state_.phase))
        return;
    if (exit_code == 0 && total_seen_) {
        state_.phase = SteamPhase::Done;
        return;
    }
    state_.phase = SteamPhase::Failed;
    if (state_.detail.empty())
        state_.detail = tail_.empty() ? "DepotDownloader exited with " + std::to_string(exit_code)
                                      : tail_.back();
}

void SteamOutput::answered() {
    if (terminal(state_.phase))
        return;
    state_.phase = SteamPhase::Starting;
    state_.code_rejected = false;
    pending_.clear();
}

std::vector<std::string> steam_arguments(const SteamSpec &spec, const SteamDepot &depot,
                                         const std::string &dir, const std::string &filelist,
                                         const std::string &username) {
    std::vector<std::string> a = {"-app", std::to_string(spec.app), "-depot",
                                  std::to_string(depot.id)};
    if (!depot.manifest.empty())
        a.insert(a.end(), {"-manifest", depot.manifest});
    if (!depot.branch.empty())
        a.insert(a.end(), {"-branch", depot.branch});
    // The game is Windows': without -os the tool picks the depots for the Mac it runs on.
    a.insert(a.end(), {"-os", "windows", "-dir", dir});
    if (!filelist.empty())
        a.insert(a.end(), {"-filelist", filelist});
    if (username.empty())
        a.push_back("-qr");
    else
        a.insert(a.end(), {"-username", username});
    a.push_back("-remember-password");
    return a;
}

// ---------------------------------------------------------------------------
// account.config
// ---------------------------------------------------------------------------
namespace {

bool varint(const std::vector<uint8_t> &b, size_t *at, uint64_t *v) {
    *v = 0;
    for (int shift = 0; shift < 64 && *at < b.size(); shift += 7) {
        const uint8_t byte = b[(*at)++];
        *v |= uint64_t(byte & 0x7f) << shift;
        if (!(byte & 0x80))
            return true;
    }
    return false;
}

// Calls `field(number, bytes)` for each length-delimited field of a message
// and skips the rest; false when the bytes are not a message.
bool fields(const std::vector<uint8_t> &b, size_t from, size_t to,
            const std::function<void(uint64_t, size_t, size_t)> &field) {
    size_t at = from;
    while (at < to) {
        uint64_t key = 0, n = 0;
        if (!varint(b, &at, &key))
            return false;
        switch (key & 7) {
        case 0:
            if (!varint(b, &at, &n))
                return false;
            break;
        case 1:
            at += 8;
            break;
        case 2:
            if (!varint(b, &at, &n) || n > to - at)
                return false;
            field(key >> 3, at, at + size_t(n));
            at += size_t(n);
            break;
        case 5:
            at += 4;
            break;
        default:
            return false;
        }
    }
    return at == to;
}

void find_files(const std::string &dir, const char *name, int depth,
                std::vector<std::string> *out) {
    if (depth > 8)
        return;
    struct Ctx {
        std::vector<std::string> names;
    } ctx;
    os_listdir(
        dir.c_str(),
        [](const char *n, void *arg) {
            static_cast<Ctx *>(arg)->names.push_back(n);
            return 0;
        },
        &ctx);
    for (const std::string &n : ctx.names) {
        if (n == "." || n == "..")
            continue;
        const std::string path = dir + "/" + n;
        OsStat st{};
        if (os_lstat(path.c_str(), &st) != 0 || st.is_symlink)
            continue;
        if (st.is_dir)
            find_files(path, name, depth + 1, out);
        else if (n == name)
            out->push_back(path);
    }
}

} // namespace

std::vector<std::string> steam_saved_accounts_from_bytes(const std::vector<uint8_t> &file) {
    std::vector<std::string> names;
    if (file.empty())
        return names;
    size_t size = 0;
    void *raw = tinfl_decompress_mem_to_heap(file.data(), file.size(), &size, 0);
    if (!raw)
        return names;
    const std::vector<uint8_t> msg(static_cast<uint8_t *>(raw), static_cast<uint8_t *>(raw) + size);
    mz_free(raw);
    // AccountSettingsStore: 4 = LoginTokens, a map; each entry 1 = name, 2 = token.
    fields(msg, 0, msg.size(), [&](uint64_t number, size_t a, size_t b) {
        if (number != 4)
            return;
        fields(msg, a, b, [&](uint64_t n, size_t x, size_t y) {
            if (n == 1)
                names.emplace_back(reinterpret_cast<const char *>(msg.data() + x), y - x);
        });
    });
    return names;
}

std::vector<std::string> steam_saved_accounts(const std::string &home) {
    // .NET's isolated storage: <LocalApplicationData>/IsolatedStorage/<hash>/<hash>/.../account.config.
    std::vector<std::string> files, names;
    find_files(home, "account.config", 0, &files);
    for (const std::string &path : files) {
        std::vector<uint8_t> bytes;
        int fd = os_fd_open(path.c_str(), OS_O_RDONLY);
        if (fd < 0)
            continue;
        uint8_t buf[4096];
        for (int64_t n; (n = os_fd_read(fd, buf, sizeof buf)) > 0;)
            bytes.insert(bytes.end(), buf, buf + n);
        os_fd_close(fd);
        for (const std::string &n : steam_saved_accounts_from_bytes(bytes))
            if (std::find(names.begin(), names.end(), n) == names.end())
                names.push_back(n);
    }
    return names;
}

// ---------------------------------------------------------------------------
// SteamSession
// ---------------------------------------------------------------------------
struct SteamSession::Shared {
    mutable std::mutex m;
    SteamOutput out;
    int64_t pid = 0;
    int in_fd = -1, out_fd = -1;
    bool running = false;
};

SteamSession::~SteamSession() {
    cancel();
}

bool SteamSession::start(const std::string &tool, const std::vector<std::string> &args,
                         const std::string &home, std::string *error) {
    cancel();
    auto shared = std::make_shared<Shared>();
    std::vector<const char *> argv = {tool.c_str()};
    for (const std::string &a : args)
        argv.push_back(a.c_str());
    argv.push_back(nullptr);
    // The login token lives in the app's own data, and the tool's text is not translated.
    const std::string home_var = "HOME=" + home;
    const char *env[] = {home_var.c_str(), "DOTNET_CLI_TELEMETRY_OPTOUT=1",           "LC_ALL=C",
                         "LANG=C",         "DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1", nullptr};
    if (os_spawn_piped(argv.data(), env, &shared->pid, &shared->in_fd, &shared->out_fd) != 0) {
        if (error)
            *error = "cannot start " + tool;
        return false;
    }
    shared->running = true;
    shared_ = shared;
    auto *heap = new std::shared_ptr<Shared>(shared);
    OsThread *t = os_thread_create(
        [](void *arg) -> void * {
            std::shared_ptr<Shared> s = *static_cast<std::shared_ptr<Shared> *>(arg);
            delete static_cast<std::shared_ptr<Shared> *>(arg);
            char buf[4096];
            for (int64_t n; (n = os_fd_read(s->out_fd, buf, sizeof buf)) > 0;) {
                std::lock_guard<std::mutex> lock(s->m);
                s->out.feed(std::string(buf, size_t(n)));
            }
            int code = -1;
            os_wait(s->pid, &code);
            std::lock_guard<std::mutex> lock(s->m);
            os_fd_close(s->out_fd);
            os_fd_close(s->in_fd);
            s->out_fd = s->in_fd = -1;
            s->out.finish(code);
            s->running = false;
            return nullptr;
        },
        heap, 0);
    if (!t) {
        delete heap;
        os_kill(shared->pid);
        if (error)
            *error = "cannot start a thread for " + tool;
        return false;
    }
    os_thread_detach(t);
    return true;
}

void SteamSession::answer(const std::string &text) {
    if (!shared_)
        return;
    std::lock_guard<std::mutex> lock(shared_->m);
    if (shared_->in_fd < 0)
        return;
    const std::string line = text + "\n";
    os_fd_write(shared_->in_fd, line.data(), line.size());
    shared_->out.answered();
}

void SteamSession::cancel() {
    if (!shared_)
        return;
    std::lock_guard<std::mutex> lock(shared_->m);
    if (shared_->running)
        os_kill(shared_->pid);
}

bool SteamSession::running() const {
    if (!shared_)
        return false;
    std::lock_guard<std::mutex> lock(shared_->m);
    return shared_->running;
}

SteamState SteamSession::state() const {
    if (!shared_)
        return SteamState{};
    std::lock_guard<std::mutex> lock(shared_->m);
    return shared_->out.state();
}

std::vector<std::string> SteamSession::tail() const {
    if (!shared_)
        return {};
    std::lock_guard<std::mutex> lock(shared_->m);
    return shared_->out.tail();
}

} // namespace launcher
