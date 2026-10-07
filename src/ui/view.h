// The desktop screens (port of app/lib/ui/desktop.dart and tracks.dart): sidebar | page | inspector.
#pragma once
#include <windows.h>
#include <shellapi.h>

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <chrono>
#include <filesystem>
#include <future>
#include <memory>

#include "library/store.h"
#include "net/bug_report.h"
#include "net/updates.h"
#include "player/milkdrop.h"
#include "player/player.h"
#include "player/visualizer.h"
#include "ui/jobs.h"
#include "ui/ui.h"

namespace wb::sync {
class Server;
class Tunnel;
}  // namespace wb::sync
namespace wb::soulseek {
class Sync;
}

namespace wb::ui {

inline constexpr UINT WM_APP_TAB = WM_APP + 3;  // wParam: text box id, lParam: 1 = backwards (Shift+Tab)
inline constexpr int kSearchBox = 1;           // control id of the search box; settings fields use 100+
inline constexpr int kUrlBox = 201;            // Open URL…
inline constexpr int kBugTitle = 401, kBugBody = 402;  // Report a bug
inline constexpr int kSlskQuery = 403;                  // Soulseek: custom search words
inline constexpr UINT_PTR kRefreshTimer = 1, kPlayerTimer = 2, kTooltipTimer = 3;

class View {
public:
    View(HWND hwnd, LibraryStore& store, Jobs& jobs, Ui& ui, player::Player& player);
    ~View();

    // Player input from the window procedure (view_player.cpp / view_visualizer.cpp).
    void drop(HDROP files);           // WM_DROPFILES
    bool app_command(int cmd);        // WM_APPCOMMAND: media keys
    bool double_click();              // full screen visualizer: double-click leaves
    void mouse_moved();               // wakes the full-screen overlay
    bool hide_cursor() const;         // full screen with the overlay faded out
    void set_fullscreen(bool on);
    bool fullscreen() const { return fullscreen_; }
    // The full-screen visualizer is moving: the window repaints right after each frame, paced by the compositor.
    bool vsync_loop() const { return vsync_loop_; }
    void set_update(std::optional<updates::Info> info);  // the startup check found a newer release (or nothing)
    void attach_soulseek(soulseek::Sync& sl);
    void attach_phone(sync::Server& server, sync::Tunnel& tunnel);  // the phone-sync services (owned by the app)
    void play_paths(std::vector<std::string> paths);  // files, folders, playlists or URLs (Open, drop, command line)

    void paint();
    void store_changed() { dirty_ = true; }  // data changed: recompute lists on the next paint
    void set_loading(bool loading) { loading_ = loading; }

    // Input from the window procedure. Coordinates in DIPs. Each returns true if a repaint is needed.
    bool mouse_down(float x, float y, bool right);
    bool key(WPARAM vk);
    void search_changed();       // EN_CHANGE from the search box
    HBRUSH edit_colors(HDC dc, HWND box);  // WM_CTLCOLOREDIT
    void dpi_changed();          // new font for the text boxes
    void tab(int from_id, bool back);  // Tab / Shift+Tab inside a text box

private:
    enum class Page { home, all, downloaded, missing, ignored, on_pc, playlist, queue, soulseek, phone, settings };
    enum class Sort { none, title, artist, bpm, key, energy, type };
    enum class Format { any, flac, not_flac };  // not_flac: has a file, of another type
    struct Mix {
        std::wstring min_bpm, max_bpm;
        std::optional<std::string> key;
        bool compatible = true;
        Format format = Format::any;
        bool active() const { return !min_bpm.empty() || !max_bpm.empty() || key || format != Format::any; }
    };

    // A header action (button or status text): its width, and how to draw it at (x, y).
    struct Action {
        float w;
        std::function<void(float x, float y)> draw;
    };

    // Sections
    void sidebar(const Rect& r);
    float header(const Rect& r, const std::wstring& eyebrow, const std::wstring& title, const std::wstring& subtitle,
                 const std::vector<Action>& actions = {});  // returns the height used
    std::vector<Action> library_actions(bool write_tags);
    void page(const Rect& r);
    void welcome(const Rect& r);
    void home(const Rect& r);
    void placeholder(const Rect& r, const std::wstring& title, const std::wstring& subtitle, const std::wstring& phase);
    void track_page(const Rect& r);
    float filters(const Rect& r);  // returns the height used (the row wraps when the page is narrow)
    void list(const Rect& r);
    void row(const TrackRow& row, const Rect& r);
    void inspector(const Rect& r, bool floating);
    void settings_page(const Rect& r);  // view_settings.cpp
    void phone_page(const Rect& r);     // view_phone.cpp
    void phone_message(std::string s);
    void account_job(std::string working, std::function<std::string()> work);

