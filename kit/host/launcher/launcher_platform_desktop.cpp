// launcher_platform_desktop.cpp - the launcher's platform on macOS, Windows
// and Linux: SDL's native dialogs (the desktop portal on Flatpak), installs
// played where they are, and a copy in the profile when the player asks.
#include "launcher_sdl.h"

#include "../../platform/os.h"
#include "../../runtime/layout.h"
#include "game_config.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

#ifdef __APPLE__
// host/sdl/os_update_macos.mm
extern "C" int os_update_fetch_release(const char *repo, const char *asset,
                                       void (*done)(void *ctx, int ok, const char *tag,
                                                    const char *url, unsigned long long size,
                                                    const char *page),
                                       void *ctx);
extern "C" int os_trash_path(const char *path);
extern "C" int os_bundle_identifier(const char *bundle_path, char *out, size_t cap);
#endif

#ifdef RECOMP_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#endif

namespace launcher {
namespace {

struct Pending {
    PickDone done;
    bool folder;
};

void SDLCALL on_dialog(void *userdata, const char *const *files, int) {
    std::unique_ptr<Pending> pending(static_cast<Pending *>(userdata));
    std::vector<Picked> picked;
    std::string error;
    if (!files)
        error = SDL_GetError();
    else
        for (const char *const *f = files; *f; ++f) {
            Picked p;
            p.path = *f;
            p.name = *f;
            picked.push_back(p);
        }
    pending->done(picked, error);
}

std::string file_url(const std::string &path) {
    std::string url = "file://";
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || c == ':')
            url.push_back(char(c));
        else {
            char hex[4];
            snprintf(hex, sizeof hex, "%%%02X", c);
            url += hex;
        }
    }
#ifdef _WIN32
    if (url.size() > 7 && url[7] != '/')
        url.insert(7, "/");
#endif
    return url;
}

// The .app holding this executable ("" when it is not in one).
std::string own_bundle() {
    char exe[4096];
    if (os_exe_path(exe, sizeof exe) != 0)
        return "";
    const std::string path = exe;
    const size_t at = path.rfind(".app/Contents/MacOS/");
    return at == std::string::npos ? "" : path.substr(0, at + 4);
}

int run_tool(std::vector<std::string> args) {
    std::vector<const char *> argv;
    for (const std::string &a : args)
        argv.push_back(a.c_str());
    argv.push_back(nullptr);
    int64_t pid = 0;
    int code = -1;
    if (os_spawn(argv.data(), &pid) != 0 || os_wait(pid, &code) != 0)
        return -1;
    return code;
}

int64_t file_size(const std::string &path) {
    OsStat st{};
    return os_stat(path.c_str(), &st) == 0 ? int64_t(st.size) : -1;
}

std::string home_dir() {
    const char *h = getenv("HOME");
    return h ? h : "";
}

class DesktopPlatform final : public Platform {
  public:
    explicit DesktopPlatform(SDL_Window *window) : window_(window) {}

    PlatformInfo info() override {
        PlatformInfo i;
        const std::string profile = host_layout().profile_dir;
        i.plays_in_place = true;
        i.can_pick_folder = true;
        i.can_pick_zip = true;
        i.can_open_folder = true;
        i.touch = false;
        i.profile_dir = profile;
        i.import_root = profile + "/game";
        i.folders_file = profile + "/launcher-folders.txt";
        i.cache_file = profile + "/launcher-verified.txt";
        i.drop_hint = spec_from_config().language == "pl"
                          ? "Folder gry albo jego ZIP można też upuścić na to okno."
                          : "You can also drop the game folder or a ZIP of it on this window.";
        // The game's launcher art, packaged from its repository's launcher/.
        i.art_dir = host_resource("launcher");
        // DepotDownloader beside the executable (Contents/MacOS in the app), when the game uses one.
        const std::string tool = spec_from_config().steam.tool;
        char exe[4096];
        if (!tool.empty() && os_exe_path(exe, sizeof exe) == 0) {
            std::string dir = exe;
            dir.erase(dir.find_last_of('/') + 1);
            OsStat st{};
            if (os_stat((dir + tool).c_str(), &st) == 0 && st.is_regular)
                i.steam_tool = dir + tool;
        }
        i.steam_home = profile + "/steam";
        return i;
    }
    void pick_folder(PickDone done) override {
        SDL_ShowOpenFolderDialog(on_dialog, new Pending{std::move(done), true}, window_, nullptr,
                                 false);
    }
    void pick_zip(PickDone done) override {
        static const SDL_DialogFileFilter filters[] = {{"ZIP archive", "zip"}};
        SDL_ShowOpenFileDialog(on_dialog, new Pending{std::move(done), false}, window_, filters, 1,
                               nullptr, false);
    }
    void pick_export(const std::string &suggested, PickDone done) override {
        static const SDL_DialogFileFilter filters[] = {{"ZIP archive", "zip"}};
        SDL_ShowSaveFileDialog(on_dialog, new Pending{std::move(done), false}, window_, filters, 1,
                               suggested.c_str());
    }
    void pick_saves(PickDone done) override {
        pick_zip(std::move(done));
    }
    std::vector<std::string> candidates(const Spec &spec) override {
        return detect_installs(spec);
    }
    void open_folder(const std::string &path) override {
        SDL_OpenURL(file_url(path).c_str());
    }
    void open_url(const std::string &url) override {
        SDL_OpenURL(url.c_str());
    }
    // SDL decodes JPEG itself (its stb_image, SDL_ConvertPixels from MJPG):
    // no image library of the launcher's own.
    bool decode_image(const std::string &path, Image *out) override {
        size_t size = 0;
        void *data = SDL_LoadFile(path.c_str(), &size);
        if (!data)
            return false;
        int w = 0, h = 0;
        bool ok = jpeg_size(static_cast<const uint8_t *>(data), size, &w, &h) && w <= 8192 &&
                  h <= 8192 && size < (size_t(1) << 31);
        if (ok) {
            out->w = w;
            out->h = h;
            out->rgba.assign(size_t(w) * size_t(h) * 4, 0);
            ok = SDL_ConvertPixels(w, h, SDL_PIXELFORMAT_MJPG, data, int(size),
                                   SDL_PIXELFORMAT_RGBA32, out->rgba.data(), w * 4);
            if (!ok)
                fprintf(stderr, "[launcher] %s: %s\n", path.c_str(), SDL_GetError());
        }
        SDL_free(data);
        return ok;
    }

