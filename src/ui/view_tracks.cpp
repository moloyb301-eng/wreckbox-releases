// Track list pages and the inspector (port of app/lib/ui/tracks.dart).
#include <shlobj.h>

#include <algorithm>
#include <format>

#include "library/matcher.h"
#include "model/settings.h"
#include "sources/sync.h"
#include "ui/view.h"

namespace wb::ui {

using namespace theme;

namespace {

std::optional<double> parse_number(const std::wstring& s) {
    if (s.empty()) return std::nullopt;
    try {
        return std::stod(s);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

int compare_ci(const std::string& a, const std::string& b) {
    const auto wa = widen(a), wb_ = widen(b);
    return CompareStringOrdinal(wa.c_str(), int(wa.size()), wb_.c_str(), int(wb_.size()), TRUE) - CSTR_EQUAL;
}

// Column widths of the desktop list, right to left: status 16 (+10 gap), duration 50, energy 60, key 70, BPM 60.
constexpr float kStatus = 26, kDuration = 50, kEnergy = 60, kKey = 70, kBpm = 60;

}  // namespace

// MARK: Derived data

void View::refresh() {
    dirty_ = false;
    refreshed_at_ = GetTickCount64();
    lib_ = store_.library();
    n_downloaded_ = store_.count(TrackStatus::downloaded);
    n_missing_ = store_.count(TrackStatus::missing);
    n_ignored_ = store_.count(TrackStatus::ignored);
    n_analysed_ = store_.analysis_count();
    n_priority_ = store_.state_copy().download_priority.size();
    mixes_for_.reset();

    recent_.clear();
    if (lib_) {
        std::vector<const LibraryTrack*> ts;
        for (const auto& t : lib_->tracks) ts.push_back(&t);
        const size_t n = std::min<size_t>(16, ts.size());
        std::partial_sort(ts.begin(), ts.begin() + std::ptrdiff_t(n), ts.end(),
                          [](const auto* a, const auto* b) { return a->first_added.value_or("") > b->first_added.value_or(""); });
        for (size_t i = 0; i < n; ++i) recent_.push_back(ts[i]->id);
    }

    ids_.clear();
    if (!lib_ || page_ == Page::home) return;
    if (page_ == Page::playlist && playlist_) {
        const auto& s = Settings::current();
        const std::string key = *playlist_ + "|" + lib_->built_at + "|" + s.spotify_client_id + "|" + s.google_client_id;
        if (key != sync_key_) {
            sync_key_ = key;
            const auto pl = std::find_if(lib_->playlists.begin(), lib_->playlists.end(), [&](const auto& p) { return p.name == *playlist_; });
            sync_unavailable_ = pl == lib_->playlists.end() ? std::optional<std::string>("This playlist is no longer in your library.")
                                                             : sync::unavailable(*pl);
        }
    }
    const ListFilter filter = list_filter();
    ids_ = store_.row_ids(filter, playlist_, narrow(search_text_));
    list_total_ = store_.row_ids(filter, playlist_).size();
    list_crate_ = filter == ListFilter::all || filter == ListFilter::downloaded ? store_.row_ids(ListFilter::downloaded, playlist_).size() : 0;
    if (!mix_.active() && sort_ == Sort::none) return;

    // Filtering by BPM / key and sorting need each row's analysis.
    std::vector<TrackRow> rows;
    rows.reserve(ids_.size());
    const auto lo = parse_number(mix_.min_bpm), hi = parse_number(mix_.max_bpm);
    const auto keys = mix_.key ? compatible_keys(*mix_.key) : std::set<std::string>{};
    for (const auto& id : ids_) {
        auto r = store_.row(id);
        if (!r) continue;
        const auto b = r->bpm();
        if (lo && (!b || *b < *lo)) continue;
        if (hi && (!b || *b > *hi)) continue;
        if (mix_.key && (mix_.compatible ? !keys.contains(r->camelot()) : r->camelot() != *mix_.key)) continue;
        rows.push_back(std::move(*r));
    }
    auto by = [this](auto key) {
        return [this, key](const TrackRow& a, const TrackRow& b) { return asc_ ? key(a) < key(b) : key(b) < key(a); };
    };
    switch (sort_) {
        case Sort::title:
            std::stable_sort(rows.begin(), rows.end(), [this](const TrackRow& a, const TrackRow& b) {
                const int c = compare_ci(a.track.title, b.track.title);
                return asc_ ? c < 0 : c > 0;
            });
            break;
        case Sort::artist:
            std::stable_sort(rows.begin(), rows.end(), [this](const TrackRow& a, const TrackRow& b) {
                const int c = compare_ci(a.track.artist(), b.track.artist());
                return asc_ ? c < 0 : c > 0;
            });
            break;
        case Sort::bpm: std::stable_sort(rows.begin(), rows.end(), by([](const TrackRow& r) { return r.bpm().value_or(-1); })); break;
        case Sort::key:
            std::stable_sort(rows.begin(), rows.end(), by([](const TrackRow& r) { return camelot_order(r.camelot().empty() ? std::nullopt : std::optional(r.camelot())); }));
            break;
        case Sort::energy: std::stable_sort(rows.begin(), rows.end(), by([](const TrackRow& r) { return r.energy().value_or(-1); })); break;
        case Sort::none: break;
    }
    ids_.clear();
    for (const auto& r : rows) ids_.push_back(r.id());
}

// MARK: Track page

void View::track_page(const Rect& r) {
    std::wstring title;
    switch (page_) {
        case Page::downloaded: title = L"In my crate"; break;
        case Page::missing: title = L"Missing"; break;
        case Page::ignored: title = L"Ignored"; break;
        default: title = playlist_ ? wide(*playlist_) : L"All tracks";
    }
    auto actions = library_actions(page_ == Page::downloaded);
    const bool is_playlist = page_ == Page::playlist && playlist_;
    if (is_playlist) {
        // Sync: fetch this playlist again from its source (new songs in, removed songs out of the playlist).
        const bool can = !sync_unavailable_ && !store_.busy();
        actions.insert(actions.begin(), Action{ui_.pill_width(L"Sync", icon::sync), [this, can](float x, float y) {
                                                   ui_.pill(x, y, L"Sync", icon::sync, Ui::Pill::smart,
                                                            can ? std::function<void()>{[this] { sync_playlist(); }} : std::function<void()>{});
                                               }});
    }
    float h = header(r, playlist_ ? L"Playlist" : L"Library", title, std::format(L"{} tracks · {} in your crate", list_total_, list_crate_), actions);
    // The last sync's result, or why this playlist can't sync.
    if (is_playlist && (!sync_message_.empty() || sync_unavailable_)) {
        const bool msg = !sync_message_.empty(), failed = sync_message_.rfind("Sync failed", 0) == 0;
        g_.text(widen(msg ? sync_message_ : *sync_unavailable_), Rect{r.l, r.t + h - 8, r.r, r.t + h + 12},
                {Font::ui, 12, msg ? 600 : 400, failed ? peach : msg ? lilac : text3});
        h += 18;
    }
    filters(Rect{r.l, r.t + h, r.r, r.t + h + 38});
    list(Rect{r.l, r.t + h + 50, r.r, r.b});
}

void View::sync_playlist() {
    if (!lib_ || !playlist_ || store_.busy()) return;
    const auto it = std::find_if(lib_->playlists.begin(), lib_->playlists.end(), [&](const auto& p) { return p.name == *playlist_; });
    if (it == lib_->playlists.end()) return;
    const LibraryPlaylist pl = *it;
    sync_message_.clear();
    store_.set_busy("Syncing " + pl.name + "…");
    auto result = std::make_shared<std::pair<sync::Result, std::string>>();  // (result, error)
    jobs_.run(
        [this, pl, result] {
            try {
                result->first = sync::playlist(pl, [this](const std::string& line) { store_.set_busy(line); });
                store_.load();
                store_.log("playlist sync", pl.name + ": " + std::to_string(result->first.added) + " new, " +
                                                std::to_string(result->first.removed) + " removed");
                store_.save();
            } catch (const std::exception& e) {
                result->second = e.what();
            }
            store_.set_busy(std::nullopt);
        },
        [this, pl, result] {
            const auto& [r, error] = *result;
            if (!error.empty()) {
                sync_message_ = "Sync failed: " + error;
                return;
            }
            if (!r.name.empty() && r.name != pl.name) playlist_ = r.name;  // renamed at the source
            sync_message_ = r.added == 0 && r.removed == 0 ? "Synced — already up to date."
                                                           : "Synced — " + std::to_string(r.added) + " new, " + std::to_string(r.removed) + " removed.";
            dirty_ = true;
        });
}

void View::filters(const Rect& r) {
    // Search: a native text box laid over a drawn pill.
    const Rect box = Rect::xywh(r.l, r.t, 260, 38);
    g_.fill_round(box, 19, glass_fill);
    g_.stroke_round(box, 19, GetFocus() == search_ ? with_alpha(lilac, 0.6f) : hairline);
    g_.icon(icon::search, box.l + 22, box.t + 19, 14, text3);
    edit(kSearchBox, Rect{box.l + 38, box.t + 10, box.r - 14, box.b - 9});
    ui_.click(box, [this] { SetFocus(search_); });

    float x = box.r + 10;
    const float cy = r.t + 19;
    x += ui_.dot_label(L"BPM", x, cy, text3, 10) + 8;
    static const std::tuple<const wchar_t*, const wchar_t*, const wchar_t*> presets[] = {
        {L"< 100", L"", L"99"}, {L"100–120", L"100", L"120"}, {L"120–130", L"120", L"130"}, {L"130–145", L"130", L"145"}, {L"145+", L"145", L""}};
    auto changed = [this] {
        list_scroll_ = 0;
        dirty_ = true;
    };
    for (const auto& [label, lo, hi] : presets) {
        const bool on = mix_.min_bpm == lo && mix_.max_bpm == hi;
        x += ui_.chip(x, r.t + 4, label, std::nullopt, on, false, [this, on, lo, hi, changed] {
            mix_.min_bpm = on ? L"" : lo;
            mix_.max_bpm = on ? L"" : hi;
            changed();
        }) + 6;
    }
    if (!mix_.key) {
        x += ui_.chip(x, r.t + 4, L"Any key", std::nullopt, false, false, [this] { key_menu(); }) + 6;
    } else {
        const float w = ui_.key_badge(x, cy, *mix_.key);
        ui_.click(Rect{x, r.t, x + w, r.b}, [this] { key_menu(); });
        x += w + 6;
        x += ui_.chip(x, r.t + 4, L"+ compatible", std::nullopt, false, mix_.compatible, [this, changed] {
            mix_.compatible = !mix_.compatible;
            changed();
        }) + 6;
    }
    if (mix_.active()) {
        const Rect clear = Rect::xywh(x + 4, r.t + 4, g_.measure(L"Clear", {Font::ui, 12.5f, 600}) + 16, 30);
        g_.text(L"Clear", clear, {Font::ui, 12.5f, 600, ui_.hover(clear) ? text : text2, Align::center});
        ui_.click(clear, [this, changed] {
            mix_ = {};
            changed();
        });
    }
}

void View::list(const Rect& r) {
    ui_.glass(r);
    // Column header (click to sort: ascending → descending → off).
    const float hy = r.t + 14, hb = r.t + 36;
    const float right = r.r - 16;
    const float x_status = right - kStatus, x_dur = x_status - kDuration, x_energy = x_dur - kEnergy, x_key = x_energy - kKey,
                x_bpm = x_key - kBpm;
    auto head = [&](const wchar_t* label, Sort k, float x0, float x1) {
        const bool on = sort_ == k;
        const Rect hr{x0, hy, x1, hb - 4};
        const float w = ui_.dot_label(label, x0, (hr.t + hr.b) / 2, on || ui_.hover(hr) ? text : text3, 10);
        if (on) g_.icon(asc_ ? icon::up : icon::down, x0 + w + 9, (hr.t + hr.b) / 2, 9, text);
        ui_.click(hr, [this, k, on] {
            if (!on) sort_ = k, asc_ = true;
            else if (asc_) asc_ = false;
            else sort_ = Sort::none, asc_ = true;
            dirty_ = true;
        });
    };
    head(L"Title", Sort::title, r.l + 16 + 52, x_bpm);
    head(L"BPM", Sort::bpm, x_bpm, x_key);
    head(L"Key", Sort::key, x_key, x_energy);
    head(L"Energy", Sort::energy, x_energy, x_dur);
    g_.line(r.l, hb, r.r, hb, hairline);

    const Rect area{r.l, hb + 1, r.r, r.b - 1};
    list_view_h_ = area.h();
    if (ids_.empty()) {
        const bool filtered = !search_text_.empty() || mix_.active();
        g_.text(filtered ? L"No tracks match." : L"Nothing here yet.", area, {Font::ui, 13, 400, text3, Align::center});
        return;
    }
    constexpr float row_h = 54, pad = 6;
    const float content = float(ids_.size()) * row_h + 2 * pad;
    const float before = list_scroll_;
    ui_.scroll_area(area, list_scroll_, content);
    if (list_scroll_ != before) InvalidateRect(hwnd_, nullptr, FALSE);
    ui_.push_clip(area);
    // Virtualised: only the rows in view are fetched and drawn.
    const size_t first = size_t(std::max(0.f, (list_scroll_ - pad) / row_h));
    const size_t last = std::min(ids_.size(), first + size_t(area.h() / row_h) + 2);
    for (size_t i = first; i < last; ++i) {
        const float y = area.t + pad + float(i) * row_h - list_scroll_;
        if (const auto tr = store_.row(ids_[i])) row(*tr, Rect{area.l + pad, y, area.r - pad - 8, y + row_h});
    }
    ui_.pop_clip();
}

void View::row(const TrackRow& tr, const Rect& r) {
    const auto f = store_.focus();
    const bool focused = f && *f == tr.id();
    if (focused) {
        g_.fill_round(r, 12, theme::selected);
        g_.stroke_round(r, 12, with_alpha(lilac, 0.6f));
    } else if (ui_.hover(r)) {
        g_.fill_round(r, 12, theme::hover);
    }
    const Rect c = r.inset(10, 6);
    const float cy = (c.t + c.b) / 2;
    const Rect cover = Rect::xywh(c.l, cy - 20, 40, 40);
    ui_.artwork(tr.track, cover, 8);
    const player::Item* now = player_.current();
    const bool current = now && now->track_id == tr.id();
    const bool playable = tr.status() == TrackStatus::downloaded;
    if (current || (playable && ui_.hover(cover))) {
        g_.fill_round(cover, 8, argb(0x99000000));
        g_.icon(current && player_.playing() ? icon::pause : icon::play, cover.l + 20, cover.t + 20, 16, current ? lilac : white);
    }

    const float right = c.r + 8;  // columns line up with the header (which has no scrollbar gutter)
    const float x_status = right - kStatus, x_dur = x_status - kDuration, x_energy = x_dur - kEnergy, x_key = x_energy - kKey,
                x_bpm = x_key - kBpm;
    g_.text(wide(tr.track.title), Rect{c.l + 52, cy - 18, x_bpm - 8, cy}, {Font::ui, 13.5f, 600, current ? lilac : text});
    g_.text(wide(tr.track.artist()), Rect{c.l + 52, cy + 1, x_bpm - 8, cy + 17}, {Font::ui, 12, 400, text2});
    ui_.bpm_readout(x_bpm, cy, tr.bpm(), tr.file && tr.file->bpm_unsure());
    ui_.key_badge(x_key, cy, tr.camelot(), tr.file && tr.file->key_unsure());
    ui_.energy_meter(x_energy, cy + 7, tr.energy());
    g_.text(wide(tr.duration_text()), Rect{x_dur, cy - 10, x_dur + kDuration, cy + 10}, {Font::dot, 12, 700, text3, Align::right});
    ui_.status_dot(right - 8, cy, tr.status());
    ui_.click(r, [this, id = tr.id()] { focus(id); }, [this, id = tr.id()] { row_menu(id); });
    if (playable) ui_.click(cover, [this, id = tr.id()] { play_track(id); }, [this, id = tr.id()] { row_menu(id); });
}

// MARK: Inspector

void View::inspector(const Rect& r, bool floating) {
    const auto id = store_.focus();
    const auto tr = id ? store_.row(*id) : std::nullopt;
    if (!tr) return;
    ui_.glass(r, 24, false, floating);
    const Rect in = r.inset(18);
    ui_.push_clip(r.inset(1));
    float y = in.t - insp_scroll_;

    // Status + close
    ui_.dot_label(tr->status() == TrackStatus::downloaded ? L"In your crate" : tr->status() == TrackStatus::ignored ? L"Ignored" : L"Missing",
                  in.l, y + 14);
    const Rect close = Rect::xywh(in.r - 30, y, 30, 28);
    if (ui_.hover(close)) g_.fill_round(close, 8, theme::hover);
    g_.icon(icon::close, (close.l + close.r) / 2, (close.t + close.b) / 2, 13, text2);
    ui_.click(close, [this] { focus(std::nullopt); });
    y += 36;

    const float art = std::min(280.f, in.w());
    ui_.artwork(tr->track, Rect::xywh((in.l + in.r - art) / 2, y, art, art), 20);
    y += art + 16;
    y += g_.paragraph(wide(tr->track.title), Rect{in.l, y, in.r, y}, {Font::ui, 22, 600, text});
    y += g_.paragraph(wide(tr->track.artist()), Rect{in.l, y, in.r, y}, {Font::ui, 15, 400, text2});
    std::wstring meta;
    for (const auto& part : {tr->track.album, tr->track.year})
        if (part) meta += (meta.empty() ? L"" : L" · ") + wide(*part);
    y += g_.paragraph(meta, Rect{in.l, y, in.r, y}, {Font::ui, 12, 400, text3}) + 14;

    // Readouts
    const auto& fa = tr->file;
    const float rw = (in.w() - 16) / 3, rh = 92;
    auto readout = [&](int i, const wchar_t* label) {
        const Rect b = Rect::xywh(in.l + float(i) * (rw + 8), y, rw, rh);
        ui_.glass(b, 16);
        ui_.dot_label(label, b.l + 12, b.t + 18, text3, 10);
        return b;
    };
    {
        const Rect b = readout(0, L"BPM");
        ui_.bpm_readout(b.l + 12, b.t + 50, tr->bpm(), fa && fa->bpm_unsure(), 28);
        if (fa && fa->bpm_alternate)
            g_.text(std::format(L"or {}", std::lround(*fa->bpm_alternate)), Rect{b.l + 12, b.t + 68, b.r, b.t + 84}, {Font::dot, 11, 700, text3});
    }
    {
        const Rect b = readout(1, L"Key");
        ui_.key_badge(b.l + 12, b.t + 50, tr->camelot(), fa && fa->key_unsure(), true);
        g_.text(fa && fa->key ? wide(*fa->key) : L"", Rect{b.l + 12, b.t + 70, b.r - 4, b.t + 86}, {Font::ui, 11, 400, text3});
    }
    {
        const Rect b = readout(2, L"Energy");
        ui_.energy_meter(b.l + 12, b.t + 62, tr->energy(), 22);
        g_.text(tr->energy() ? std::format(L"{}%", std::lround(*tr->energy() * 100)) : L"–", Rect{b.l + 12, b.t + 68, b.r, b.t + 84},
                {Font::dot, 11, 700, text3});
    }
    y += rh + 8;
    std::wstring note;
    if (!fa) note = tr->status() == TrackStatus::downloaded ? L"Not analysed yet — run Rescan & analyse." : L"Not on this device yet.";
    else {
        note = std::format(L"Full-track analysis · tempo confidence {}%", std::lround(fa->bpm_confidence.value_or(0) * 100));
        if (fa->key_agreement) note += std::format(L" · key {}/3 methods agree", *fa->key_agreement);
    }
    y += g_.paragraph(note, Rect{in.l, y, in.r, y}, {Font::ui, 11.5f, 400, text3}) + 14;

    // Playlists
    if (!tr->track.playlists.empty()) {
        ui_.dot_label(L"Playlists", in.l, y + 7);
        y += 22;
        float x = in.l;
        for (const auto& pl : tr->track.playlists) {
            const float w = ui_.chip_width(wide(pl));
            if (x + w > in.r && x > in.l) x = in.l, y += 36;
            ui_.chip(x, y, wide(pl), std::nullopt, false, false, [this, pl] { go(Page::playlist, pl); });
            x += w + 6;
        }
        y += 30 + 14;
    }

    // Mixes with (cached: it scans the whole library)
    if (mixes_for_ != tr->id()) {
        mixes_ = store_.mixes_with(*tr);
        mixes_for_ = tr->id();
    }
    const float mix_h = 14 + 24 + (tr->camelot().empty() || !tr->bpm() || mixes_.empty() ? 36.f : float(mixes_.size()) * 38) + 10;
    const Rect mb{in.l, y, in.r, y + mix_h};
    ui_.glass(mb, 18, true);
    g_.icon(icon::sparkle, mb.l + 21, mb.t + 22, 12, text);
    ui_.dot_label(L"Mixes with", mb.l + 34, mb.t + 22, text);
    float my = mb.t + 40;
    if (tr->camelot().empty() || !tr->bpm()) {
        g_.paragraph(L"Needs a key and BPM — available once the track is on this device and analysed.", Rect{mb.l + 14, my, mb.r - 14, my},
                     {Font::ui, 12, 400, text2});
    } else if (mixes_.empty()) {
        g_.paragraph(L"Nothing in your crate within ±6% tempo in a compatible key yet.", Rect{mb.l + 14, my, mb.r - 14, my}, {Font::ui, 12, 400, text2});
    } else {
        for (const auto& mx : mixes_) {
            const Rect mr{mb.l + 8, my, mb.r - 8, my + 38};
            if (ui_.hover(mr)) g_.fill_round(mr, 10, theme::hover);
            ui_.artwork(mx.track, Rect::xywh(mr.l + 6, my + 4, 30, 30), 6);
            const float kw = 52;
            g_.text(wide(mx.track.title), Rect{mr.l + 46, my, mr.r - kw - 40, my + 38}, {Font::ui, 12.5f, 600, text});
            ui_.bpm_readout(mr.r - kw - 34, my + 19, mx.bpm(), false, 13);
            ui_.key_badge(mr.r - kw, my + 19, mx.camelot());
            ui_.click(mr, [this, id = mx.id()] { focus(id); });
            my += 38;
        }
    }
    y += mix_h + 14;

    // Actions
    float x = in.l;
    auto action = [&](const std::wstring& label, const wchar_t* glyph, Ui::Pill style, std::function<void()> fn) {
        const float w = ui_.pill_width(label, glyph);
        if (x + w > in.r && x > in.l) x = in.l, y += 42;
        ui_.pill(x, y, label, glyph, style, std::move(fn));
        x += w + 8;
    };
    const bool busy = store_.busy().has_value();
    const auto path = tr->state ? tr->state->local_path : std::nullopt;
    if (tr->status() == TrackStatus::downloaded && path) {
        const player::Item* now = player_.current();
        const bool playing = now && now->track_id == tr->id() && player_.playing();
        action(playing ? L"Pause" : L"Play", playing ? icon::pause : icon::play, Ui::Pill::primary, [this, i = tr->id()] { play_track(i); });
        action(L"Write tags to file", icon::tag, Ui::Pill::smart,
               busy ? std::function<void()>{} : [this, i = tr->id()] { jobs_.run([this, i] { store_.write_tags(std::vector{i}); }); });
        action(L"Show in folder", icon::folder, Ui::Pill::glass, [this, p = *path] { show_in_folder(p); });
    }
    if (tr->status() == TrackStatus::missing)
        action(L"Ignore", icon::block, Ui::Pill::glass, [this, i = tr->id()] { jobs_.run([this, i] { store_.set_status({i}, TrackStatus::ignored); }); });
    if (tr->status() == TrackStatus::ignored)
        action(L"Un-ignore", icon::undo, Ui::Pill::glass, [this, i = tr->id()] { jobs_.run([this, i] { store_.set_status({i}, TrackStatus::missing); }); });
    if (x > in.l) y += 34;
    if (path) y += 8 + g_.paragraph(wide(*path), Rect{in.l, y + 8, in.r, y + 8}, {Font::ui, 11, 400, text3});
    y += 4;
    ui_.pop_clip();
    const float before = insp_scroll_;
    ui_.scroll_area(in, insp_scroll_, y + insp_scroll_ - in.t);
    if (insp_scroll_ != before) InvalidateRect(hwnd_, nullptr, FALSE);
}

// MARK: Menus & shell

void View::row_menu(const std::string& id) {
    const auto tr = store_.row(id);
    if (!tr) return;
    focus(id);
    HMENU m = CreatePopupMenu();
    const auto path = tr->state ? tr->state->local_path : std::nullopt;
    if (tr->status() == TrackStatus::downloaded && path) {
        AppendMenuW(m, MF_STRING, 5, L"Play");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, store_.busy() ? MF_GRAYED : MF_STRING, 1, L"Write tags to file");
        AppendMenuW(m, MF_STRING, 2, L"Show in folder");
    }
    if (tr->status() == TrackStatus::missing) AppendMenuW(m, MF_STRING, 3, L"Ignore");
    if (tr->status() == TrackStatus::ignored) AppendMenuW(m, MF_STRING, 4, L"Un-ignore");
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = GetMenuItemCount(m) ? TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr) : 0;
    DestroyMenu(m);
    switch (cmd) {
        case 1: jobs_.run([this, id] { store_.write_tags(std::vector{id}); }); break;
        case 2: show_in_folder(*path); break;
        case 3: jobs_.run([this, id] { store_.set_status({id}, TrackStatus::ignored); }); break;
        case 4: jobs_.run([this, id] { store_.set_status({id}, TrackStatus::missing); }); break;
        case 5: play_track(id); break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void View::key_menu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 100, L"Any key");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (int n = 1; n <= 12; ++n)
        for (const char l : {'A', 'B'}) AppendMenuW(m, MF_STRING, UINT_PTR(n * 2 + (l == 'B')), std::format(L"{}{}", n, wchar_t(l)).c_str());
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd == 100) mix_.key.reset();
    else if (cmd > 0) mix_.key = std::to_string(cmd / 2) + (cmd % 2 ? "B" : "A");
    else return;
    list_scroll_ = 0;
    dirty_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void View::show_in_folder(const std::string& path) {
    if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(widen(path).c_str())) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
    }
}

}  // namespace wb::ui
