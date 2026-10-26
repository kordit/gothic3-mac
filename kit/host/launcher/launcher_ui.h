// launcher_ui.h - the launcher screen's model: what it shows, what each
// button does, and the import running behind it. No SDL: the host feeds it
// pointer and key input and presents what it draws (launcher_sdl.cpp), and
// the tests drive it directly. The platform supplies pickers and storage.
#pragma once
#include "launcher.h"
#include "launcher_canvas.h"
#include "launcher_mods.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace launcher {

// Something the player chose in a picker: a filesystem path, or on Android a
// content URI (a tree for a folder, a document for a ZIP) opened by the
// platform.
struct Picked {
    std::string path;
    std::string uri;
    bool tree = false;
    std::string name; // for messages
};
using PickDone = std::function<void(std::vector<Picked> picked, std::string error)>;

struct PlatformInfo {
    bool plays_in_place = true; // desktop: an install is played where it is
    bool can_pick_folder = true;
    bool can_pick_zip = true;
    bool can_open_folder = false;
    bool touch = false;
    std::string import_root;  // where an import goes (<...>/game)
    std::string profile_dir;  // saves and settings
    std::string folders_file; // remembered installs (desktop)
    std::string cache_file;   // verified digests of installs played in place
    std::string drop_hint;    // how else files can arrive ("copy it into ... with Finder")
    std::string auto_import;  // imported without asking when the game is not found (a bundled copy)
    // The game's launcher art (backdrop.jpg, logo.jpg, frame.jpg, rosette.jpg;
    // the app's Resources/launcher); "" or missing files: drawn without it.
    std::string art_dir;
    // DepotDownloader beside the app's executable ("" when the app carries none), and the HOME it
    // runs with: its login token and the download's lists stay in the app's own data.
    std::string steam_tool;
    std::string steam_home;
};

// A newer release of this app ([launcher] releases_repo on GitHub).
struct UpdateInfo {
    std::string version; // without the "v"
    std::string url;     // the app's archive among the release's assets
    std::string page;    // the release page, for when it cannot be installed here
    uint64_t size = 0;
};
using UpdateFound = std::function<void(bool ok, UpdateInfo info)>;
using UpdateProgress = std::function<void(double fraction)>;
using UpdateDone = std::function<void(bool ok, std::string error)>;

class Platform {
  public:
    virtual ~Platform() = default;
    virtual PlatformInfo info() = 0;
    virtual void pick_folder(PickDone done) = 0;
    virtual void pick_zip(PickDone done) = 0;
    // Where exported saves go (a path to write), and a ZIP of saves to read.
    virtual void pick_export(const std::string &suggested_name, PickDone done) = 0;
    virtual void pick_saves(PickDone done) = 0;
    // A source over what was picked. The default opens a path as a folder or a ZIP.
    virtual std::unique_ptr<Source> open(const Picked &p, std::string *error);
    // Folders that may hold the game.
    virtual std::vector<std::string> candidates(const Spec &spec) = 0;
    // Something the app was opened with (Android "Open with" on a ZIP), once.
    virtual bool initial_pick(Picked *) {
        return false;
    }
    virtual void open_folder(const std::string &) {}
    virtual void open_url(const std::string &) {}
    // An import starts, advances or ends: keep the device awake, keep the
    // process alive (Android), show a notification.
    virtual void import_activity(bool /*active*/, const Progress *) {}
    // Whether an import from `p` may move its files and remove what is left:
    // a folder the player copied into app storage.
    virtual bool movable(const Picked &) {
        return false;
    }
    // A finished import: e.g. keep it out of device backups (iPadOS).
    virtual void protect_import(const std::string &) {}
    // The import from `p` ended, whatever the result (security-scoped access).
    virtual void release(const Picked &) {}
    // pick_export's file has been written: hand it to the player (a share
    // sheet, a document the player creates). Desktop wrote it in place.
    virtual void export_ready(const Picked &) {}
    // The import copy runs on this thread; tests run it inline.
    virtual void run_in_background(std::function<void()> work);
    // The latest release of `repo` that carries `asset`; false when this platform cannot ask (done is not called).
    virtual bool check_update(const std::string & /*repo*/, const std::string & /*asset*/,
                              UpdateFound /*done*/) {
        return false;
    }
    // Downloads the release's archive, puts the new app where this one is and keeps the player's data;
    // false when this platform cannot (the launcher opens the release page instead).
    virtual bool install_update(const UpdateInfo &, UpdateProgress, UpdateDone) {
        return false;
    }
    // Starts the app again (after an update) once this process has ended.
    virtual void relaunch() {}
    // Removes everything this app keeps (the game, the Steam login, saves, caches) and the app itself
    // where it can; false with *error when something stayed.
    virtual bool uninstall(std::string *error);
    // A JPEG (a mod's cover) decoded to RGBA8; false when this platform cannot.
    virtual bool decode_image(const std::string & /*path*/, Image * /*out*/) {
        return false;
    }
    // One frame of a video (the game's intro) at `seconds`, RGBA8; false when
    // this platform cannot.
    virtual bool decode_video_frame(const std::string & /*path*/, double /*seconds*/,
                                    Image * /*out*/) {
        return false;
    }
};

