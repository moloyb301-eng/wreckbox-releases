// Settings and first-run setup (port of app/lib/ui/settings_page.dart): CSV import, Spotify and YouTube direct import,
// bug-report details, about. Soulseek login arrives with phase 8, update checks with phase 7.
#include <shobjidl.h>

#include <format>

#include "engine/engine.h"
#include "model/paths.h"
#include "model/settings.h"
#include "net/account.h"
#include "net/oauth.h"
#include "net/tunnel.h"
#include "sources/csv_import.h"
#include "sources/spotify.h"
#include "sources/youtube.h"
#include "ui/view.h"

namespace wb::ui {

using namespace theme;

namespace {

enum Field : int { kSpotifyId = 101, kGoogleId, kGoogleSecret, kReporterName, kReporterContact };

std::string trim(std::string s) {
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    return s;
}

// The Windows file picker, several .csv files at once. Empty if cancelled.
std::vector<std::string> pick_csv_files(HWND owner) {
    std::vector<std::string> out;
    Microsoft::WRL::ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    const COMDLG_FILTERSPEC filter[] = {{L"CSV files", L"*.csv"}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, filter);
    dlg->SetTitle(L"Choose the playlist CSV files");
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    if (FAILED(dlg->Show(owner))) return out;
    Microsoft::WRL::ComPtr<IShellItemArray> items;
    DWORD n = 0;
    if (FAILED(dlg->GetResults(&items)) || FAILED(items->GetCount(&n))) return out;
    for (DWORD i = 0; i < n; ++i) {
        Microsoft::WRL::ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            out.push_back(narrow(path));
            CoTaskMemFree(path);
        }
    }
    return out;
}

void copy_to_clipboard(HWND owner, const std::string& value) {
    const std::wstring w = widen(value);
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t))) {
        memcpy(GlobalLock(mem), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
}

}  // namespace

bool View::save_settings() {
    auto& s = Settings::current();
    auto take = [this](int id, std::string& into) {
        if (edits_.contains(id)) into = trim(edit_text(id));
    };
    take(kSpotifyId, s.spotify_client_id);
    take(kGoogleId, s.google_client_id);
    take(kGoogleSecret, s.google_client_secret);
    take(kReporterName, s.reporter_name);
    take(kReporterContact, s.reporter_contact);
    try {
        s.save();
        return true;
    } catch (const std::exception& e) {
        set_status(std::string("Couldn't save settings: ") + e.what());
        return false;
    }
}