    // One frame of the game's own video through the FFmpeg the app already
    // carries for Bink: the first frame at or after `seconds`.
    bool decode_video_frame(const std::string &path, double seconds, Image *out) override {
#ifdef RECOMP_HAVE_FFMPEG
        AVFormatContext *fmt = nullptr;
        if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0)
            return false;
        bool ok = false;
        AVCodecContext *dec = nullptr;
        AVPacket *pkt = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        do {
            if (avformat_find_stream_info(fmt, nullptr) < 0)
                break;
            const int index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (index < 0)
                break;
            AVStream *st = fmt->streams[index];
            const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
            if (!codec || !(dec = avcodec_alloc_context3(codec)) ||
                avcodec_parameters_to_context(dec, st->codecpar) < 0 || avcodec_open2(dec, codec, nullptr) < 0)
                break;
            const int64_t want = int64_t(seconds / av_q2d(st->time_base));
            av_seek_frame(fmt, index, want, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(dec);
            while (!ok && av_read_frame(fmt, pkt) >= 0) {
                if (pkt->stream_index == index && avcodec_send_packet(dec, pkt) >= 0)
                    while (!ok && avcodec_receive_frame(dec, frame) >= 0) {
                        const int64_t pts = frame->best_effort_timestamp;
                        if (pts != AV_NOPTS_VALUE && pts < want)
                            continue;
                        if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVA420P)
                            break;
                        // BT.601 limited range to RGBA.
                        out->w = frame->width;
                        out->h = frame->height;
                        out->rgba.resize(size_t(out->w) * size_t(out->h) * 4);
                        for (int y = 0; y < out->h; ++y)
                            for (int x = 0; x < out->w; ++x) {
                                const float Y = 1.164f * (float(frame->data[0][y * frame->linesize[0] + x]) - 16);
                                const float U = float(frame->data[1][(y / 2) * frame->linesize[1] + x / 2]) - 128;
                                const float V = float(frame->data[2][(y / 2) * frame->linesize[2] + x / 2]) - 128;
                                uint8_t *q = &out->rgba[(size_t(y) * size_t(out->w) + size_t(x)) * 4];
                                auto c = [](float v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); };
                                q[0] = c(Y + 1.596f * V);
                                q[1] = c(Y - 0.392f * U - 0.813f * V);
                                q[2] = c(Y + 2.017f * U);
                                q[3] = 255;
                            }
                        ok = true;
                    }
                av_packet_unref(pkt);
            }
        } while (false);
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        if (!ok)
            fprintf(stderr, "[launcher] no frame at %.1f s in %s\n", seconds, path.c_str());
        return ok;
#else
        (void)path;
        (void)seconds;
        (void)out;
        return false;
#endif
    }