    // Behaviour
    void go(Page p, std::optional<std::string> playlist = std::nullopt);
    void refresh();  // recompute everything derived from the store
    ListFilter list_filter() const;
    void focus(const std::optional<std::string>& id);
    void row_menu(const std::string& id);
    void key_menu();
    void show_in_folder(const std::string& path);

    // Native text boxes laid over drawn fields (IME, clipboard, undo and screen readers for free). Each frame places
    // the ones it draws; the rest are hidden after the paint.
    struct Edit {
        HWND hwnd = nullptr;
        RECT px{};
        bool used = false;
        COLORREF bg = RGB(20, 20, 22);  // the colour of the drawn field under it, so the box blends in
    };
    // `bg`: the drawn field's colour — on the page background (20,20,22) or on a glass panel (30,30,32).
    HWND edit(int id, const Rect& r, bool password = false, const wchar_t* cue = L"", const std::string& initial = "",
              COLORREF bg = RGB(20, 20, 22), bool multiline = false);
    std::string edit_text(int id) const;
    void hide_unused_edits();
    // Settings fields → Settings::current(), then save. Returns false (with status_ set) if saving failed.
    bool save_settings();
    void set_status(const std::string& s);
    std::string status() const;

    HWND hwnd_;
    LibraryStore& store_;
    Jobs& jobs_;
    Ui& ui_;
    Gfx& g_;
    HWND search_ = nullptr;
    std::map<int, Edit> edits_;
    HFONT edit_font_ = nullptr;
    std::map<COLORREF, HBRUSH> edit_brushes_;
    HANDLE font_handle_ = nullptr;

    // Extras (view_extras.cpp): update banner, Report a bug, Use from anywhere
    void update_banner(const Rect& r);
    bool update_visible() const;
    void check_updates();
    void set_update_msg(std::string s);
    std::string update_msg() const;
    void set_share_remotely(bool on);
    std::vector<std::string> pick_folders();
    void open_bug_report();
    void close_bug_report();
    void bug_dialog();
    void bug_add_images();
    void bug_send();
    void set_bug_msg(std::string s);
    std::optional<updates::Info> update_;
    size_t n_on_pc_ = 0;            // On this PC: audio files in the library folders
    std::optional<size_t> n_flac_;  // and how many are FLAC (counted when that page shows)
    bool update_busy_ = false;
    std::string update_msg_, bug_msg_;  // guarded by status_m_
    bool bug_open_ = false, bug_sending_ = false, bug_include_shot_ = true, bug_focused_ = false;
    std::string bug_shot_;  // PNG of the app, taken when the dialog opened
    std::vector<bugs::Screenshot> bug_extra_;

    // Soulseek sync and the download queue (view_soulseek.cpp)
    void soulseek_page(const Rect& r);
    void queue_page(const Rect& r);
    void slsk_query_dialog();
    void slsk_query_submit();
    void close_slsk_query();
    void slsk_message(std::string s);
    soulseek::Sync* slsk_ = nullptr;
    std::string slsk_tab_ = "not_found", slsk_msg_;  // slsk_msg_ guarded by status_m_
    float slsk_scroll_ = 0, queue_scroll_ = 0;
    bool slsk_query_open_ = false, slsk_query_focused_ = false;
    std::string slsk_query_for_, slsk_query_default_;

    // Sync to phone (view_phone.cpp)
    sync::Server* sync_ = nullptr;
    sync::Tunnel* tunnel_ = nullptr;
    float phone_scroll_ = 0;
    bool account_busy_ = false, account_creating_ = false;
    std::string phone_msg_;  // the last result line of the account / pairing buttons (guarded by status_m_)

    // Settings page
    float settings_scroll_ = 0;
    bool importing_ = false;
    mutable std::mutex status_m_;
    std::string status_;  // last progress / result line of an import, shared by the import buttons (as in Flutter)

    Page page_ = Page::home;
    std::optional<std::string> playlist_;
    std::wstring search_text_;
    Mix mix_;
    Sort sort_ = Sort::none;
    bool asc_ = true;
    float list_scroll_ = 0, home_scroll_ = 0, side_scroll_ = 0, insp_scroll_ = 0;
    float list_view_h_ = 0;  // height of the visible list area (for keyboard scrolling)

    // Derived from the store, rebuilt by refresh().
    bool dirty_ = true, loading_ = true;
    ULONGLONG refreshed_at_ = 0;
    std::shared_ptr<const Library> lib_;
    std::vector<std::string> ids_;  // the list on screen, filtered and sorted
    size_t list_total_ = 0, list_crate_ = 0;
    size_t n_downloaded_ = 0, n_missing_ = 0, n_ignored_ = 0, n_analysed_ = 0, n_priority_ = 0;
    std::vector<std::string> recent_;
    std::optional<std::string> mixes_for_;
    std::vector<TrackRow> mixes_;

