// Soulseek sync and Download queue: start / stop the sync, what it wants next, what it downloaded / couldn't find / failed
// with retry (also with your own search words) and ignore, its log, storage; the priority list that decides what it
// downloads first; and picking songs / playlists to download and deleting songs. The work is in net/soulseek.
#include <filesystem>
#include <format>

#include "model/paths.h"
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

// MARK: Picking and deleting

void View::want(const std::vector<std::string>& ids, bool on) {
    const auto state = store_.state_copy();
    auto prios = state.download_priority;
    std::vector<std::string> unskip, skip, fetch;
    for (const auto& id : ids) {
        if (!store_.track(id)) continue;  // a My folders file that isn't in the library
        const auto st = state.tracks.find(id);
        const TrackStatus status = st == state.tracks.end() ? TrackStatus::missing : st->second.status;
        if (status == TrackStatus::downloaded) continue;
        const std::string key = "track:" + id;
        std::erase(prios, key);
        if (on) {
            prios.push_back(key);
            if (status == TrackStatus::ignored) unskip.push_back(id);
            fetch.push_back(id);
        } else if (status == TrackStatus::missing) {
            skip.push_back(id);
        }
    }
    // Picks of songs that are on this PC now have done their job.
    std::erase_if(prios, [&](const std::string& k) {
        if (!k.starts_with("track:")) return false;
        const auto st = state.tracks.find(k.substr(6));
        return st != state.tracks.end() && st->second.status == TrackStatus::downloaded;
    });
    const size_t n = on ? fetch.size() : skip.size();
    if (n == 0) {
        show_toast(on ? L"Those songs are already on this PC" : L"Nothing to skip");
        return;
    }
    const bool can_run = slsk_ && soulseek::Sync::available() && slsk_->configured();
    auto why = std::make_shared<std::string>();
    jobs_.run(
        [this, prios, unskip, skip, fetch, on, can_run, why] {
            if (!unskip.empty()) store_.set_status(unskip, TrackStatus::missing);
            if (!skip.empty()) store_.set_status(skip, TrackStatus::ignored);
            store_.set_download_priority(prios);
            if (!slsk_) return;
            if (on) slsk_->retry(fetch);  // first in line, even if an earlier try gave up
            slsk_->write_queue();
            if (on && can_run && !slsk_->running()) *why = slsk_->start();
        },
        [this, n, on, can_run, why] {
            dirty_ = true;
            const std::wstring songs = n == 1 ? L"1 song" : std::format(L"{} songs", n);
            if (!on) show_toast(songs + L" won't download");
            else if (!can_run) show_toast(L"Picked " + songs + L" · add your Soulseek login in Settings to download");
            else if (!why->empty()) show_toast(widen(*why));
            else show_toast(L"Downloading " + songs);
        });
}

void View::pick_playlist(const std::string& name, bool on) {
    auto prios = store_.state_copy().download_priority;
    const std::string key = "playlist:" + name;
    std::erase(prios, key);
    if (on) prios.push_back(key);
    const bool can_run = slsk_ && soulseek::Sync::available() && slsk_->configured();
    jobs_.run(
        [this, prios, on, can_run] {
            store_.set_download_priority(prios);
            if (!slsk_) return;
            slsk_->write_queue();
            if (on && can_run && !slsk_->running()) slsk_->start();
        },
        [this, on, can_run, name] {
            dirty_ = true;
            show_toast(!on       ? widen(name) + L" won't download any more"
                       : can_run ? L"Downloading " + widen(name)
                                 : L"Picked " + widen(name) + L" · add your Soulseek login in Settings to download");
        });
}