enum class Screen { Main, Manage, Importing, Message, Mods, Steam, Update };
enum class Key { Up, Down, Left, Right, Next, Previous, Activate, Back };

enum Action : int {
    kPlay = 1,
    kImportFolder,
    kImportZip,
    kLocate,
    kCopyIntoApp,
    kManage,
    kExportSaves,
    kImportSaves,
    kDeleteData,
    kOpenFolder,
    kStore,
    kBack,
    kCancel,
    kOk,
    kQuit,
    kUseCandidate = 100, // + index
    // The Steam screen (launcher_ui_steam.cpp).
    kSteam = 4000,  // download from the Steam account
    kSteamPassword, // log in with a name and a password instead of the QR code
    kSteamQr,       // back to the QR code
    kSteamLogin,    // the name and password are typed: log in
    kSteamUserField,
    kSteamPassField,
    kSteamCodeField,
    kSteamSend,          // send the Steam Guard code
    kSteamCancel,        // stop; the next download continues
    kSteamRetry,         // the same depot again, where it stopped
    kSteamOtherAccount,  // forget the stored login and log in again
    kSteamToggle = 4100, // + depot index: an optional part on or off
    kUpdate = 4300,      // install the newer release
    kUninstall,          // remove the app and everything it keeps (pressed twice)
    // The mod screen (Gothic 3, Sefaris catalog).
    kMods = 5000,
    kModsBack,
    kModsPlay,
    kModsPreset,
    kModsRoot,
    kModToggle,
    kModsSearch,            // the search field
    kModsPresetNone,        // "no preset"
    kModsGet,               // open [launcher] mods_url: where the mod catalog comes from
    kModsCategory = 6000,   // + 2 + index into categories; kModsCategory: all, +1: enabled
    kModsPresetPick = 7000, // + index into presets
    kModTile = 10000,       // + index into the catalog
};

// How a button is drawn and laid out.
enum ButtonKind : int {
    kKindNormal,  // a framed button
    kKindPrimary, // the big one (Play)
    kKindBar,     // a link in the bottom bar
    kKindIcon,    // a square icon button
    kKindList,    // a row in the mod screen's sidebar
    kKindTile,    // a mod's card
    kKindField,   // the search field
};

struct Button {
    int id;
    std::string label;
    bool enabled = true;
    Rect rect;
    int kind = kKindNormal;
    int icon = -1; // launcher_look.h Icon, -1 none
};

struct Look; // launcher_look.h

class Launcher {
  public:
    Launcher(const Spec &spec, Platform &platform);
    ~Launcher();

    // `known` is a folder already resolved (saved path, checkout, data root);
    // it may be "".
    void start(const std::string &known);