void View::settings_page(const Rect& r) {
    const auto& s = Settings::current();
    const float width = std::min(r.w(), 780.f);
    const std::string status = this->status();
    ui_.push_clip(Rect{r.l - 22, r.t, r.r + 22, r.b});
    float y = r.t - settings_scroll_;
    y += header(Rect{r.l, y, r.l + width, r.b}, L"Settings", L"Setup", L"");

    // Every section is laid out twice: once to measure its height (draw = false), then for real on a panel that size.
    bool draw = false;
    const float pad = 18, x = r.l + pad, w = width - 2 * pad;

    auto para = [&](const std::wstring& body, float size = 13, D2D1_COLOR_F c = text2, int weight = 400) {
        const TextStyle st{Font::ui, size, weight, c};
        const float h = g_.measure_height(body, w, st);
        if (draw) g_.paragraph(body, Rect{x, y, x + w, y + h}, st);
        y += h + 8;
    };
    auto step = [&](int n, const std::wstring& body, const std::string& copy = "") {
        const float tw = w - 26 - (copy.empty() ? 0 : 56);
        const TextStyle st{Font::ui, 13, 400, text2};
        const float h = std::max(18.f, g_.measure_height(body, tw, st));
        if (draw) {
            g_.text(n > 0 ? std::to_wstring(n) : L"·", Rect{x, y, x + 20, y + 18}, {Font::dot, 15, 700, lilac});
            g_.paragraph(body, Rect{x + 26, y, x + 26 + tw, y + h}, st);
            if (!copy.empty()) {
                const Rect btn{x + w - 52, y - 2, x + w, y + 20};
                g_.text(L"Copy", btn, {Font::ui, 12, 600, ui_.hover(btn) ? text : lilac, Align::center});
                ui_.click(btn, [this, copy] {
                    copy_to_clipboard(hwnd_, copy);
                    set_status("Copied " + copy);
                });
            }
        }
        y += h + 8;
    };
    struct Button {
        std::wstring label;
        const wchar_t* icon;
        Ui::Pill style;
        std::function<void()> on_click;
    };
    auto buttons = [&](const std::vector<Button>& bs) {
        float bx = x;
        for (const auto& b : bs) {
            const float bw = ui_.pill_width(b.label, b.icon);
            if (bx + bw > x + w && bx > x) bx = x, y += 42;
            if (draw) ui_.pill(bx, y, b.label, b.icon, b.style, b.on_click);
            bx += bw + 8;
        }
        y += 34 + 12;
    };
    auto field = [&](int id, const wchar_t* label, const std::string& initial, bool password = false, const wchar_t* cue = L"") {
        if (draw) {
            g_.text(label, Rect{x, y, x + w, y + 16}, {Font::ui, 12, 400, text2});
            const Rect box{x, y + 20, x + w, y + 60};
            constexpr COLORREF on_panel = RGB(30, 30, 32);
            const HWND h = edits_.contains(id) ? edits_[id].hwnd : edit(id, Rect{}, password, cue, initial, on_panel);
            g_.fill_round(box, 14, glass_fill);
            g_.stroke_round(box, 14, GetFocus() == h ? with_alpha(lilac, 0.6f) : hairline);
            ui_.click(box, [h] { SetFocus(h); });
            // A native box can't be clipped, so it only shows when its field is fully in view.
            if (box.t >= r.t && box.b <= r.b) edit(id, Rect{box.l + 14, box.t + 11, box.r - 14, box.b - 11}, password, cue, initial, on_panel);
        }
        y += 70;
    };
    auto status_line = [&] {
        if (!status.empty()) para(widen(status), 12.5f, text2);
    };
    auto section = [&](const wchar_t* title, bool smart, const std::function<void()>& content) {
        const float top = y;
        draw = false;
        y = top + pad + 24;
        content();
        const float h = y - top + pad - 8;
        draw = true;
        ui_.glass(Rect{r.l, top, r.l + width, top + h}, 20, smart);
        ui_.dot_label(title, x, top + pad + 7, text);
        y = top + pad + 24;
        content();
        y = top + h + 16;
    };
    const bool busy = importing_;
    auto run_import = [this](std::string first_line, std::function<void(const csv::Log&)> work) {
        importing_ = true;
        set_status(first_line);
        jobs_.run(
            [this, work] {
                try {
                    work([this](const std::string& line) { set_status(line); });
                    store_.load();
                    Settings::current().onboarded = true;
                    Settings::current().save();
                } catch (const std::exception& e) {
                    set_status(std::string("Import failed: ") + e.what());
                }
            },
            [this] { importing_ = false; });
    };

    section(L"Import playlists", !lib_, [&] {
        para(L"Bring in your Spotify and YouTube playlists as CSV files — no accounts or developer keys needed. Re-import any time; a "
             L"playlist with the same name is replaced.");
        step(1, L"Spotify: open exportify.net, log in with Spotify, click \"Export All\" (or export single playlists). You get one CSV per "
                L"playlist.");
        step(2, L"YouTube / YouTube Music: takeout.google.com → deselect all → tick \"YouTube and YouTube Music\" → choose only "
                L"\"playlists\" → export. Or use tunemymusic.com → your service → \"Export to file\" (CSV).");
        step(3, L"Click Choose CSV files and select them all at once.");
        buttons({{L"Open Exportify", icon::open, Ui::Pill::glass, [] { oauth::open_in_browser("https://exportify.net"); }},
                 {L"Open Google Takeout", icon::open, Ui::Pill::glass, [] { oauth::open_in_browser("https://takeout.google.com"); }},
                 {L"Open TuneMyMusic", icon::open, Ui::Pill::glass, [] { oauth::open_in_browser("https://www.tunemymusic.com"); }}});
        buttons({{busy ? L"Importing…" : L"Choose CSV files", icon::upload, Ui::Pill::primary,
                  busy ? std::function<void()>{} : [this, run_import] {
                      const auto files = pick_csv_files(hwnd_);
                      if (files.empty()) return;
                      run_import("Reading the CSV files…", [files](const csv::Log& log) { csv::run(files, log); });
                  }}});
        status_line();
        para(L"Each song is looked up in free music catalogues (MusicBrainz, Deezer) for its ISRC and cover. The first import of a big "
             L"library takes a while — about one song per second.",
             11.5f, text3);
    });

    section(L"Spotify — direct (optional, needs Spotify Premium)", false, [&] {
        para(L"Advanced: import straight from your Spotify account instead of CSV. Spotify requires the key's owner to have Premium; one "
             L"key can be shared with up to 5 people (add their Spotify emails under the app's \"User Management\").",
             12.5f, text3);
        para(L"Spotify only lets each developer app have a few users, so everyone uses their own free key. It takes about two minutes:");
        step(1, L"Open the Spotify developer dashboard and log in with your Spotify account.");
        step(2, L"Create an app (any name, e.g. \"My WreckBox\"). Tick \"Web API\".");
        step(3, L"Add both of these as Redirect URIs, then save:");
        step(0, widen(spotify::kRedirect), spotify::kRedirect);
        step(0, L"wreckbox://spotify-callback", "wreckbox://spotify-callback");
        step(4, L"Copy the app's Client ID and paste it here.");
        buttons({{L"Open Spotify dashboard", icon::open, Ui::Pill::glass, [] { oauth::open_in_browser("https://developer.spotify.com/dashboard"); }}});
        field(kSpotifyId, L"Spotify Client ID", s.spotify_client_id, false, L"32 characters");
        buttons({{busy ? L"Importing…" : L"Import my playlists", icon::download, Ui::Pill::primary,
                  busy ? std::function<void()>{} : [this, run_import] {
                      if (!save_settings()) return;
                      run_import("Opening Spotify sign-in in your browser…", [](const csv::Log& log) { spotify::run(log); });
                  }}});
        status_line();
    });

    section(L"YouTube — direct (optional)", false, [&] {
        para(L"Bring in your YouTube and YouTube Music playlists and liked music. Songs that are also on Spotify merge into one entry. "
             L"This imports the playlists only — not audio.");
        step(1, L"In Google Cloud Console create a project, then enable \"YouTube Data API v3\".");
        step(2, L"OAuth consent screen: External, add yourself as a user, then set Publishing status to \"In production\" (otherwise "
                L"Google signs you out every 7 days; the \"unverified app\" warning is expected — it's your own app).");
        step(3, L"Credentials → Create OAuth client ID → type \"Desktop app\". Copy the Client ID and Client secret here.");
        buttons({{L"Open Google Cloud Console", icon::open, Ui::Pill::glass,
                  [] { oauth::open_in_browser("https://console.cloud.google.com/apis/library/youtube.googleapis.com"); }}});
        field(kGoogleId, L"Google Client ID", s.google_client_id, false, L"….apps.googleusercontent.com");
        field(kGoogleSecret, L"Google Client secret", s.google_client_secret, true);
        buttons({{busy ? L"Importing…" : L"Import my YouTube playlists", icon::video, Ui::Pill::primary,
                  busy ? std::function<void()>{} : [this, run_import] {
                      if (!save_settings()) return;
                      run_import("Opening Google sign-in in your browser…", [](const csv::Log& log) { youtube::run(log); });
                  }}});
    });

    // Where WreckBox looks for your songs, and the Downloads organiser.
    const auto folders = store_.scan_folders();
    section(L"Library folders", false, [&] {
        para(L"WreckBox looks for your songs in these folders when it scans (Rescan & analyse). Songs that match your playlists are added to "
             L"your crate.");
        for (const auto& f : folders) {
            const bool own = f == narrow(paths::tracks().wstring());  // the library's Tracks folder stays
            if (draw) {
                g_.text(widen(f), Rect{x, y, x + w - (own ? 0 : 70), y + 22}, {Font::ui, 12.5f, 400, text2});
                if (!own) {
                    const Rect btn{x + w - 62, y, x + w, y + 22};
                    g_.text(L"Remove", btn, {Font::ui, 12, 600, ui_.hover(btn) ? text : lilac, Align::right});
                    ui_.click(btn, [this, f] { store_.remove_scan_folder(f); });
                }
            }
            y += 26;
        }
        y += 6;
        buttons({{L"Add folder…", icon::add, Ui::Pill::glass, [this] {
                      for (const auto& f : pick_folders()) store_.add_scan_folder(f);
                  }},
                 {L"Reset to defaults", icon::undo, Ui::Pill::glass, [this] { store_.reset_scan_folders(); }},
                 {L"Scan now", icon::scan, Ui::Pill::glass, [this] { jobs_.run([this] { store_.rescan(); }); }}});
        const bool organise = s.organise_downloads;
        const std::wstring downloads = paths::downloads() ? paths::downloads()->wstring() : L"(not used with a test library)";
        para(std::wstring(L"Organise new downloads automatically: ") + (organise ? L"on" : L"off") +
                 L". New audio files in your Downloads folder (" + downloads +
                 L") that belong to your library are tagged, renamed and moved into Tracks. Others are left alone.",
             12.5f);
        buttons({{organise ? L"Turn off" : L"Turn on", organise ? icon::close : icon::check, organise ? Ui::Pill::glass : Ui::Pill::primary, [this, organise] {
                      Settings::current().organise_downloads = !organise;
                      save_settings();
                  }}});
    });

    section(L"Use from anywhere", false, [&] {
        para(L"Lets your phone (signed in to the same WreckBox account) stream and download from this computer away from home, while WreckBox "
             L"is open. Pairing and the account are on the Sync to phone page.",
             12.5f);
        if (account::signed_in()) {
            if (tunnel_) para(widen(tunnel_->status()), 12.5f, tunnel_ && tunnel_->running() ? lilac : text2, 600);
            const bool want = s.share_remotely;
            buttons({{want ? L"Turn off" : L"Turn on", want ? icon::close : icon::check, want ? Ui::Pill::glass : Ui::Pill::primary,
                      [this, want] { set_share_remotely(!want); }}});
        } else {
            buttons({{L"Sign in on Sync to phone", icon::phone, Ui::Pill::glass, [this] { go(Page::phone); }}});
        }
    });

    section(L"Bug reports", false, [&] {
        para(L"Shown on reports you send, so the developer can follow up. Send one with Report a bug at the bottom of the sidebar.");
        field(kReporterName, L"Your name", s.reporter_name);
        field(kReporterContact, L"Email or contact (optional)", s.reporter_contact);
    });

    section(L"About", false, [&] {
        para(std::format(L"WreckBox {} (native Windows build) · engine {}", widen(WB_VERSION), widen(engine_version())));
        para(L"Library folder: " + paths::root().wstring(), 12, text3);
        para(L"Settings file: " + paths::settings_file().wstring(), 12, text3);
        para(L"Plays music with VLC's engine (libVLC " + widen(player::VlcEngine::version()) +
                 L", LGPL 2.1+; some plugins GPL 2+). The licences and a link to VLC's source code are in the licenses "
                 L"folder next to WreckBox.",
             12, text3);
        const std::string update_note = update_msg();
        buttons({{update_busy_ ? L"Checking…" : L"Check for updates", icon::sync, Ui::Pill::glass,
                  update_busy_ ? std::function<void()>{} : [this] { check_updates(); }}});
        if (!update_note.empty()) para(widen(update_note), 12.5f, text2, 600);
        buttons({{L"Open licences", icon::folder, Ui::Pill::glass, [this] {
                      wchar_t exe[MAX_PATH]{};
                      GetModuleFileNameW(nullptr, exe, MAX_PATH);
                      const auto dir = std::filesystem::path(exe).parent_path() / L"licenses";
                      ShellExecuteW(hwnd_, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                  }}});
    });

    if (draw) {
        const float bw = ui_.pill(r.l, y, L"Save settings", icon::check, Ui::Pill::primary, busy ? std::function<void()>{} : [this] {
            if (save_settings()) set_status("Settings saved.");
        });
        // The latest status (saved, copied, import progress) next to the button too: the sections above may be scrolled away.
        if (!status.empty()) g_.text(widen(status), Rect{r.l + bw + 14, y, r.l + width, y + 34}, {Font::ui, 12.5f, 400, text2});
        y += 34 + 30;
    }
    ui_.pop_clip();
    const float before = settings_scroll_;
    ui_.scroll_area(r, settings_scroll_, y + settings_scroll_ - r.t);
    if (settings_scroll_ != before) InvalidateRect(hwnd_, nullptr, FALSE);
}

}  // namespace wb::ui
