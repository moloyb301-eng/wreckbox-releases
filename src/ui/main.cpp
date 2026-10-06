// WreckBox for Windows: entry point, window and message loop.
//
//   wreckbox.exe [--root <library folder>] [--background] [files, folders or URLs to play…]
//     --root        use another library folder (testing; settings go to <root>\_settings)
//     --background  open behind other windows without taking focus (automated UI checks)
//     anything else is played, like a drop on the window (Explorer's "Open with")
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <memory>

#include "../res/resource.h"
#include "library/downloads_watcher.h"
#include "library/store.h"
#include "model/paths.h"
#include "model/settings.h"
#include "net/account.h"
#include "net/sync_server.h"
#include "net/tunnel.h"
#include "net/updates.h"
#include "player/player.h"
#include "ui/artwork.h"
#include "ui/gfx.h"
#include "ui/jobs.h"
#include "ui/media_controls.h"
#include "ui/ui.h"
#include "ui/view.h"

namespace {

using namespace wb;
using namespace wb::ui;

void app_log(const std::string& line) {
    std::ofstream out(paths::app_log(), std::ios::app);
    out << iso_seconds_now() << " " << line << "\n";
}

double ms_since_process_start() {
    FILETIME created, x1, x2, x3;
    GetProcessTimes(GetCurrentProcess(), &created, &x1, &x2, &x3);
    FILETIME now;
    GetSystemTimePreciseAsFileTime(&now);
    const auto t = [](FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return double(t(now) - t(created)) / 10000.0;
}

struct App {
    HWND hwnd = nullptr;
    LibraryStore store;
    Jobs jobs;
    sync::Server phone_server{store};  // phone sync + "use from anywhere": idle (a thread asleep) until started
    sync::Tunnel tunnel{phone_server};
    std::unique_ptr<DownloadsWatcher> watcher;  // files new downloads into the library; after the store it works on
    std::unique_ptr<player::Player> player;  // before the view, which holds a reference to it
    Gfx gfx;
    std::unique_ptr<ArtworkCache> art;
    std::unique_ptr<Ui> ui;
    std::unique_ptr<View> view;
    MediaControls media;
    std::string cover_for;  // the track whose cover file was looked for
    std::optional<std::filesystem::path> cover;
    std::atomic<bool> change_posted{false};
    int last_hit = -2;
    bool first_paint = true, tracking_leave = false;

    float dips(LPARAM lp) const { return float(GET_X_LPARAM(lp)) * 96.f / gfx.dpi(); }
    float dips_y(LPARAM lp) const { return float(GET_Y_LPARAM(lp)) * 96.f / gfx.dpi(); }
    void repaint() { InvalidateRect(hwnd, nullptr, FALSE); }

    void create(HWND h) {
        hwnd = h;
        gfx.init(h);
        art = std::make_unique<ArtworkCache>(store, jobs, gfx);
        ui = std::make_unique<Ui>(gfx, *art);
        // libVLC's events arrive on its threads; the player hands them to the UI thread through WM_APP_DONE.
        player = std::make_unique<player::Player>(store, [this](std::function<void()> fn) {
            auto* p = new std::function<void()>(std::move(fn));
            if (!PostMessageW(hwnd, WM_APP_DONE, 0, reinterpret_cast<LPARAM>(p))) delete p;
        });
        player->on_changed = [this] { repaint(); };
        view = std::make_unique<View>(h, store, jobs, *ui, *player);
        view->attach_phone(phone_server, tunnel);
        tunnel.on_changed = [this] { InvalidateRect(hwnd, nullptr, FALSE); };  // from the tunnel's thread
        DragAcceptFiles(h, TRUE);
        if (!media.init(h)) app_log("Windows media controls unavailable; media keys work only while WreckBox is focused");
        // The store fires on any thread; coalesce into one repaint message.
        store.on_changed = [this] {
            if (!change_posted.exchange(true)) PostMessageW(hwnd, WM_APP_CHANGED, 0, 0);
        };
        jobs.start(h, std::max(2u, std::thread::hardware_concurrency() - 1));
        // Load in the background so the window appears at once.
        auto started = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
        jobs.run([this] { store.load(); },
                 [this, started] {
                     view->set_loading(false);
                     view->store_changed();
                     const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - *started).count();
                     const auto lib = store.library();
                     app_log(std::format("library loaded in {:.0f} ms ({} tracks)", ms, lib ? lib->tracks.size() : 0));
                     if (Settings::current().share_remotely && account::signed_in()) tunnel.start();  // as last time
                     if (paths::downloads() && !watcher) {  // not with a --root test library
                         watcher = std::make_unique<DownloadsWatcher>(store, *paths::downloads());
                         watcher->start();
                     }
                     check_for_update();
                 });
    }

    // Once per start, off the UI thread, and not for a --root test library or with WRECKBOX_NO_UPDATE_CHECK=1.
    void check_for_update() {
        wchar_t v[4]{};
        if (paths::overridden() || GetEnvironmentVariableW(L"WRECKBOX_NO_UPDATE_CHECK", v, 4)) return;
        auto result = std::make_shared<updates::Result>();
        jobs.run([result] { *result = updates::check(); }, [this, result] {
            if (result->newer) {
                app_log("update available: " + result->newer->version);
                view->set_update(result->newer);
            } else if (!result->error.empty()) {
                app_log("update check: " + result->error);
            }
        });
    }

    // WRECKBOX_PERF=1: log paint times (average / worst per 100 paints) to wreckbox.log — for checking weak PCs.
    const bool perf = [] {
        wchar_t v[4]{};
        return GetEnvironmentVariableW(L"WRECKBOX_PERF", v, 4) && v[0] == L'1';
    }();
    double perf_build = 0, perf_total = 0, perf_max = 0;
    int perf_n = 0;
    ULONGLONG perf_since = GetTickCount64();
    int why_done = 0, why_changed = 0, why_timer = 0, why_input = 0, why_size = 0;

    void paint() {
        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();
        auto t1 = t0;
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        if (gfx.begin()) {
            view->paint();
            t1 = clock::now();
            gfx.end();
        }
        EndPaint(hwnd, &ps);
        sync_media();
        if (perf) {
            const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
            const double total = ms(t0, clock::now());
            perf_build += ms(t0, t1), perf_total += total, perf_max = std::max(perf_max, total);
            if (++perf_n == 100) {
                app_log(std::format("perf: 100 paints in {:.1f} s — build avg {:.2f} ms, with present avg {:.2f} ms, worst {:.2f} ms "
                                    "(requests: jobs {}, store {}, timer {}, input {}, size {})",
                                    double(GetTickCount64() - perf_since) / 1000, perf_build / 100, perf_total / 100, perf_max, why_done,
                                    why_changed, why_timer, why_input, why_size));
                why_done = why_changed = why_timer = why_input = why_size = 0;
                perf_build = perf_total = perf_max = 0, perf_n = 0;
                perf_since = GetTickCount64();
            }
        }
        if (first_paint) {
            first_paint = false;
            app_log(std::format("startup: first paint {:.0f} ms after process start", ms_since_process_start()));
        }
    }

    // What Windows' media flyout shows. Cheap when nothing changed; the cover is picked up once its file exists.
    void sync_media() {
        MediaControls::State s;
        const player::Item* cur = player->current();
        s.active = cur != nullptr;
        s.playing = player->playing();
        s.can_previous = s.active;
        s.can_next = player->queue().has_next();
        if (cur) {
            const auto d = player->display();
            s.title = widen(d.title);
            s.artist = widen(d.subtitle);
            if (cur->track_id) {
                if (cover_for != *cur->track_id || !cover) {
                    cover_for = *cur->track_id;
                    cover.reset();
                    std::error_code ec;
                    if (const auto t = store.track(cover_for); t && std::filesystem::exists(store.artwork_file(*t), ec)) cover = store.artwork_file(*t);
                }
                s.cover = cover;
            }
        }
        media.update(s);
    }

    void save_placement() {
        WINDOWPLACEMENT wp{sizeof wp};
        if (!GetWindowPlacement(hwnd, &wp)) return;
        const RECT& r = wp.rcNormalPosition;
        auto& s = Settings::current();
        s.extra["windowPlacement"] = {{"x", r.left}, {"y", r.top}, {"w", r.right - r.left}, {"h", r.bottom - r.top},
                                      {"maximized", wp.showCmd == SW_SHOWMAXIMIZED}};
        try {
            s.save();
        } catch (const std::exception& e) {
            app_log(std::string("couldn't save settings: ") + e.what());
        }
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_PAINT: paint(); return 0;
            case WM_SYSCOMMAND:
                if (perf) app_log(std::format("WM_SYSCOMMAND {:#x}", wp & 0xFFF0));
                break;
            case WM_ERASEBKGND: return 1;  // Direct2D paints everything
            case WM_SIZE: gfx.resize(LOWORD(lp), HIWORD(lp)); ++why_size; repaint(); return 0;
            case WM_DPICHANGED: {
                gfx.set_dpi(float(HIWORD(wp)));
                view->dpi_changed();
                const RECT* r = reinterpret_cast<RECT*>(lp);
                SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
                repaint();
                return 0;
            }
            case WM_GETMINMAXINFO: {
                const float k = float(GetDpiForWindow(hwnd)) / 96.f;
                reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {LONG(1000 * k), LONG(640 * k)};
                return 0;
            }
            case WM_MOUSEMOVE: {
                if (!tracking_leave) {
                    TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd, 0};
                    tracking_leave = TrackMouseEvent(&t);
                }
                ui->mx = dips(lp), ui->my = dips_y(lp);
                view->mouse_moved();
                if (ui->mouse_drag(ui->mx, ui->my)) return repaint(), 0;
                // Repaint only when the thing under the pointer changes (hover highlight).
                const int hit = ui->hit_index(ui->mx, ui->my);
                if (hit != last_hit) last_hit = hit, ++why_input, repaint();
                return 0;
            }
            case WM_MOUSELEAVE:
                tracking_leave = false;
                ui->mx = ui->my = -1;
                last_hit = -2;
                repaint();
                return 0;
            case WM_LBUTTONDBLCLK:
                if (view->double_click()) return 0;
                [[fallthrough]];  // elsewhere a double-click is two clicks
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
                SetCapture(hwnd);
                if (view->mouse_down(dips(lp), dips_y(lp), msg == WM_RBUTTONDOWN)) repaint();
                return 0;
            case WM_LBUTTONUP:
            case WM_RBUTTONUP:
                ReleaseCapture();
                ui->mouse_up();  // a slider's release (seek, volume) acts here
                repaint();
                return 0;
            case WM_MOUSEWHEEL: {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ScreenToClient(hwnd, &pt);
                const float k = 96.f / gfx.dpi();
                if (ui->wheel(float(pt.x) * k, float(pt.y) * k, float(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * 64)) repaint();
                return 0;
            }
            case WM_KEYDOWN:
                if (view->key(wp)) repaint();
                return 0;
            case WM_COMMAND:
                if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == kSearchBox) view->search_changed();
                if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) repaint();  // focus ring on the search pill
                return 0;
            case WM_APP_TAB:
                view->tab(int(wp), lp != 0);
                return 0;
            case WM_CTLCOLOREDIT: return reinterpret_cast<LRESULT>(view->edit_colors(reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp)));
            case WM_TIMER:
                ++why_timer;
                if (wp != kPlayerTimer) KillTimer(hwnd, wp);  // one-shots; the player's tick repeats
                repaint();
                return 0;
            case WM_APP_CHANGED:
                ++why_changed;
                change_posted = false;
                view->store_changed();
                if (account::signed_in()) account::schedule_upload(store);  // the account's copy follows once changes settle
                repaint();
                return 0;
            case WM_APP_DONE:
                ++why_done;
                Jobs::dispatch(lp);
                repaint();
                return 0;
            case WM_DROPFILES:
                view->drop(reinterpret_cast<HDROP>(wp));
                repaint();
                return 0;
            case WM_APP_MEDIA:
                view->app_command(int(wp));
                repaint();
                return 0;
            case WM_APPCOMMAND:
                if (view->app_command(GET_APPCOMMAND_LPARAM(lp))) return repaint(), TRUE;
                break;
            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT && view->hide_cursor()) return SetCursor(nullptr), TRUE;
                break;
            case WM_CLOSE:
                view->set_fullscreen(false);  // so the normal window position is what's saved
                save_placement();
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                player->stop();
                tunnel.stop();
                watcher.reset();
                account::shutdown();
                jobs.stop();
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