#ifdef __APPLE__
    bool check_update(const std::string &repo, const std::string &asset,
                      UpdateFound done) override {
        auto *heap = new UpdateFound(std::move(done));
        if (!os_update_fetch_release(
                repo.c_str(), asset.c_str(),
                [](void *ctx, int ok, const char *tag, const char *url, unsigned long long size,
                   const char *page) {
                    std::unique_ptr<UpdateFound> fn(static_cast<UpdateFound *>(ctx));
                    UpdateInfo info;
                    info.version = tag ? tag : "";
                    info.url = url ? url : "";
                    info.page = page ? page : "";
                    info.size = size;
                    (*fn)(ok != 0, info);
                },
                heap)) {
            delete heap;
            return false;
        }
        return true;
    }

    // curl into the caches, ditto next to this app (the same volume, so the swap is two renames), the new
    // app checked (its bundle identifier and its signature), this one to the Trash.
    bool install_update(const UpdateInfo &info, UpdateProgress progress, UpdateDone done) override {
        const std::string bundle = own_bundle();
        if (bundle.empty() || info.url.empty())
            return false;
        run_in_background([info, progress, done, bundle]() {
            auto fail = [&](const std::string &why) { done(false, why); };
            const std::string cache = home_dir() + "/Library/Caches/" RECOMP_APP_NAME;
            os_mkdir(cache.c_str());
            const std::string zip = cache + "/update.zip";
            os_unlink(zip.c_str());
            std::atomic<bool> finished{false};
            std::atomic<int> code{-1};
            std::thread fetch([&]() {
                code = run_tool({"/usr/bin/curl", "--proto", "=https", "--proto-redir", "=https",
                                 "-L", "-f", "-s", "-o", zip, info.url});
                finished = true;
            });
            while (!finished) {
                const int64_t have = file_size(zip);
                if (info.size && have > 0)
                    progress(std::min(0.95, double(have) / double(info.size)));
                os_sleep_us(200000);
            }
            fetch.join();
            if (code != 0)
                return fail("the download failed (curl " + std::to_string(code.load()) + ")");
            const std::string parent = bundle.substr(0, bundle.rfind('/'));
            std::string dir = parent + "/." RECOMP_APP_NAME "-update-XXXXXX";
            if (os_mkdtemp(dir.data()) != 0)
                return fail("cannot write next to the app in " + parent);
            auto clean = [&]() {
                remove_tree(dir);
                os_unlink(zip.c_str());
            };
            if (run_tool({"/usr/bin/ditto", "-x", "-k", zip, dir}) != 0) {
                clean();
                return fail("the archive would not unpack");
            }
            const std::string fresh = dir + "/" + bundle.substr(bundle.rfind('/') + 1);
            char id[256] = "";
            os_bundle_identifier(fresh.c_str(), id, sizeof id);
            if (std::string(id) != RECOMP_BUNDLE_ID) {
                clean();
                return fail("the archive holds no " RECOMP_APP_NAME ".app");
            }
            if (run_tool({"/usr/bin/codesign", "--verify", "--deep", "--strict", fresh}) != 0) {
                clean();
                return fail("the new app's signature does not verify");
            }
            progress(0.98);
            const std::string old = dir + "/previous.app";
            if (os_rename(bundle.c_str(), old.c_str()) != 0) {
                clean();
                return fail("cannot move " + bundle);
            }
            if (os_rename(fresh.c_str(), bundle.c_str()) != 0) {
                os_rename(old.c_str(), bundle.c_str());
                clean();
                return fail("cannot put the new app at " + bundle);
            }
            if (!os_trash_path(old.c_str()))
                remove_tree(old);
            clean();
            progress(1.0);
            done(true, "");
        });
        return true;
    }

    void relaunch() override {
        const std::string bundle = own_bundle();
        if (bundle.empty())
            return;
        // After this process is gone: `open` would bring this very instance forward instead.
        const char *argv[] = {"/bin/sh", "-c", "sleep 1; /usr/bin/open \"$0\"", bundle.c_str(),
                              nullptr};
        int64_t pid = 0;
        os_spawn(argv, &pid);
    }

    bool uninstall(std::string *error) override {
        const PlatformInfo i = info();
        const std::string home = home_dir();
        bool ok = true;
        for (const std::string &dir :
             {i.profile_dir, home + "/Library/Caches/" RECOMP_APP_NAME,
              home + "/Library/Saved Application State/" RECOMP_BUNDLE_ID ".savedState"}) {
            OsStat st{};
            if (!dir.empty() && os_stat(dir.c_str(), &st) == 0 && !remove_tree(dir)) {
                ok = false;
                if (error)
                    *error = "cannot remove " + dir;
            }
        }
        os_unlink((home + "/Library/Preferences/" RECOMP_BUNDLE_ID ".plist").c_str());
        const std::string bundle = own_bundle();
        if (ok && !bundle.empty() && !os_trash_path(bundle.c_str())) {
            ok = false;
            if (error)
                *error = "cannot move " + bundle + " to the Trash";
        }
        return ok;
    }
#endif

  private:
    SDL_Window *window_;
};

} // namespace

std::unique_ptr<Platform> make_platform(SDL_Window *window) {
    return std::make_unique<DesktopPlatform>(window);
}

} // namespace launcher