void View::delete_songs(const std::vector<std::string>& ids) {
    confirm_delete_.clear();
    struct Target {
        std::string path;
        bool own;  // a My folders file: it just goes; a library song also becomes skipped
    };
    std::vector<Target> targets;
    uint64_t bytes = 0;
    std::wstring title;
    for (const auto& id : ids) {
        const auto tr = store_.row(id);
        if (!tr || tr->status() != TrackStatus::downloaded || !tr->state || !tr->state->local_path) continue;
        if (const player::Item* now = player_.current(); now && now->track_id == id) player_.stop();  // VLC holds it open
        std::error_code ec;
        bytes += std::filesystem::file_size(widen(*tr->state->local_path), ec);
        targets.push_back({*tr->state->local_path, !tr->file_id.empty()});
        title = widen(tr->track.title);
    }
    if (targets.empty()) return;
    auto result = std::make_shared<std::pair<size_t, std::string>>();  // deleted, the last error
    jobs_.run(
        [this, targets, result] {
            for (const auto& t : targets) {
                const std::string err = store_.delete_file(t.path, true, !t.own);
                if (err.empty()) ++result->first;
                else result->second = err;
            }
        },
        [this, result, bytes, title, total = targets.size()] {
            dirty_ = true;
            const auto& [done, err] = *result;
            if (done == 0) show_toast(widen(err));
            else if (total == 1) show_toast(L"Moved to the Recycle Bin: " + title);
            else show_toast(std::format(L"Moved {} songs ({}) to the Recycle Bin", done, size_text(bytes)) + (err.empty() ? L"" : L" · " + widen(err)));
        });
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

    // Storage (measured at most every 10 s: it reads every downloaded file's size).
    if (GetTickCount64() - storage_at_ > 10000 || storage_at_ == 0) {
        storage_at_ = GetTickCount64();
        downloads_bytes_ = 0;
        const auto tracks_dir = paths::tracks();
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(tracks_dir, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec)) downloads_bytes_ += it->file_size(ec);
        const auto space = std::filesystem::space(paths::root(), ec);
        free_disk_ = ec ? 0 : space.available;
        slsk_smaller_ = slsk_->prefer_smaller();
    }
    g_.text(std::format(L"WreckBox's songs use {} · {} free on this drive", size_text(downloads_bytes_), size_text(free_disk_)),
            Rect{r.l, y, r.r, y + 20}, {Font::ui, 13, 400, text2});
    y += 30;

    // What downloads, and in what quality.
    const auto state = store_.state_copy();
    float x = r.l;
    x += ui_.dot_label(L"Download", x, y + 15, text3, 10) + 10;
    for (const auto& [label, only] : {std::pair{L"Only what I pick", true}, std::pair{L"Everything missing", false}})
        x += ui_.chip(x, y, label, std::nullopt, state.priority_only == only, false, [this, only = only, prios = state.download_priority] {
                 store_.set_download_priority(prios, only);
                 jobs_.run([this] { slsk_->write_queue(); });
             }) + 6;
    x += 18;
    x += ui_.dot_label(L"Quality", x, y + 15, text3, 10) + 10;
    for (const auto& [label, smaller] : {std::pair{L"Best (FLAC first)", false}, std::pair{L"Smaller (MP3 320 first)", true}})
        x += ui_.chip(x, y, label, std::nullopt, slsk_smaller_.value_or(false) == smaller, false, [this, smaller = smaller] {
                 slsk_smaller_ = smaller;
                 jobs_.run([this, smaller] { slsk_->set_prefer_smaller(smaller); });
             }) + 6;
    y += 46;

    // Tabs and "Retry all".
    const auto records = slsk_->records();
    const auto wanted = slsk_->wanted();
    struct Tab {
        const char* id;
        const wchar_t* label;
        int n;
    };
    const Tab tabs[] = {{"wanted", L"Wanted", int(wanted.size())},
                        {"done", L"Downloaded", slsk_->done()},
                        {"not_found", L"Not found", slsk_->not_found()},
                        {"failed", L"Failed", slsk_->failed()}};
    x = r.l;
    for (const auto& t : tabs) x += ui_.chip(x, y, t.label, t.n, slsk_tab_ == t.id, false, [this, id = t.id] { slsk_tab_ = id, slsk_scroll_ = 0; }) + 8;
    const bool wanted_tab = slsk_tab_ == "wanted";
    std::vector<std::pair<std::string, const soulseek::SyncRecord*>> entries;
    if (wanted_tab) {  // the queue, in order
        for (const auto& id : wanted)
            if (store_.track(id)) {
                const auto rec = records.find(id);
                entries.push_back({id, rec == records.end() ? nullptr : &rec->second});
            }
    } else {
        for (const auto& [id, rec] : records)
            if (rec.status == slsk_tab_ && store_.track(id)) entries.push_back({id, &rec});
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.second->last_try > b.second->last_try; });
    }
    if (!wanted_tab && slsk_tab_ != "done" && !entries.empty()) {
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
    if (entries.empty())
        g_.text(wanted_tab ? L"Nothing to download. Pick songs or playlists with Download (right-click a song, or a playlist's Download button)."
                           : L"Nothing here.",
                area.inset(20, 0), {Font::ui, 13, 400, text3, Align::center});
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
            if (wanted_tab) {
                const auto btn = [&](const wchar_t* glyph, const wchar_t* tip, std::function<void()> fn) {
                    right -= 32;
                    ui_.icon_button(Rect::xywh(right, cy - 16, 32, 32), glyph, 14, std::move(fn), false, false, tip);
                    right -= 2;
                };
                btn(icon::close, L"Don't download", [this, id] { want({id}, false); });
                if (i > 0) btn(icon::up, L"Download first", [this, id] { jobs_.run([this, id] { slsk_->retry({id}); }); });
            } else if (slsk_tab_ != "done") {
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
            if (rec) {
                add(rec->reason.value_or(""));
                std::string fmt = rec->format.value_or("");
                for (auto& c : fmt) c = char(std::toupper(static_cast<unsigned char>(c)));
                add(fmt);
                add(std::to_string(rec->attempts) + (rec->attempts == 1 ? " try" : " tries"));
            }
            if (wanted_tab && i == 0) add("next");
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
    // Single-song picks ("track:<id>") are listed in the Soulseek page's Wanted tab; this list keeps playlists / genres
    // and leaves those where they are when it reorders.
    std::vector<std::string> prios, picks;
    for (const auto& k : state.download_priority) (k.starts_with("track:") ? picks : prios).push_back(k);
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
    auto set = [this, picks](std::vector<std::string> p, std::optional<bool> only = std::nullopt) {
        p.insert(p.end(), picks.begin(), picks.end());
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
    g_.text(L"Off: download only what you pick (these playlists, and songs you chose)", Rect{x, py + 22, panel.r - 160, py + 40},
            {Font::ui, 11.5f, 400, text3});
    const std::wstring label = everything ? L"On" : L"Off";
    ui_.chip(panel.r - pad - ui_.chip_width(label) - (everything ? 12 : 0), py + 4, label, std::nullopt, everything, false, [prios, everything, set] { set(prios, everything); });
    y = panel.b + 30;
    ui_.pop_clip();
    const float before = queue_scroll_;
    ui_.scroll_area(r, queue_scroll_, y + queue_scroll_ - r.t);
    if (queue_scroll_ != before) InvalidateRect(hwnd_, nullptr, FALSE);
}

}  // namespace wb::ui