    // Playlist Sync: why the open playlist can't sync (cached per playlist + library version + client ids, since
    // finding out reads the source files), and the last sync's result line for it.
    std::string sync_key_;
    std::optional<std::string> sync_unavailable_;
    std::string sync_message_;
    void sync_playlist();

    // Player (view_player.cpp): the bar under the page, the equalizer pop-over, Open, drag & drop.
    void player_bar(const Rect& r);
    void transport(const Rect& r);  // previous / play / next, shared with the full-screen panel
    void seek_bar(const Rect& r);
    void volume(const Rect& r);
    void eq_panel();
    void preset_menu();
    void url_dialog();
    void open_menu();
    void open_url();
    void play_track(const std::string& id);  // the current track toggles
    bool player_key(WPARAM vk);
    bool vsync_loop_ = false;
    void update_timer();  // repaint ticks: 4 a second while playing, ~60 in the full-screen visualizer
    player::Player& player_;
    bool eq_open_ = false, url_open_ = false;
    float eq_x_ = 0, bar_top_ = 0;     // where the equalizer pop-over hangs from
    std::optional<float> seek_drag_;   // 0–1 while the seek slider is held
    UINT timer_ms_ = 0;

    // Full-screen visualizer (view_visualizer.cpp).
    void visualizer_screen();
    void draw_spectrum(const Rect& r);
    void draw_scope(const Rect& r, bool over_bars);
    void vis_overlay(const Rect& r, float alpha);
    void vis_menu();
    void save_vis();
    bool vis_key(WPARAM vk);
    float overlay_alpha() const;
    void vis_click();
    void vis_use_milk(bool milk);  // M, and the MilkDrop / Bars chips
    void milk_poll();
    void milk_start();                        // creates the engine (once per visit to full screen)
    void milk_screen(float W, float H);       // renders one MilkDrop frame (or keeps the last one) and draws it
    std::filesystem::path user_presets_dir() const;
    bool fullscreen_ = false;
    player::Visualizer vis_;
    std::vector<float> tap_ = std::vector<float>(player::Visualizer::kFft);
    std::chrono::steady_clock::time_point vis_at_{}, moved_at_{};
    float frame_ms_ = 0;  // average full-screen frame time; above 12 ms the visualizer drops to 30 fps
    bool vis_settling_ = false;  // bars still falling after a pause
    // MilkDrop: made when full screen opens in that mode and freed when it closes, so the GPU and RAM aren't held.
    std::unique_ptr<player::MilkDrop> milk_;
    std::future<std::unique_ptr<player::MilkDrop>> milk_pending_;  // being built on a worker thread (scanning ~10k presets)
    std::chrono::steady_clock::time_point milk_asked_{};
    bool milk_tried_ = false;     // started (or failed to) on this visit; a failure is final until the next one
    std::string milk_why_;        // why it failed, for the log
    int milk_w_ = 0, milk_h_ = 0, milk_quality_ = 720;  // render size, and the quality after any automatic drop
    int slow_frames_ = 0;
    // The audio heard when a frame appears (AudioOutput::heard_index): MilkDrop is fed what was heard since the last
    // frame, the bars and the beat pulse look at the same moment.
    int64_t milk_heard_ = -1;  // tap index fed up to; -1 = start afresh
    std::vector<float> pcm_ = std::vector<float>(4096);
    player::BeatPulse pulse_;
    bool bt_output_ = false;   // default speaker is Bluetooth: +180 ms, which Windows doesn't report
    std::chrono::steady_clock::time_point frame_at_{};
    float frame_dt_ = 1 / 60.f;  // seconds between full-screen frames (averaged)
    int feed_frames_ = 0, feed_empty_ = 0;  // WRECKBOX_PERF: what MilkDrop was fed, logged every 300 frames
    int64_t feed_samples_ = 0;
    std::chrono::steady_clock::time_point feed_since_{};
    std::wstring toast_;         // a short note at the top ("Sync +50 ms"), until toast_until_
    std::chrono::steady_clock::time_point toast_until_{};
    int64_t heard_end(const player::AudioOutput& out, float frames_ahead) const;
    void show_toast(std::wstring note);
    void draw_toast();
    void milk_restart();         // rebuild the engine (a different set of presets)
    std::chrono::steady_clock::time_point audio_at_{};  // last time sound was playing: pictures settle for 4 s after
    player::VisOptions::Mode winamp_mode_ = player::VisOptions::Mode::spectrum;  // what M switches back to
    player::VisOptions::Mode mode_before_click_{};
    LONG saved_style_ = 0;
    WINDOWPLACEMENT saved_place_{sizeof(WINDOWPLACEMENT)};
};

}  // namespace wb::ui
