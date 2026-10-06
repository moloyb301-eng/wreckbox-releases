// Phase 7 extras on screen: the "WreckBox x.y.z is available" banner, update checks from Settings, "Report a bug" (a dialog
// with the automatic screenshot, extra images and what gets sent), and the "Use from anywhere" switch shared with the phone
// page. The network parts are net/updates and net/bug_report.
#include <shobjidl.h>

#include <fstream>

#include "model/settings.h"
#include "net/bug_report.h"
#include "net/oauth.h"
#include "net/sync_server.h"
#include "net/tunnel.h"
#include "net/updates.h"
#include "ui/screenshot.h"
#include "ui/view.h"

namespace wb::ui {

using namespace theme;

namespace {

std::string trim(std::string s) {
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    return s;
}

// The Windows file picker: images (several), or a folder.
std::vector<std::string> pick(HWND owner, bool folder) {
    std::vector<std::string> out;
    Microsoft::WRL::ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    if (folder) {
        dlg->SetTitle(L"Choose a folder with music");
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    } else {
        const COMDLG_FILTERSPEC filter[] = {{L"Images", L"*.png;*.jpg;*.jpeg"}};
        dlg->SetFileTypes(1, filter);
        dlg->SetTitle(L"Add screenshots");
        dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    }
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

}  // namespace

std::vector<std::string> View::pick_folders() { return pick(hwnd_, true); }

// MARK: Updates

void View::set_update(std::optional<updates::Info> info) {
    update_ = std::move(info);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool View::update_visible() const {
    return update_ && Settings::current().extra.value("updateDismissed", "") != update_->version;
}

void View::update_banner(const Rect& r) {
    ui_.glass(r, 18, true);
    g_.fill_circle(r.l + 24, (r.t + r.b) / 2, 4, lilac);
    g_.text(L"WreckBox " + widen(update_->version) + L" is available", Rect{r.l + 40, r.t, r.r - 230, r.b}, {Font::ui, 13.5f, 600, text});
    const float lw = ui_.pill_width(L"Later"), dw = ui_.pill_width(L"Download");
    ui_.pill(r.r - 12 - lw, r.t + 6, L"Later", nullptr, Ui::Pill::glass, [this] {
        auto& s = Settings::current();
        s.extra["updateDismissed"] = update_->version;  // until the next release
        try {
            s.save();
        } catch (const std::exception&) {
        }
    });
    ui_.pill(r.r - 12 - lw - 8 - dw, r.t + 6, L"Download", nullptr, Ui::Pill::primary, [this] { oauth::open_in_browser(update_->url); });
}

void View::check_updates() {
    if (update_busy_) return;
    update_busy_ = true;
    set_update_msg("Checking…");
    auto result = std::make_shared<updates::Result>();
    jobs_.run([result] { *result = updates::check(); },
              [this, result] {
                  update_busy_ = false;
                  if (!result->error.empty()) set_update_msg("Couldn't check: " + result->error);
                  else if (result->newer) {
                      Settings::current().extra.erase("updateDismissed");
                      set_update(result->newer);
                      set_update_msg("WreckBox " + result->newer->version + " is available.");
                  } else {
                      set_update(std::nullopt);
                      set_update_msg(std::string("You're up to date (") + WB_VERSION + ").");
                  }
              });
}

void View::set_update_msg(std::string s) {
    {
        std::lock_guard lock(status_m_);
        update_msg_ = std::move(s);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::string View::update_msg() const {
    std::lock_guard lock(status_m_);
    return update_msg_;
}

// MARK: Use from anywhere

void View::set_share_remotely(bool on) {
    auto& s = Settings::current();
    s.share_remotely = on;
    try {
        s.save();
    } catch (const std::exception& e) {
        set_status(std::string("Couldn't save the setting: ") + e.what());
    }
    if (!tunnel_) return;
    if (on) tunnel_->start();
    else tunnel_->stop();
}

// MARK: Report a bug

void View::open_bug_report() {
    bug_shot_ = capture_window_png(hwnd_, g_.wic());  // before the dialog covers what the user was seeing
    bug_include_shot_ = !bug_shot_.empty();
    bug_extra_.clear();
    set_bug_msg("");
    bug_open_ = true;
    bug_focused_ = false;
    eq_open_ = url_open_ = false;
}

void View::set_bug_msg(std::string s) {
    {
        std::lock_guard lock(status_m_);
        bug_msg_ = std::move(s);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void View::close_bug_report() {
    bug_open_ = false;
    for (const int id : {kBugTitle, kBugBody})
        if (const auto it = edits_.find(id); it != edits_.end()) SetWindowTextW(it->second.hwnd, L"");
    bug_shot_.clear();
    bug_extra_.clear();
    SetFocus(hwnd_);
}

void View::bug_add_images() {
    for (const auto& path : pick(hwnd_, false)) {
        if (bug_extra_.size() >= 2) break;
        std::ifstream f(widen(path), std::ios::binary);
        std::string bytes{std::istreambuf_iterator<char>(f), {}};
        const auto name = std::filesystem::path(widen(path)).filename().string();
        if (bytes.size() > bugs::kMaxScreenshotBytes) {
            set_bug_msg(name + " is over 3 MB; pick a smaller image.");
            continue;
        }
        std::wstring ext = std::filesystem::path(widen(path)).extension().wstring();
        const bool jpeg = ext.size() > 1 && (ext[1] == L'j' || ext[1] == L'J');
        bug_extra_.push_back({std::move(bytes), jpeg ? "image/jpeg" : "image/png", name});
    }
}

void View::bug_send() {
    const std::string body = trim(edit_text(kBugBody)), title = trim(edit_text(kBugTitle));
    if (body.empty()) return set_bug_msg("Please describe what happened.");
    bugs::Report report;
    report.title = title.empty() ? body.substr(0, body.find('\n')) : title;
    report.description = body;
    if (bug_include_shot_ && !bug_shot_.empty() && bug_shot_.size() <= bugs::kMaxScreenshotBytes) report.screenshots.push_back({bug_shot_, "image/png", "screenshot.png"});
    for (const auto& e : bug_extra_) report.screenshots.push_back(e);
    bug_sending_ = true;
    set_bug_msg("Sending…");
    jobs_.run(
        [this, report = std::move(report)] {
            try {
                set_bug_msg("Sent — thank you! (report #" + std::to_string(bugs::send(report, store_)) + ")");
            } catch (const std::exception& e) {
                set_bug_msg(std::string("Couldn't send: ") + e.what());
            }
        },
        [this] { bug_sending_ = false; });
}

void View::bug_dialog() {
    const float W = g_.width(), H = g_.height();
    g_.fill(Rect{0, 0, W, H}, argb(0x99000000));
    ui_.click(Rect{0, 0, W, H}, [this] { close_bug_report(); });
    const float dh = 452;
    const Rect p = Rect::xywh((W - 540) / 2, std::max(10.f, (H - dh) / 2), 540, dh);
    ui_.glass(p, 22, false, true);
    ui_.click(p, [] {});
    const Rect in = p.inset(24, 22);
    std::string message;
    {
        std::lock_guard lock(status_m_);
        message = bug_msg_;
    }
    const bool sent = message.rfind("Sent", 0) == 0;

    ui_.dot_label(L"Report a bug", in.l, in.t + 8, text);
    g_.text(L"What went wrong? The more detail, the faster it gets fixed.", Rect{in.l, in.t + 22, in.r, in.t + 42}, {Font::ui, 12.5f, 400, text2});
    constexpr COLORREF fill = RGB(30, 30, 32);
    auto box = [&](const Rect& field, int id, const wchar_t* cue, bool multiline) {
        const HWND h = edit(id, field.inset(14, 11), false, cue, "", fill, multiline);
        g_.fill_round(field, 14, argb(0xFF1E1E20));
        g_.stroke_round(field, 14, GetFocus() == h ? with_alpha(lilac, 0.6f) : hairline);
        ui_.click(field, [h] { SetFocus(h); });
        return h;
    };
    box(Rect{in.l, in.t + 54, in.r, in.t + 94}, kBugTitle, L"Short title (optional)", false);
    const HWND body = box(Rect{in.l, in.t + 102, in.r, in.t + 222}, kBugBody, L"What did you do, what happened, what did you expect?", true);
    if (!bug_focused_) bug_focused_ = true, SetFocus(body);

    float y = in.t + 234;
    if (!bug_shot_.empty())
        ui_.chip(in.l, y, L"Include a screenshot of the app", std::nullopt, bug_include_shot_, false, [this] { bug_include_shot_ = !bug_include_shot_; });
    y += 38;
    float x = in.l;
    for (size_t i = 0; i < bug_extra_.size(); ++i) {
        const std::wstring label = widen(bug_extra_[i].name) + L"  ×";
        x += ui_.chip(x, y, label, std::nullopt, false, false, [this, i] { bug_extra_.erase(bug_extra_.begin() + std::ptrdiff_t(i)); }) + 8;
    }
    if (bug_extra_.size() < 2) ui_.pill(x, y - 3, L"Add screenshots", icon::add, Ui::Pill::glass, [this] { bug_add_images(); });
    y += 42;
    const std::wstring note = L"Also sent: app version, Windows version, your name from Settings" + std::wstring(Settings::current().reporter_name.empty() ? L" (not set)" : L"") +
                              L", and recent WreckBox activity. No music files or passwords.";
    g_.paragraph(note, Rect{in.l, y, in.r, y + 34}, {Font::ui, 11.5f, 400, text3});
    if (!message.empty()) g_.text(widen(message), Rect{in.l, in.b - 56, in.r, in.b - 38}, {Font::ui, 13, 600, sent ? lilac : message.rfind("Sending", 0) == 0 ? text2 : peach});

    const std::wstring send_label = bug_sending_ ? L"Sending…" : L"Send report";
    const float bw = ui_.pill_width(send_label, icon::arrow_right), cw = ui_.pill_width(sent ? L"Close" : L"Cancel");
    ui_.pill(in.r - bw, in.b - 34, send_label, icon::arrow_right, Ui::Pill::primary, bug_sending_ || sent ? std::function<void()>{} : [this] { bug_send(); });
    ui_.pill(in.r - bw - 10 - cw, in.b - 34, sent ? L"Close" : L"Cancel", nullptr, Ui::Pill::glass, [this] { close_bug_report(); });
}

}  // namespace wb::ui