App* g_app = nullptr;

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        g_app->hwnd = hwnd;
    } else if (msg == WM_CREATE) {
        g_app->create(hwnd);
        return 0;
    }
    if (g_app && g_app->view) return g_app->handle(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    std::optional<std::filesystem::path> root;
    bool background = false;
    std::vector<std::string> play;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring a = argv[i];
            if (a == L"--root" && i + 1 < argc) root = argv[++i];
            else if (a == L"--background") background = true;
            else if (!a.starts_with(L"--")) play.push_back(narrow(a));
        }
        LocalFree(argv);
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    paths::init(root);
    Settings::load();

    App app;
    g_app = &app;
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_DBLCLKS;  // double-click leaves the full-screen visualizer
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"WreckBox";
    RegisterClassExW(&wc);

    // Restore the last window position, else 1440×900 at 100 % scaling, centred by Windows.
    const int sys_dpi = int(GetDpiForSystem());
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = MulDiv(1440, sys_dpi, 96), h = MulDiv(900, sys_dpi, 96);
    bool maximized = false;
    if (const auto& p = Settings::current().extra.value("windowPlacement", wb::json::object()); p.is_object() && p.contains("w")) {
        x = p.value("x", x), y = p.value("y", y), w = p.value("w", w), h = p.value("h", h);
        maximized = p.value("maximized", false);
    }
    HWND hwnd = CreateWindowExW(0, L"WreckBox", L"WreckBox", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    if (background) {
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        ShowWindow(hwnd, maximized ? SW_SHOWMAXIMIZED : show);
    }

    if (!play.empty()) app.view->play_paths(std::move(play));

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
