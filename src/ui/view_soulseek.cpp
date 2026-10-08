// Soulseek sync and Download queue: start / stop the sync, what it
// downloaded / couldn't find / failed with retry (also with your own search words) and ignore, its log; and the priority list
// that decides what it downloads first. The work is in net/soulseek.
#include <format>

#include "model/settings.h"
#include "net/soulseek.h"
#include "ui/view.h"

namespace wb::ui {

using namespace theme;

void View::attach_soulseek(soulseek::Sync& sl) { slsk_ = &sl; }

void View::slsk_message(std::string s) {
    {
        std::lock_guard lock(status_m_);
        slsk_msg_ = std::move(s);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// MARK: Soulseek sync

void View::soulseek_page(const Rect& r) {
    if (!slsk_) return;
    std::string message;
    {
        std::lock_guard lock(status_m_);
        message = slsk_msg_;
    }
    const bool running = slsk_->running();
    const std::wstring toggle = running ? L"Stop" : L"Start sync";
    float y = r.t + header(r, L"Tools", L"Soulseek sync",
                           running ? L"Running — checks your playlists again every 30 minutes" : L"Stopped",
                           {{ui_.pill_width(toggle, running ? icon::close : icon::play), [this, toggle, running](float x, float py) {
                                 ui_.pill(x, py, toggle, running ? icon::close : icon::play, running ? Ui::Pill::glass : Ui::Pill::smart, [this, running] {
                                     if (running) {
                                         slsk_->stop();
                                         slsk_message("");
                                     } else {
                                         slsk_message(slsk_->start());
                                     }
                                 });
                             }}});
    if (message.empty() && (!soulseek::Sync::available() || !slsk_->configured()))
        message = !soulseek::Sync::available() ? "The Soulseek component is missing from this install." : "Add your Soulseek login in Settings to start.";
    if (!message.empty()) {
        g_.text(widen(message), Rect{r.l, y, r.r, y + 22}, {Font::ui, 13, 600, peach});
        y += 30;
    }

    // Tabs and "Retry all".
    const auto records = slsk_->records();
    struct Tab {
        const char* id;
        const wchar_t* label;
        int n;
    };
    const Tab tabs[] = {{"done", L"Downloaded", slsk_->done()}, {"not_found", L"Not found", slsk_->not_found()}, {"failed", L"Failed", slsk_->failed()}};
    float x = r.l;
    for (const auto& t : tabs) x += ui_.chip(x, y, t.label, t.n, slsk_tab_ == t.id, false, [this, id = t.id] { slsk_tab_ = id, slsk_scroll_ = 0; }) + 8;
    std::vector<std::pair<std::string, const soulseek::SyncRecord*>> entries;
    for (const auto& [id, rec] : records)
        if (rec.status == slsk_tab_ && store_.track(id)) entries.push_back({id, &rec});
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.second->last_try > b.second->last_try; });
    if (slsk_tab_ != "done" && !entries.empty()) {
        const std::wstring label = std::format(L"Retry all {}", entries.size());
        ui_.pill(r.r - ui_.pill_width(label, icon::sync), y - 3, label, icon::sync, Ui::Pill::smart, [this, entries] {
            std::vector<std::string> ids;
            for (const auto& e : entries) ids.push_back(e.first);
            jobs_.run([this, ids] { slsk_->retry(ids); });
        });
    }
    y += 42;

    const float bottom = r.b - 8, gap = 12, left_w = (r.w() - gap) * 0.6f;
    const Rect results{r.l, y, r.l + left_w, bottom}, logbox{results.r + gap, y, r.r, bottom};
    ui_.glass(results);
    constexpr float row_h = 58, pad = 6;
    const Rect area = results.inset(0, 1);
    if (entries.empty()) g_.text(L"Nothing here.", area, {Font::ui, 13, 400, text3, Align::center});
    else {
        const float before = slsk_scroll_;
        ui_.scroll_area(area, slsk_scroll_, float(entries.size()) * row_h + 2 * pad);
        if (slsk_scroll_ != before) InvalidateRect(hwnd_, nullptr, FALSE);
        ui_.push_clip(area);
        const size_t first = size_t(std::max(0.f, (slsk_scroll_ - pad) / row_h));
        const size_t last = std::min(entries.size(), first + size_t(area.h() / row_h) + 2);
        for (size_t i = first; i < last; ++i) {
            const auto& [id, rec] = entries[i];
            const auto t = store_.track(id);
            const float ry = area.t + pad + float(i) * row_h - slsk_scroll_;
            const Rect row{area.l + pad, ry, area.r - pad - 8, ry + row_h};
            if (ui_.hover(row)) g_.fill_round(row, 12, hover);
            const float cy = (row.t + row.b) / 2;
            ui_.artwork(*t, Rect::xywh(row.l + 10, cy - 20, 40, 40), 8);
            float right = row.r - 8;
            if (slsk_tab_ != "done") {
                const auto btn = [&](const wchar_t* glyph, const wchar_t* tip, std::function<void()> fn) {
                    right -= 32;
                    ui_.icon_button(Rect::xywh(right, cy - 16, 32, 32), glyph, 14, std::move(fn), false, false, tip);
                    right -= 2;
                };
                btn(icon::block, L"Ignore", [this, id] { store_.set_status({id}, TrackStatus::ignored); });
                btn(icon::search, L"Retry with your own search words", [this, id, tr = *t] {
                    slsk_query_for_ = id;
                    slsk_query_default_ = (tr.artists.empty() ? std::string() : tr.artists[0] + " ") + tr.title;
                    slsk_query_open_ = true;
                    slsk_query_focused_ = false;
                });
                btn(icon::sync, L"Retry", [this, id] { jobs_.run([this, id] { slsk_->retry({id}); }); });
            }
            std::wstring sub = widen(t->artist());
            const auto add = [&](const std::string& s) {
                if (!s.empty()) sub += L" · " + widen(s);
            };
            add(rec->reason.value_or(""));
            std::string fmt = rec->format.value_or("");
            for (auto& c : fmt) c = char(std::toupper(static_cast<unsigned char>(c)));
            add(fmt);
            add(std::to_string(rec->attempts) + (rec->attempts == 1 ? " try" : " tries"));
            if (slsk_->retry_pending(id)) add("retry queued");
            g_.text(widen(t->title), Rect{row.l + 60, cy - 20, right - 6, cy}, {Font::ui, 13.5f, 600, text});
            g_.text(sub, Rect{row.l + 60, cy, right - 6, cy + 20}, {Font::ui, 11.5f, 400, text3});
            ui_.click(row, [this, id] { store_.set_focus(id); });
        }
        ui_.pop_clip();
    }

    ui_.glass(logbox);
    ui_.dot_label(L"Log", logbox.l + 16, logbox.t + 22, text3);
    const Rect lines = Rect{logbox.l + 14, logbox.t + 40, logbox.r - 8, logbox.b - 10};
    ui_.push_clip(lines);
    float ly = lines.t;
    for (const auto& l : slsk_->recent()) {
        if (ly > lines.b) break;
        const bool ok = l.find("\xE2\x9C\x93") != std::string::npos, bad = l.find("\xE2\x9C\x97") != std::string::npos;
        const TextStyle st{Font::ui, 11.5f, 400, ok ? lilac : bad ? peach : text2};
        const float h = std::max(16.f, g_.measure_height(widen(l), lines.w() - 12, st));
        g_.paragraph(widen(l), Rect{lines.l, ly, lines.r - 8, ly + h}, st);
        ly += h + 4;
    }
    ui_.pop_clip();
}

void View::slsk_query_dialog() {
    const float W = g_.width(), H = g_.height();
    g_.fill(Rect{0, 0, W, H}, argb(0x99000000));
    ui_.click(Rect{0, 0, W, H}, [this] { close_slsk_query(); });
    const Rect p = Rect::xywh((W - 540) / 2, (H - 196) / 2, 540, 196);
    ui_.glass(p, 22, false, true);
    ui_.click(p, [] {});
    const Rect in = p.inset(24, 22);
    g_.text(L"Custom search", Rect{in.l, in.t, in.r, in.t + 24}, {Font::ui, 18, 600, text});
    g_.text(L"WreckBox searches Soulseek for these words on its next pass (within a few seconds if it's running).",
            Rect{in.l, in.t + 26, in.r, in.t + 44}, {Font::ui, 12.5f, 400, text2});
    const Rect field{in.l, in.t + 56, in.r, in.t + 96};
    constexpr COLORREF fill = RGB(30, 30, 32);
    const HWND h = edit(kSlskQuery, field.inset(14, 11), false, L"artist title", slsk_query_default_, fill);
    g_.fill_round(field, 14, argb(0xFF1E1E20));
    g_.stroke_round(field, 14, GetFocus() == h ? with_alpha(lilac, 0.6f) : hairline);
    if (!slsk_query_focused_) slsk_query_focused_ = true, SetFocus(h), SendMessageW(h, EM_SETSEL, 0, -1);
    const std::wstring ok = L"Search on next pass";
    const float bw = ui_.pill_width(ok, icon::search), cw = ui_.pill_width(L"Cancel");
    ui_.pill(in.r - bw, in.b - 34, ok, icon::search, Ui::Pill::primary, [this] { slsk_query_submit(); });
    ui_.pill(in.r - bw - 10 - cw, in.b - 34, L"Cancel", nullptr, Ui::Pill::glass, [this] { close_slsk_query(); });
}

void View::slsk_query_submit() {
    std::string q = edit_text(kSlskQuery);
    q.erase(0, q.find_first_not_of(" \t\r\n"));
    q.erase(q.find_last_not_of(" \t\r\n") + 1);
    const std::string id = slsk_query_for_;
    close_slsk_query();
    if (!q.empty() && slsk_) jobs_.run([this, id, q] { slsk_->retry({id}, q); });
}

void View::close_slsk_query() {
    slsk_query_open_ = false;
    if (const auto it = edits_.find(kSlskQuery); it != edits_.end()) SetWindowTextW(it->second.hwnd, L"");
    SetFocus(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// MARK: Download queue

void View::queue_page(const Rect& r) {
    if (!slsk_) return;
    ui_.push_clip(Rect{r.l - 22, r.t, r.r + 22, r.b});
    float y = r.t - queue_scroll_;
    y += header(Rect{r.l, y, r.r, r.b}, L"Tools", L"Download queue", L"Soulseek downloads missing tracks in this order");
    const float width = std::min(r.w(), 780.f), pad = 18;
    const auto lib = lib_;
    const auto state = store_.state_copy();
    const auto& prios = state.download_priority;
    // Missing tracks per playlist.
    auto missing_in = [&](const std::string& name) {
        size_t n = 0;
        if (!lib) return n;
        std::set<std::string> seen;
        for (const auto& pl : lib->playlists)
            if (pl.name == name)
                for (const auto& id : pl.track_ids) {
                    const auto st = state.tracks.find(id);
                    if ((st == state.tracks.end() || st->second.status == TrackStatus::missing) && seen.insert(id).second) ++n;
                }
        return n;
    };
    const float rows = float(prios.size()) * 58;
    const float height = pad + 24 + (prios.empty() ? 40 : rows) + 50 + 20 + 60 + pad;
    const Rect panel{r.l, y, r.l + width, y + height};
    ui_.glass(panel, 24);
    float x = panel.l + pad, py = panel.t + pad;
    ui_.dot_label(L"Priority", x, py + 7, text);
    py += 28;
    if (prios.empty())
        g_.paragraph(L"Add playlists below. Their missing tracks go to the front of the queue, top to bottom.", Rect{x, py, panel.r - pad, py + 36}, {Font::ui, 13, 400, text2});
    if (prios.empty()) py += 40;
    auto set = [this](std::vector<std::string> p, std::optional<bool> only = std::nullopt) {
        store_.set_download_priority(std::move(p), only);
        jobs_.run([this] { slsk_->write_queue(); });
    };
    for (size_t i = 0; i < prios.size(); ++i) {
        const Rect row{x, py, panel.r - pad, py + 52};
        g_.fill_round(row, 12, glass_fill);
        const std::string name = prios[i].substr(prios[i].find(':') + 1);
        g_.text(std::to_wstring(i + 1), Rect{row.l + 12, row.t, row.l + 38, row.b}, {Font::dot, 16, 700, lilac});
        g_.text(widen(name), Rect{row.l + 44, row.t + 8, row.r - 120, row.t + 28}, {Font::ui, 13.5f, 600, text});
        g_.text(std::format(L"{} missing", missing_in(name)), Rect{row.l + 44, row.t + 28, row.r - 120, row.t + 46}, {Font::ui, 11, 400, text3});
        float bx = row.r - 8;
        const auto btn = [&](const wchar_t* glyph, bool enabled, std::function<void()> fn) {
            bx -= 32;
            ui_.icon_button(Rect::xywh(bx, row.t + 10, 32, 32), glyph, 13, enabled ? std::move(fn) : std::function<void()>{});
        };
        btn(icon::close, true, [prios, i, set] {
            auto p = prios;
            p.erase(p.begin() + std::ptrdiff_t(i));
            set(p);
        });
        btn(icon::down, i + 1 < prios.size(), [prios, i, set] {
            auto p = prios;
            std::swap(p[i], p[i + 1]);
            set(p);
        });
        btn(icon::up, i > 0, [prios, i, set] {
            auto p = prios;
            std::swap(p[i], p[i - 1]);
            set(p);
        });
        py += 58;
    }
    py += 2;
    std::vector<std::string> available;
    if (lib)
        for (const auto& pl : lib->playlists)
            if (std::find(prios.begin(), prios.end(), "playlist:" + pl.name) == prios.end()) available.push_back(pl.name);
    ui_.pill(x, py, L"Add playlist", icon::add, Ui::Pill::glass, available.empty() ? std::function<void()>{} : [this, available, prios, set] {
        HMENU m = CreatePopupMenu();
        for (size_t i = 0; i < available.size(); ++i) AppendMenuW(m, MF_STRING, UINT_PTR(i + 1), widen(available[i]).c_str());
        POINT pt;
        GetCursorPos(&pt);
        const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
        DestroyMenu(m);
        if (cmd > 0) {
            auto p = prios;
            p.push_back("playlist:" + available[size_t(cmd - 1)]);
            set(p);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    });
    py += 50;
    g_.line(x, py, panel.r - pad, py, hairline);
    py += 14;
    const bool everything = !state.priority_only;
    g_.text(L"Then everything else", Rect{x, py, panel.r - 160, py + 22}, {Font::ui, 13.5f, 600, text});
    g_.text(L"Off: download only your priorities", Rect{x, py + 22, panel.r - 160, py + 40}, {Font::ui, 11.5f, 400, text3});
    const std::wstring label = everything ? L"On" : L"Off";
    ui_.chip(panel.r - pad - ui_.chip_width(label) - (everything ? 12 : 0), py + 4, label, std::nullopt, everything, false, [prios, everything, set] { set(prios, everything); });
    y = panel.b + 30;
    ui_.pop_clip();
    const float before = queue_scroll_;
    ui_.scroll_area(r, queue_scroll_, y + queue_scroll_ - r.t);
    if (queue_scroll_ != before) InvalidateRect(hwnd_, nullptr, FALSE);
}

}  // namespace wb::ui