    void pointer(int x, int y, bool pressed); // canvas pixels
    // A file or folder dropped on the window.
    void drop(const std::string &path);
    // Mobile: a ready game starts after `seconds` unless the player touches
    // the screen. advance() moves the countdown.
    void set_auto_play(double seconds);
    // The game cannot run here (no GPU): Play explains instead of starting.
    void set_unplayable(const std::string &reason);
    void advance(double seconds);
    bool counting_down() const {
        return countdown_ > 0;
    }
    void key(Key k);
    // A wheel turn or a trackpad swipe over (x, y), in steps (fractions from
    // a trackpad): scrolls the mod grid or the mod's description smoothly.
    void wheel(int x, int y, double steps);
    // The pointer moved (hover highlights).
    void hover(int x, int y);
    // Typing into the mod screen's search field while it has the focus.
    bool wants_text() const {
        return (screen_ == Screen::Mods && search_active_) ||
               (screen_ == Screen::Steam && steam_field_ != 0);
    }
    void type_text(const std::string &utf8);
    void erase_text();
    // Something is still moving (a hover fading, a scroll gliding): draw again soon.
    bool animating() const;
    void set_version(const std::string &v) {
        version_ = v;
    }
    // Scripted screenshots: select the first mod whose title contains `text`.
    bool select_mod(const std::string &text);
    // The part of a mod's card that switches it on or off.
    Rect tile_toggle(const Rect &tile) const;
    void activate(int id);
    void tick(); // picker results and import progress, on the host's thread
    // `density`: canvas pixels per point (2 on a Retina display). The layout
    // is in points, everything is drawn at the canvas's own resolution.
    void draw(Canvas &canvas, float density);
    // The last frame, after Play: the backdrop and "Starting the game…" alone. It stays on screen until the
    // game shows its own first picture, so the hand-over is one change, not the launcher frozen mid-click.
    void draw_starting(Canvas &canvas, float density);

    bool finished() const {
        return finished_;
    }
    bool quit() const {
        return quit_;
    }
    std::string exe() const {
        return status_.exe;
    }
    const Status &status() const {
        return status_;
    }
    Screen screen() const {
        return screen_;
    }
    const std::string &message() const {
        return message_;
    }
    const std::vector<std::string> &found() const {
        return found_;
    }
    const std::vector<Button> &buttons();
    bool dirty() const {
        return dirty_;
    }
    int focus() const {
        return focus_;
    }
    // The mod screen's state, for the tests.
    const ModCatalog &mod_catalog() const {
        return mods_;
    }
    const ModSelection &mod_selection() const {
        return sel_;
    }
    const std::string &mod_notice() const {
        return mod_notice_;
    }
    int mod_selected() const {
        return mod_selected_;
    }

  private:
    void rebuild();
    void evaluate(const std::string &root);
    void pick_then(bool folder, bool play_in_place);
    void begin_import(const Picked &p);
    void show(const std::string &text);
    std::string L(const char *en, const char *pl) const {
        return spec_.language == "pl" ? pl : en;
    }
    int P(float points) const; // points to canvas pixels
    void ensure_look();
    void draw_main(Canvas &c);
    void draw_button(Canvas &c, const Button &b, int index);
    float hot(int index) const; // 0..1: hover/press highlight, animated
    bool focus_visible(int index) const;
    void move_focus(int dx, int dy);
    bool clickable(const Button &b, int x, int y) const;
    std::string mods_summary();
    // The mod screen.
    void open_mods();
    void load_mods(const std::string &root);
    bool write_mods_for_play(); // false when the player has to confirm first
    void build_view();
    std::vector<int> visible_mods() const;
    void draw_mods(Canvas &c);
    void mods_buttons();
    std::string choice_file() const;
    // The Steam screen (launcher_ui_steam.cpp).
    bool steam_offered() const;
    void begin_steam();
    // The .ini edits and the modules (spec_.stage) at the import root, then Play.
    void prepare_then_play(const std::vector<std::string> &edits);
    void mark_started(); // the first Play in this profile: first_start_
    bool first_start_ = false;
    std::vector<std::string> steam_edits(); // [launcher.steam] ini, and each part's on or off edits
    // Whether `root` is a game this app can start: with modules to make, only its own import.
    bool playable_here(const std::string &root) const;
    void launch_depot();
    void steam_tick();
    void steam_buttons();
    void activate_steam(int id);
    void steam_text(std::string *headline, std::string *sub, std::string *detail,
                    Color *accent) const;
    float steam_extras_height(float s) const;
    void draw_steam_extras(Canvas &c, float x0, float col_w, float s, float *y);
    void draw_steam_qr(Canvas &c, float W, float H, float bar_h, float col_w, float Lm);
    bool steam_type(const std::string &utf8);
    bool steam_erase();
    std::string *steam_field_text();
    bool depot_wanted(size_t i) const;
    double steam_overall() const;

