// Sync to phone: start / stop sharing, the pairing QR code and
// link, unpairing, the WreckBox account (sign in / create / sync / sign out) and "Use from anywhere" (the tunnel).
// The servers themselves are net/sync_server, net/account and net/tunnel; this file is the screen.
#include <qrcodegen.hpp>

#include <format>

#include "model/settings.h"
#include "net/account.h"
#include "net/sync_server.h"
#include "net/tunnel.h"
#include "ui/view.h"

namespace wb::ui {

using namespace theme;

namespace {

enum Field : int { kAccountName = 301, kAccountEmail, kAccountPassword };

std::string trim(std::string s) {
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    return s;
}

void copy_text(HWND owner, const std::string& value) {
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

void View::attach_phone(sync::Server& server, sync::Tunnel& tunnel) {
    sync_ = &server;
    tunnel_ = &tunnel;
}

void View::phone_message(std::string s) {
    {
        std::lock_guard lock(status_m_);
        phone_msg_ = std::move(s);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// Runs an account call on a worker: the key derivation and the network must not block the window.
void View::account_job(std::string working, std::function<std::string()> work) {
    account_busy_ = true;
    phone_message(std::move(working));
    jobs_.run(
        [this, work = std::move(work)] {
            std::string result;
            try {
                result = work();
            } catch (const std::exception& e) {
                result = e.what();
            }
            phone_message(std::move(result));
        },
        [this] { account_busy_ = false; });
}

void View::phone_page(const Rect& r) {
    if (!sync_ || !tunnel_) return;
    const auto& s = Settings::current();
    const float width = std::min(r.w(), 780.f);
    std::string message;
    {
        std::lock_guard lock(status_m_);
        message = phone_msg_;
    }
    const bool on = sync_->running(), signed_in = account::signed_in();
    ui_.push_clip(Rect{r.l - 22, r.t, r.r + 22, r.b});
    float y = r.t - phone_scroll_;

    const std::wstring toggle = on ? L"Stop sharing" : L"Start sharing";
    y += header(Rect{r.l, y, r.l + width, r.b}, L"Tools", L"Sync to phone", L"Send tracks from this computer to the WreckBox phone app over your Wi-Fi",
                {{ui_.pill_width(toggle, on ? icon::close : icon::phone), [this, toggle, on](float x, float py) {
                      ui_.pill(x, py, toggle, on ? icon::close : icon::phone, on ? Ui::Pill::glass : Ui::Pill::smart, [this, on] {
                          if (on) sync_->stop();
                          else if (!sync_->start()) phone_message("Couldn't start sharing: port " + std::to_string(sync::Server::configured_port()) + " is in use.");
                          InvalidateRect(hwnd_, nullptr, FALSE);
                      });
                  }}});

    // Every section is laid out twice: once to measure its height (draw = false), then for real on a panel that size.
    bool draw = false;
    const float pad = 18;
    float x = r.l + pad, w = width - 2 * pad;

    auto para = [&](const std::wstring& body, float size = 13, D2D1_COLOR_F c = text2, int weight = 400) {
        const TextStyle st{Font::ui, size, weight, c};
        const float h = g_.measure_height(body, w, st);
        if (draw) g_.paragraph(body, Rect{x, y, x + w, y + h}, st);
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
    auto field = [&](int id, const wchar_t* label, const wchar_t* cue, bool password = false) {
        if (draw) {
            g_.text(label, Rect{x, y, x + w, y + 16}, {Font::ui, 12, 400, text2});
            const Rect box{x, y + 20, x + w, y + 60};
            constexpr COLORREF on_panel = RGB(30, 30, 32);
            const HWND h = edits_.contains(id) ? edits_[id].hwnd : edit(id, Rect{}, password, cue, id == kAccountEmail ? s.account_email.value_or("") : "", on_panel);
            g_.fill_round(box, 14, glass_fill);
            g_.stroke_round(box, 14, GetFocus() == h ? with_alpha(lilac, 0.6f) : hairline);
            ui_.click(box, [h] { SetFocus(h); });
            // A native box can't be clipped, so it only shows when its field is fully in view.
            if (box.t >= r.t && box.b <= r.b) edit(id, Rect{box.l + 14, box.t + 11, box.r - 14, box.b - 11}, password, cue, "", on_panel);
        }
        y += 70;
    };
    auto section = [&](const wchar_t* title, bool smart, const std::function<void()>& content) {
        const float top = y;
        draw = false;
        y = top + pad + 24;
        content();
        const float h = y - top + pad - 8;
        draw = true;
        ui_.glass(Rect{r.l, top, r.l + width, top + h}, 20, smart);
        ui_.dot_label(title, r.l + pad, top + pad + 7, text);
        y = top + pad + 24;
        content();
        y = top + h + 16;
    };

    // The pairing code.
    std::string uri;
    if (on) uri = sync_->pairing_uri();
    section(L"Pair a phone", false, [&] {
        constexpr float qr = 220, gap = 24;
        const float top = y, left_x = x, left_w = w;
        if (draw) {
            const Rect box = Rect::xywh(x, y, qr, qr);
            if (on) {
                const qrcodegen::QrCode code = qrcodegen::QrCode::encodeText(uri.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
                g_.cached(L"qr:" + widen(uri), box, [&](Gfx& g) {
                    g.fill(box, white);
                    const float cell = (qr - 20) / float(code.getSize());
                    for (int cy = 0; cy < code.getSize(); ++cy)
                        for (int cx = 0; cx < code.getSize(); ++cx)
                            if (code.getModule(cx, cy)) g.fill(Rect::xywh(box.l + 10 + float(cx) * cell, box.t + 10 + float(cy) * cell, cell + 0.5f, cell + 0.5f), black);
                });
            } else {
                g_.paragraph(L"Start sharing to show the pairing code", box.inset(20, 90, 20, 0), {Font::ui, 13, 400, text3, Align::center});
            }
        }
        x = left_x + qr + gap, w = left_w - qr - gap;
        y = top;
        para(L"On your phone", 18, text, 600);
        const wchar_t* steps[] = {L"Connect the phone to the same Wi-Fi as this computer.", L"Open WreckBox → Computer → Scan pairing code.",
                                  L"Pick playlists and tap Download. Tracks arrive tagged and analysed."};
        for (int i = 0; i < 3; ++i) {
            const TextStyle st{Font::ui, 13, 400, text2};
            const float h = std::max(18.f, g_.measure_height(steps[i], w - 26, st));
            if (draw) {
                g_.text(std::to_wstring(i + 1), Rect{x, y, x + 20, y + 18}, {Font::dot, 15, 700, lilac});
                g_.paragraph(steps[i], Rect{x + 26, y, x + w, y + h}, st);
            }
            y += h + 8;
        }
        if (on) {
            para(L"Camera won't read it? On the phone tap \"Enter pairing link\" and paste:", 12);
            const TextStyle st{Font::ui, 11.5f, 400, text3};
            const float tw = w - 56, h = g_.measure_height(widen(uri), tw, st);
            if (draw) {
                g_.paragraph(widen(uri), Rect{x, y, x + tw, y + h}, st);
                const Rect btn{x + w - 52, y - 2, x + w, y + 20};
                g_.text(L"Copy", btn, {Font::ui, 12, 600, ui_.hover(btn) ? text : lilac, Align::center});
                ui_.click(btn, [this, uri] {
                    copy_text(hwnd_, uri);
                    phone_message("Copied the pairing link.");
                });
            }
            y += h + 10;
        }
        para(L"Only phones that scanned this code can connect. Windows may ask to allow WreckBox on private networks — allow it.", 12, text3);
        buttons({{L"Unpair all phones", icon::close, Ui::Pill::glass, [this] {
                      try {
                          sync_->reset_token();
                          phone_message("Every phone is unpaired. Scan the new code to pair again.");
                      } catch (const std::exception& e) {
                          phone_message(std::string("Couldn't save the new code: ") + e.what());
                      }
                  }}});
        y = std::max(y, top + qr + 8);
        x = left_x, w = left_w;
    });

    // The account.
    const bool busy = account_busy_;
    section(L"WreckBox account", !signed_in, [&] {
        if (signed_in) {
            para(L"Signed in as " + widen(s.account_email.value_or("")), 14, text, 600);
            para(L"This library is saved to your account. Turn on \"Use from anywhere\" so your phone can reach this computer away from home.", 12.5f);
            buttons({{busy ? L"Syncing…" : L"Sync library now", icon::sync, Ui::Pill::glass,
                      busy ? std::function<void()>{} : [this] {
                          account_job("Uploading your library…", [this] {
                              account::upload_library(store_);
                              return std::string("Up to date.");
                          });
                      }},
                     {L"Sign out", icon::close, Ui::Pill::glass, busy ? std::function<void()>{} : [this] {
                          tunnel_->stop();
                          account_job("Signing out…", [] {
                              account::sign_out();
                              return std::string("Signed out.");
                          });
                      }}});
        } else {
            para(account_creating_ ? L"Create an account to use your library on every device."
                                   : L"Sign in to get your playlists on your phone and reach this computer from anywhere.",
                 12.5f);
            if (account_creating_) field(kAccountName, L"Your name", L"");
            field(kAccountEmail, L"Email", L"you@example.com");
            field(kAccountPassword, L"Password (8+ characters)", L"", true);
            auto submit = [this] {
                const std::string email = trim(edit_text(kAccountEmail)), password = edit_text(kAccountPassword), name = trim(edit_text(kAccountName));
                const bool creating = account_creating_;
                account_job(creating ? "Creating your account…" : "Signing in…", [this, email, password, name, creating] {
                    if (creating) account::sign_up(email, password, name);
                    else account::sign_in(email, password);
                    try {
                        account::upload_library(store_);
                    } catch (const std::exception& e) {
                        return std::string("Signed in, but the library didn't upload: ") + e.what();
                    }
                    return std::string("Signed in — your library is saved to your account.");
                });
                if (const auto it = edits_.find(kAccountPassword); it != edits_.end()) SetWindowTextW(it->second.hwnd, L"");
            };
            buttons({{busy ? L"Please wait…" : (account_creating_ ? L"Create account" : L"Sign in"), icon::check, Ui::Pill::primary,
                      busy ? std::function<void()>{} : submit},
                     {account_creating_ ? L"I have an account" : L"Create an account", icon::arrow_right, Ui::Pill::glass,
                      [this] { account_creating_ = !account_creating_; }}});
        }
        if (!message.empty()) para(widen(message), 12.5f, text2, 600);
    });

    // Use from anywhere.
    if (signed_in)
        section(L"Use from anywhere", tunnel_->running(), [&] {
            para(L"Your phone (signed in to the same account) can stream and download from this computer away from home — while it's on and "
                 L"WreckBox is open.",
                 12.5f);
            para(widen(tunnel_->status()), 12.5f, tunnel_->running() ? lilac : text2, 600);
            const bool want = s.share_remotely;
            buttons({{want ? L"Turn off" : L"Turn on", want ? icon::close : icon::check, want ? Ui::Pill::glass : Ui::Pill::primary,
                      [this, want] { set_share_remotely(!want); }}});
        });

    // The password box and friends exist only while their section is on screen.
    ui_.pop_clip();
    y += 30;
    const float before = phone_scroll_;
    ui_.scroll_area(r, phone_scroll_, y + phone_scroll_ - r.t);
    if (phone_scroll_ != before) InvalidateRect(hwnd_, nullptr, FALSE);
}

}  // namespace wb::ui