    const Spec &spec_;
    Platform &platform_;
    PlatformInfo info_;
    Status status_;
    Screen screen_ = Screen::Main;
    std::string message_;
    std::vector<std::string> found_;
    std::vector<Button> buttons_;
    int focus_ = 0;
    int columns_ = 1; // Up and Down move a whole row
    int pressed_ = -1;
    bool finished_ = false, quit_ = false, dirty_ = true, confirm_delete_ = false;
    double countdown_ = 0;
    std::string unplayable_;
    Picked importing_;

    // The mod screen.
    ModCatalog mods_;
    ModSelection sel_;
    bool mods_loaded_ = false, have_choice_ = false, confirm_play_ = false;
    bool default_choice_ = false; // sel_ is [launcher] default_mods, not the player's own
    std::string mod_root_;
    int mod_category_ = 0; // 0 all, 1 enabled, 2+ categories()[i - 2]
    int mod_selected_ = -1;
    int pointer_x_ = 0, pointer_y_ = 0;
    bool pointer_activation_ = false;
    std::vector<ReadmeLine> readme_;
    std::string mod_notice_;
    Rect grid_rect_, detail_rect_; // the scrolled views, canvas pixels
    int cover_w_ = 0, cover_h_ = 0;
    std::map<int, Image> covers_; // by catalog index, at cover_w_ x cover_h_
    std::vector<int> cover_order_; // least recently drawn first
    std::vector<int> no_cover_;
    Image detail_cover_;
    int detail_cover_for_ = -1, detail_cover_w_ = 0;
    std::string search_;
    bool search_active_ = false;
    bool reveal_selected_ = false; // scroll the selected card into view at the next draw
    // Scrolling in points: where it is and where it glides to.
    float grid_scroll_ = 0, grid_target_ = 0, grid_content_ = 0;
    float detail_scroll_ = 0, detail_target_ = 0, detail_content_ = 0;
    void select_tile(int idx);
    void reveal_focus();
    const Image *cover_for(int idx, int w, int h, int *budget, bool *pending);

    // The look and the animation.
    std::unique_ptr<Look> look_;
    float d_ = 1; // canvas pixels per point
    std::string version_;
    int hover_ = -1;        // the button under the pointer
    bool keyboard_ = false; // the last input was a key: show the focus ring
    std::map<int, float> anim_; // by button id: the highlight shown, 0..1

    // Shared with picker callbacks and the import thread.
    struct Shared {
        std::mutex m;
        std::vector<std::function<void()>> posted;
        Progress progress;
        bool importing = false, import_done = false;
        ImportOutcome outcome;
    };
    std::shared_ptr<Shared> shared_;
    std::shared_ptr<std::atomic<bool>> cancel_;
    void post(std::function<void()> fn);

    // The Steam download.
    std::unique_ptr<SteamSession> steam_;
    SteamState steam_state_; // the last one drawn
    size_t steam_depot_ = 0; // index into spec_.steam.depots
    bool steam_password_mode_ = false, steam_cancelled_ = false, steam_staging_ = false;
    bool steam_password_sent_ = false;
    int steam_field_ = 0; // the field typing goes to (kSteam*Field), 0 none
    std::string steam_user_, steam_pass_, steam_code_, steam_account_;
    std::vector<std::string> steam_skipped_; // labels of parts the player left out

    // Updates (launcher_ui_steam.cpp).
    UpdateInfo update_;
    bool update_known_ = false, confirm_uninstall_ = false;
    std::string update_error_;
    void check_for_update();
    void begin_update();
};

std::string human_bytes(uint64_t bytes);

} // namespace launcher
