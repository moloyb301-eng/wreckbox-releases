#include "library/store.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <future>
#include <regex>
#include <set>
#include <thread>

#include "engine/engine.h"
#include "model/paths.h"
#include "model/settings.h"
#include "net/http_client.h"

namespace fs = std::filesystem;

namespace wb {
namespace {

fs::path to_path(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }
std::string to_utf8(const fs::path& p) {
    const auto u = p.u8string();
    return {u.begin(), u.end()};
}
std::string basename(const std::string& utf8) { return to_utf8(to_path(utf8).filename()); }
bool exists(const std::string& utf8) {
    std::error_code ec;
    return fs::is_regular_file(to_path(utf8), ec);
}
// A path for comparing: normalised, forward slashes, lower case (Windows paths ignore case).
std::wstring folded(const std::string& p) {
    std::wstring w = to_path(p).lexically_normal().generic_wstring();
    CharLowerBuffW(w.data(), DWORD(w.size()));
    return w;
}

// Analysis-cache timestamps are compared at whole-second resolution, like isoSeconds() in the original app.
std::string iso_trim(const std::string& s) { return s.size() >= 19 ? s.substr(0, 19) + "Z" : s; }

std::optional<double> num(const json& j, const char* k) {
    if (!j.is_object()) return std::nullopt;
    const auto it = j.find(k);
    return it != j.end() && it->is_number() ? std::optional(it->get<double>()) : std::nullopt;
}

// rename(), falling back to copy + delete across volumes (e.g. another drive → Music).
void move_file(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec) return;
    fs::copy_file(from, to, fs::copy_options::none, ec);
    if (ec) throw std::runtime_error(ec.message());
    fs::remove(from, ec);
}

}  // namespace

std::string file_modified_iso(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return "";
    const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
    return iso_seconds(std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count());
}

// MARK: TrackRow

std::optional<double> TrackRow::bpm() const { return file && file->bpm ? file->bpm : num(remote, "bpm"); }
std::string TrackRow::camelot() const {
    if (file && file->camelot) return *file->camelot;
    if (remote.is_object() && remote.contains("camelot") && remote["camelot"].is_string()) return remote["camelot"].get<std::string>();
    return "";
}
std::optional<double> TrackRow::energy() const { return file && file->energy ? file->energy : num(remote, "energy"); }
std::string TrackRow::duration_text() const {
    if (!track.duration_ms) return "";
    const int64_t ms = *track.duration_ms;
    return std::format("{}:{:02}", ms / 60000, ms / 1000 % 60);
}
std::string TrackRow::format() const {
    if (!state || !state->local_path) return "";
    std::string ext = to_utf8(to_path(*state->local_path).extension());
    if (!ext.empty()) ext.erase(0, 1);
    for (auto& c : ext) c = char(std::toupper(static_cast<unsigned char>(c)));
    return ext;
}

// MARK: Load / save

void LibraryStore::load() {
    // The three big files parse in parallel (each is a few MB for a 5,000-track library).
    auto lib_job = std::async(std::launch::async, []() -> std::pair<std::shared_ptr<Library>, std::vector<std::string>> {
        const auto text = paths::read_file(paths::library_file());
        if (!text) return {};
        const json j = json::parse(*text, nullptr, false);
        if (j.is_discarded()) return {nullptr, {"bad"}};
        auto lib = std::make_shared<Library>(Library::from_json(j));
        std::vector<std::string> keys;
        keys.reserve(lib->tracks.size());
        for (const auto& t : lib->tracks) keys.push_back(normalized(t.artist() + " " + t.title + " " + t.album.value_or("")));
        return {lib, std::move(keys)};
    });
    auto state_job = std::async(std::launch::async, []() -> std::optional<AppState> {  // nullopt = unreadable
        const auto text = paths::read_file(paths::state_file());
        if (!text) return AppState{};
        const json j = json::parse(*text, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return std::nullopt;
        return AppState::from_json(j);
    });
    auto analysis_job = std::async(std::launch::async, [] {
        std::map<std::string, FileAnalysis> analysis;
        if (const auto text = paths::read_file(paths::analysis_cache())) {
            const json j = json::parse(*text, nullptr, false);
            if (j.is_object())
                for (const auto& [k, v] : j.items()) analysis[k] = FileAnalysis::from_json(v);
        }
        return analysis;
    });

    std::optional<std::string> error;
    auto [lib, search_keys] = lib_job.get();
    if (!lib && !search_keys.empty()) {
        error = "Couldn't read library.json (not valid JSON)";
        search_keys.clear();
    }
    auto loaded_state = state_job.get();
    const bool unreadable = !loaded_state;
    // Never overwrite a state file we couldn't read.
    if (unreadable) error = "Couldn't read state.json — changes won't be saved until it's fixed";
    AppState state = loaded_state ? std::move(*loaded_state) : AppState{};
    auto analysis = analysis_job.get();
    std::map<std::string, double> catalogue;
    if (const auto text = paths::read_file(paths::cache() / L"catalogue_bpm.json")) {
        const json j = json::parse(*text, nullptr, false);
        if (j.is_object())
            for (const auto& [k, v] : j.items())
                if (v.is_number()) catalogue[k] = v.get<double>();
    }
    json remote = json::object();
    if (const auto text = paths::read_file(paths::cache() / L"remote_crate.json")) {
        const json j = json::parse(*text, nullptr, false);
        if (j.is_object()) remote = j;
    }
    {
        std::lock_guard lock(m_);
        library_ = lib;
        search_keys_ = std::move(search_keys);
        index_.clear();
        if (lib)
            for (size_t i = 0; i < lib->tracks.size(); ++i) index_[lib->tracks[i].id] = i;
        state_ = std::move(state);
        state_unreadable_ = unreadable;
        analysis_ = std::move(analysis);
        catalogue_bpm_ = std::move(catalogue);
        remote_crate_ = std::move(remote);
        load_error_ = error;
    }
    if (state_copy().scan_folders.empty()) {
        const auto folders = default_scan_folders();
        std::lock_guard lock(m_);
        state_.scan_folders = folders;
    }
    changed();
}

void LibraryStore::save() {
    std::string text;
    {
        std::lock_guard lock(m_);
        if (state_unreadable_) return;
        text = state_.to_json().dump(2);
    }
    std::lock_guard lock(save_m_);
    paths::write_atomic(paths::state_file(), text);
}

void LibraryStore::save_analysis() {
    std::string analysis, catalogue;
    {
        std::lock_guard lock(m_);
        json a = json::object();
        for (const auto& [k, v] : analysis_) a[k] = v.to_json();
        analysis = a.dump();
        catalogue = json(catalogue_bpm_).dump();
    }
    std::lock_guard lock(save_m_);
    paths::write_atomic(paths::analysis_cache(), analysis);
    paths::write_atomic(paths::cache() / L"catalogue_bpm.json", catalogue);
}

void LibraryStore::save_remote_crate() {
    std::string text;
    {
        std::lock_guard lock(m_);
        text = remote_crate_.dump();
    }
    std::lock_guard lock(save_m_);
    paths::write_atomic(paths::cache() / L"remote_crate.json", text);
}

void LibraryStore::adopt_library_json(const std::string& json_text) {
    {
        std::lock_guard lock(save_m_);
        paths::write_atomic(paths::library_file(), json_text);
    }
    load();
}

// MARK: Status

std::optional<std::string> LibraryStore::load_error() const {
    std::lock_guard lock(m_);
    return load_error_;
}

std::optional<std::string> LibraryStore::busy() const {
    std::lock_guard lock(m_);
    return busy_;
}

void LibraryStore::set_busy(std::optional<std::string> b) {
    {
        std::lock_guard lock(m_);
        busy_ = std::move(b);
    }
    changed();
}

bool LibraryStore::begin_busy(const std::string& label) {
    {
        std::lock_guard lock(m_);
        if (busy_ || !library_) return false;
        busy_ = label;
    }
    changed();
    return true;
}

std::optional<std::string> LibraryStore::focus() const {
    std::lock_guard lock(m_);
    return focus_;
}

void LibraryStore::set_focus(std::optional<std::string> id) {
    {
        std::lock_guard lock(m_);
        focus_ = std::move(id);
    }
    changed();
}

void LibraryStore::log(const std::string& event, const std::string& detail, std::optional<std::string> track_id) {
    std::lock_guard lock(m_);
    log_locked(event, detail, std::move(track_id));
}

void LibraryStore::log_locked(const std::string& event, const std::string& detail, std::optional<std::string> track_id) {
    state_.log.push_back(LogEntry::make(event, detail, std::move(track_id)));
}

void LibraryStore::changed() const {
    if (on_changed) on_changed();
}

// MARK: Queries

std::shared_ptr<const Library> LibraryStore::library() const {
    std::lock_guard lock(m_);
    return library_;
}

const LibraryTrack* LibraryStore::track_locked(const std::string& id) const {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : &library_->tracks[it->second];
}

std::optional<LibraryTrack> LibraryStore::track(const std::string& id) const {
    std::lock_guard lock(m_);
    const auto* t = track_locked(id);
    return t ? std::optional(*t) : std::nullopt;
}

std::optional<TrackRow> LibraryStore::row_locked(const std::string& id) const {
    if (is_file_id(id)) return file_row_locked(id.substr(std::string_view(kFileIdPrefix).size()));
    const auto* t = track_locked(id);
    if (!t) return std::nullopt;
    TrackRow r{*t};
    if (const auto it = state_.tracks.find(id); it != state_.tracks.end()) {
        r.state = it->second;
        if (it->second.local_path)
            if (const auto a = analysis_.find(*it->second.local_path); a != analysis_.end()) r.file = a->second;
    }
    if (const auto g = state_.genre_overrides.find(id); g != state_.genre_overrides.end()) r.genre = g->second;
    if (const auto rc = remote_crate_.find(id); rc != remote_crate_.end()) r.remote = *rc;
    return r;
}

// A file found by a scan, as a row: the library track it matched (names, cover, playlists), else its own tags or name.
std::optional<TrackRow> LibraryStore::file_row_locked(const std::string& path) const {
    const auto a = analysis_.find(path);
    if (a == analysis_.end()) return std::nullopt;
    const FileAnalysis& f = a->second;
    TrackRow r;
    if (const auto* t = f.library_track_id ? track_locked(*f.library_track_id) : nullptr) {
        r.track = *t;
        if (const auto g = state_.genre_overrides.find(t->id); g != state_.genre_overrides.end()) r.genre = g->second;
    } else {
        const std::string stem = to_utf8(to_path(path).stem());
        r.track.id = std::string(kFileIdPrefix) + path;
        r.track.title = f.title && !f.title->empty() ? *f.title : stem;
        if (f.artist && !f.artist->empty()) r.track.artists = {*f.artist};
        if (f.duration_sec) r.track.duration_ms = int64_t(*f.duration_sec * 1000);
        r.track.file_name = stem;
    }
    r.file_id = std::string(kFileIdPrefix) + path;
    r.state = TrackState{};
    r.state->status = TrackStatus::downloaded;
    r.state->local_path = path;
    r.file = f;
    return r;
}

std::optional<TrackRow> LibraryStore::row(const std::string& id) const {
    std::lock_guard lock(m_);
    return row_locked(id);
}

std::vector<std::string> LibraryStore::row_ids(ListFilter filter, const std::optional<std::string>& playlist, const std::string& search) const {
    const std::string q = normalized(search);  // outside the lock: the only per-call text work
    std::lock_guard lock(m_);
    std::vector<std::string> out;
    if (filter == ListFilter::on_pc) {  // every analysed file; searched by its tags, its name and the track it matched
        // Only files inside the library folders, and never WreckBox's own folder: that's its downloads (Tracks), and ~/Music
        // contains it. The organiser also analyses files it leaves outside the folders.
        std::vector<std::wstring> roots;
        for (const auto& d : state_.scan_folders) {
            auto root = folded(d);
            if (!root.empty() && root.back() != L'/') root += L'/';
            roots.push_back(std::move(root));
        }
        const auto own = folded(to_utf8(paths::root())) + L'/';
        for (const auto& [path, f] : analysis_) {
            const auto fp = folded(path);
            if (fp.starts_with(own)) continue;
            if (std::none_of(roots.begin(), roots.end(), [&](const std::wstring& root) { return fp.starts_with(root); })) continue;
            if (!q.empty()) {
                const auto* t = f.library_track_id && library_ ? track_locked(*f.library_track_id) : nullptr;
                const std::string hay = normalized(f.artist.value_or("") + " " + f.title.value_or("") + " " + to_utf8(to_path(path).stem()) +
                                                   (t ? " " + t->artist() + " " + t->title + " " + t->album.value_or("") : ""));
                if (hay.find(q) == std::string::npos) continue;
            }
            out.push_back(std::string(kFileIdPrefix) + path);
        }
        return out;
    }
    if (!library_) return out;
    auto keep = [&](size_t i) {
        const auto& id = library_->tracks[i].id;
        if (filter != ListFilter::all) {
            const auto st = state_.tracks.find(id);
            const TrackStatus s = st == state_.tracks.end() ? TrackStatus::missing : st->second.status;
            if ((filter == ListFilter::missing && s != TrackStatus::missing) || (filter == ListFilter::downloaded && s != TrackStatus::downloaded) ||
                (filter == ListFilter::ignored && s != TrackStatus::ignored))
                return false;
        }
        return q.empty() || search_keys_[i].find(q) != std::string::npos;
    };
    if (playlist) {
        const auto pl = std::find_if(library_->playlists.begin(), library_->playlists.end(), [&](const auto& p) { return p.name == *playlist; });
        if (pl == library_->playlists.end()) return out;
        std::set<std::string> seen;
        for (const auto& id : pl->track_ids)
            if (const auto it = index_.find(id); it != index_.end() && seen.insert(id).second && keep(it->second)) out.push_back(id);
    } else {
        for (size_t i = 0; i < library_->tracks.size(); ++i)
            if (keep(i)) out.push_back(library_->tracks[i].id);
    }
    return out;
}

std::vector<TrackRow> LibraryStore::rows(ListFilter filter, const std::optional<std::string>& playlist, const std::string& search) const {
    const auto ids = row_ids(filter, playlist, search);
    std::lock_guard lock(m_);
    std::vector<TrackRow> out;
    out.reserve(ids.size());
    for (const auto& id : ids)
        if (auto r = row_locked(id)) out.push_back(std::move(*r));
    return out;
}

size_t LibraryStore::count(TrackStatus s) const {
    std::lock_guard lock(m_);
    if (!library_) return 0;
    return size_t(std::count_if(library_->tracks.begin(), library_->tracks.end(), [&](const LibraryTrack& t) {
        const auto it = state_.tracks.find(t.id);
        return (it == state_.tracks.end() ? TrackStatus::missing : it->second.status) == s;
    }));
}

size_t LibraryStore::downloaded_in(const LibraryPlaylist& pl) const {
    std::lock_guard lock(m_);
    return size_t(std::count_if(pl.track_ids.begin(), pl.track_ids.end(), [&](const std::string& id) {
        const auto it = state_.tracks.find(id);
        return it != state_.tracks.end() && it->second.status == TrackStatus::downloaded;
    }));
}

std::vector<TrackRow> LibraryStore::mixes_with(const TrackRow& r) const {
    const auto bpm = r.bpm();
    const std::string key = r.camelot();
    if (!bpm || key.empty()) return {};
    const auto keys = compatible_keys(key);
    std::vector<std::pair<TrackRow, double>> out;
    {
        std::lock_guard lock(m_);
        if (!library_) return {};
        for (const auto& t : library_->tracks) {
            if (t.id == r.id()) continue;
            auto o = *row_locked(t.id);
            const auto ob = o.bpm();
            if (!ob || !keys.contains(o.camelot())) continue;
            const double gap = std::min({std::abs(*ob - *bpm), std::abs(*ob * 2 - *bpm), std::abs(*ob / 2 - *bpm)}) / *bpm;
            if (gap <= 0.06) {
                const double score = gap + (o.camelot() == key ? 0 : 0.01);
                out.emplace_back(std::move(o), score);
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    std::vector<TrackRow> best;
    for (size_t i = 0; i < out.size() && i < 12; ++i) best.push_back(std::move(out[i].first));
    return best;
}

std::string LibraryStore::describe_locked(const std::string& id) const {
    if (is_file_id(id)) return basename(id.substr(std::string_view(kFileIdPrefix).size()));
    const auto* t = track_locked(id);
    return t ? t->artist() + " – " + t->title : id;
}

std::string LibraryStore::describe(const std::string& id) const {
    std::lock_guard lock(m_);
    return describe_locked(id);
}

AppState LibraryStore::state_copy() const {
    std::lock_guard lock(m_);
    return state_;
}

std::optional<FileAnalysis> LibraryStore::analysis_of(const std::string& path) const {
    std::lock_guard lock(m_);
    const auto it = analysis_.find(path);
    return it == analysis_.end() ? std::nullopt : std::optional(it->second);
}

size_t LibraryStore::analysis_count() const {
    std::lock_guard lock(m_);
    return analysis_.size();
}

std::vector<std::string> LibraryStore::default_scan_folders() const {
    std::vector<std::string> out{to_utf8(paths::tracks())};
    if (paths::downloads()) out.push_back(to_utf8(*paths::downloads()));
    out.push_back(to_utf8(paths::root().parent_path()));  // ~/Music
    for (const auto& f : Settings::current().extra_scan_folders) out.push_back(f);
    return out;
}

// MARK: Actions

void LibraryStore::set_status(const std::vector<std::string>& ids, TrackStatus s) {
    {
        std::lock_guard lock(m_);
        for (const auto& id : ids) {
            const auto old = state_.tracks.find(id);
            TrackState ts;
            ts.status = s;
            if (s == TrackStatus::downloaded && old != state_.tracks.end()) ts.local_path = old->second.local_path;
            ts.source = "manual";
            ts.updated_at = iso_seconds_now();
            state_.tracks[id] = ts;
            log_locked(std::string("marked ") + to_string(s), describe_locked(id), id);
        }
    }
    save();
    changed();
}

void LibraryStore::set_track_state(const std::string& id, TrackState s) {
    std::lock_guard lock(m_);
    if (s.updated_at.empty()) s.updated_at = iso_seconds_now();
    state_.tracks[id] = std::move(s);
}

void LibraryStore::set_download_priority(std::vector<std::string> priorities, std::optional<bool> priority_only) {
    {
        std::lock_guard lock(m_);
        state_.download_priority = std::move(priorities);
        if (priority_only) state_.priority_only = *priority_only;
        std::string detail;
        for (const auto& k : state_.download_priority) detail += (detail.empty() ? "" : " → ") + k.substr(k.find(':') + 1);
        log_locked("queue", detail.empty() ? "priorities cleared" : "priorities: " + detail);
    }
    save();
    changed();
}

std::vector<std::string> LibraryStore::scan_folders() const {
    std::lock_guard lock(m_);
    return state_.scan_folders;
}

void LibraryStore::set_scan_folders(std::vector<std::string> folders) {
    {
        std::lock_guard lock(m_);
        state_.scan_folders = std::move(folders);
    }
    save();
    changed();
}

namespace {

std::wstring folder_key(const std::string& folder) {
    std::wstring w = to_path(folder).wstring();
    while (w.size() > 3 && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
    std::transform(w.begin(), w.end(), w.begin(), ::towlower);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    return w;
}

void save_settings_quietly() {
    try {
        Settings::current().save();
    } catch (const std::exception&) {
        // The folder list is still in state.json; settings.json is written on the next save.
    }
}

}  // namespace

void LibraryStore::add_scan_folder(const std::string& folder) {
    if (folder.empty()) return;
    {
        std::lock_guard lock(m_);
        const auto key = folder_key(folder);
        if (std::none_of(state_.scan_folders.begin(), state_.scan_folders.end(), [&](const std::string& f) { return folder_key(f) == key; }))
            state_.scan_folders.push_back(folder);
        auto& extra = Settings::current().extra_scan_folders;
        if (std::none_of(extra.begin(), extra.end(), [&](const std::string& f) { return folder_key(f) == key; })) extra.push_back(folder);
    }
    save_settings_quietly();
    save();
    changed();
}

void LibraryStore::remove_scan_folder(const std::string& folder) {
    {
        std::lock_guard lock(m_);
        const auto key = folder_key(folder);
        const auto drop = [&](std::vector<std::string>& v) {
            v.erase(std::remove_if(v.begin(), v.end(), [&](const std::string& f) { return folder_key(f) == key; }), v.end());
        };
        drop(state_.scan_folders);
        drop(Settings::current().extra_scan_folders);
    }
    save_settings_quietly();
    save();
    changed();
}

void LibraryStore::reset_scan_folders() {
    Settings::current().extra_scan_folders.clear();
    save_settings_quietly();
    set_scan_folders(default_scan_folders());
}

// MARK: Artwork

fs::path LibraryStore::artwork_file(const LibraryTrack& t) const {
    std::string name = t.id;
    for (auto& c : name)
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    return paths::artwork() / to_path(name + ".jpg");
}

std::optional<fs::path> LibraryStore::ensure_artwork(const LibraryTrack& t) {
    const fs::path f = artwork_file(t);
    std::error_code ec;
    if (fs::exists(f, ec)) return f;
    if (!t.artwork_url) return std::nullopt;
    std::string url = *t.artwork_url;
    if (const auto at = url.find("ab67616d00001e02"); at != std::string::npos) url.replace(at, 16, "ab67616d0000b273");  // 640 px
    const auto res = http::get(url, {}, std::chrono::seconds(20));
    if (res.status != 200) return std::nullopt;
    try {
        paths::write_atomic(f, res.body);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return f;
}

// MARK: Analysis

std::optional<FileAnalysis> LibraryStore::analyze_file(const std::string& path, const std::optional<std::string>& track_id) {
    const fs::path p = to_path(path);
    std::error_code ec;
    const auto size = int64_t(fs::file_size(p, ec));
    if (ec) return std::nullopt;
    const std::string modified = file_modified_iso(p);
    {
        std::lock_guard lock(m_);
        if (const auto it = analysis_.find(path);
            it != analysis_.end() && it->second.size_bytes == size && it->second.modified && iso_trim(*it->second.modified) == modified)
            return it->second;
    }
    const json r = wb::analyze_file(p);  // the engine, not this member
    if (r.contains("error")) {
        log("analysis failed", basename(path) + ": " + r["error"].get<std::string>(), track_id);
        return std::nullopt;
    }
    FileAnalysis a = FileAnalysis::from_engine(r, path, size, modified, track_id);
    if (track_id) a = reconcile_bpm(std::move(a), *track_id);
    std::lock_guard lock(m_);
    analysis_[path] = a;
    return a;
}

// Uses Deezer's catalogue BPM (looked up by ISRC) to pick between the analyser's top tempos.
FileAnalysis LibraryStore::reconcile_bpm(FileAnalysis a, const std::string& track_id) {
    std::optional<std::string> isrc;
    std::optional<double> ref;
    {
        std::lock_guard lock(m_);
        if (const auto* t = track_locked(track_id)) isrc = t->isrc;
        if (const auto it = catalogue_bpm_.find(track_id); it != catalogue_bpm_.end()) ref = it->second;
    }
    if (!isrc || !a.bpm) return a;
    if (!ref) {
        const auto res = http::get("https://api.deezer.com/track/isrc:" + http::url_encode(*isrc), {}, std::chrono::seconds(10));
        if (res.status == 0) return a;  // offline: keep the analyser's choice, try again next time
        const json j = json::parse(res.body, nullptr, false);
        if (j.is_discarded()) return a;
        ref = num(j, "bpm").value_or(0);
        std::lock_guard lock(m_);
        catalogue_bpm_[track_id] = *ref;
    }
    if (*ref <= 0) return a;
    auto close_to = [&](double x) { return std::abs(x - *ref) / *ref < 0.02; };
    if (close_to(*a.bpm)) return a;
    std::vector<double> others;
    if (a.bpm_alternate) others.push_back(*a.bpm_alternate);
    others.insert(others.end(), a.bpm_candidates.begin(), a.bpm_candidates.end());
    for (const double c : others) {
        if (close_to(c) || close_to(c * 2) || close_to(c / 2)) {
            a.bpm = close_to(c) ? c : (close_to(c * 2) ? c * 2 : c / 2);
            return a;
        }
    }
    return a;
}

FileFacts LibraryStore::facts(const std::string& path) const {
    const json tags = read_tags_json(to_path(path));
    FileFacts f{path};
    if (tags.contains("title") && tags["title"].is_string()) f.title = tags["title"].get<std::string>();
    if (tags.contains("isrc") && tags["isrc"].is_string()) f.isrc = tags["isrc"].get<std::string>();
    if (tags.contains("artists") && tags["artists"].is_array())
        for (const auto& a : tags["artists"])
            if (a.is_string()) f.artists.push_back(a.get<std::string>());
    if (const auto a = analysis_of(path)) f.duration_sec = a->duration_sec;
    return f;
}

// MARK: Rescan

std::string LibraryStore::delete_file(const std::string& path, bool recycle, bool skip) {
    // A file in your own folders, or a song in WreckBox's Tracks folder; never WreckBox's own data (library, caches).
    const auto own = folded(to_utf8(paths::root())) + L'/';
    if (folded(path).starts_with(own) && !folded(path).starts_with(folded(to_utf8(paths::tracks())) + L'/'))
        return "WreckBox's own files can't be deleted here.";
    if (!exists(path)) return "The file is already gone.";
    std::wstring from = to_path(path).wstring();
    from.push_back(L'\0');  // SHFileOperation takes a list that ends in two nulls
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | (recycle ? FOF_ALLOWUNDO : 0);
    if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted || exists(path))
        return "Windows couldn't delete it. Is it open in another app?";
    {
        std::lock_guard lock(m_);
        analysis_.erase(path);
        for (auto& [id, s] : state_.tracks)
            if (s.status == TrackStatus::downloaded && s.local_path == path) {  // the file of a library track: missing again (or skipped)
                TrackState m;
                m.status = skip ? TrackStatus::ignored : TrackStatus::missing;
                m.source = "deleted";
                m.updated_at = iso_seconds_now();
                s = m;
            }
        log_locked("deleted", basename(path) + (recycle ? " moved to the Recycle Bin" : " deleted") + " – " + path);
    }
    save_analysis();
    save();
    changed();
    return "";
}

void LibraryStore::rescan() {
    if (!begin_busy("Scanning folders…")) return;
    const auto lib = library();
    const auto folders = state_copy().scan_folders;
    const TrackMatcher matcher(lib->tracks);

    std::vector<std::string> files;
    std::set<std::string> seen;
    for (const auto& dir : folders) {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(to_path(dir), fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (!it->is_regular_file(ec) || !paths::is_audio(it->path())) continue;
            const bool hidden = std::any_of(it->path().begin(), it->path().end(), [](const fs::path& s) {
                const auto w = s.wstring();
                return (!w.empty() && w[0] == L'.') || w == L"_inbox";
            });
            if (hidden) continue;
            const std::string f = to_utf8(it->path());
            if (seen.insert(f).second) files.push_back(f);
        }
    }

    // Analyse in parallel (one worker per core, leaving one for the UI); results are applied in file order below so
    // the outcome doesn't depend on timing.
    std::vector<std::optional<std::string>> matched(files.size());
    std::atomic<size_t> next{0}, done{0};
    const unsigned workers = std::max(1u, std::thread::hardware_concurrency() - 1);
    {
        std::vector<std::jthread> pool;
        for (unsigned w = 0; w < workers; ++w)
            pool.emplace_back([&] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                for (size_t i; (i = next++) < files.size();) {
                    const auto idx = matcher.match(facts(files[i]));
                    if (idx) matched[i] = lib->tracks[*idx].id;
                    if (auto a = analyze_file(files[i], matched[i]); a && matched[i] && a->library_track_id != matched[i]) {
                        a->library_track_id = matched[i];
                        std::lock_guard lock(m_);
                        analysis_[files[i]] = *a;
                    }
                    const size_t n = ++done;
                    set_busy(std::format("Scanning & analysing {}/{}…", n, files.size()));
                    if (n % 20 == 0) save_analysis();
                }
            });
    }

    std::map<std::string, std::string> found;  // track id → first file (in scan order) that matched it
    for (size_t i = 0; i < files.size(); ++i)
        if (matched[i]) found.emplace(*matched[i], files[i]);

    size_t added = 0, gone = 0;
    {
        std::lock_guard lock(m_);
        std::erase_if(analysis_, [](const auto& kv) { return !exists(kv.first); });
        for (const auto& t : lib->tracks) {
            const auto cur = state_.tracks.find(t.id);
            const bool has = cur != state_.tracks.end();
            if (const auto f = found.find(t.id); f != found.end()) {
                if (has && cur->second.status == TrackStatus::ignored) continue;
                if (!has || cur->second.status != TrackStatus::downloaded || cur->second.local_path != f->second) {
                    TrackState s;
                    s.status = TrackStatus::downloaded;
                    s.local_path = f->second;
                    s.source = has && cur->second.source ? *cur->second.source : "scan";
                    s.updated_at = iso_seconds_now();
                    state_.tracks[t.id] = s;
                    log_locked("found", describe_locked(t.id) + " → " + f->second, t.id);
                    ++added;
                }
            } else if (has && cur->second.status == TrackStatus::downloaded && cur->second.local_path && !exists(*cur->second.local_path)) {
                const std::string old = *cur->second.local_path;
                TrackState s;
                s.status = TrackStatus::missing;
                s.source = "scan";
                s.updated_at = iso_seconds_now();
                state_.tracks[t.id] = s;
                log_locked("file gone", describe_locked(t.id) + " – " + old + " no longer exists", t.id);
                ++gone;
            }
        }
        log_locked("rescan", std::format("{} audio files read, {} newly matched, {} gone", files.size(), added, gone));
    }
    save_analysis();
    save();
    set_busy(std::nullopt);
}

// MARK: Organising new files

std::string LibraryStore::organise(const std::string& path, const std::string& source, std::optional<std::string> track_id) {
    const auto lib = library();
    if (!lib) return "no library loaded";
    const std::string name = basename(path);
    try {
        if (!track_id) {
            analyze_file(path);  // duration helps matching
            if (const auto idx = TrackMatcher(lib->tracks).match(facts(path))) track_id = lib->tracks[*idx].id;
        }
        if (!track_id) {
            log("unmatched", name + " — not in your Spotify library, left where it is");
            save();
            return "Not in your library: " + name;
        }
        const auto t = track(*track_id);
        if (!t) throw std::runtime_error("unknown track " + *track_id);
        const auto a = analyze_file(path, track_id);
        const auto cover = ensure_artwork(*t);
        std::optional<std::string> genre;
        {
            std::lock_guard lock(m_);
            if (const auto g = state_.genre_overrides.find(*track_id); g != state_.genre_overrides.end()) genre = g->second;
        }
        json job = {{"path", path}, {"title", t->title}, {"artists", t->artists}};
        if (t->album) job["album"] = *t->album;
        if (t->year) job["year"] = *t->year;
        if (genre) job["genre"] = *genre;
        if (a && a->bpm) job["bpm"] = *a->bpm;
        if (a && a->key) job["key"] = *a->key;
        if (t->isrc) job["isrc"] = *t->isrc;
        if (cover) job["cover"] = to_utf8(*cover);
        if (const json res = write_tags_json(job); res.contains("error")) log("tag failed", name + ": " + res["error"].get<std::string>(), track_id);

        std::error_code ec;
        fs::create_directories(paths::tracks(), ec);
        std::wstring ext = to_path(path).extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        const fs::path src = to_path(path);
        fs::path dest = paths::tracks() / (to_path(t->file_name).wstring() + ext);
        for (int n = 2; fs::exists(dest, ec) && dest != src; ++n) dest = paths::tracks() / std::format(L"{} ({}){}", to_path(t->file_name).wstring(), n, ext);
        if (dest != src) move_file(src, dest);
        const std::string dest_utf8 = to_utf8(dest);
        {
            std::lock_guard lock(m_);
            if (const auto moved = analysis_.find(path); moved != analysis_.end()) {
                FileAnalysis fa = moved->second;
                analysis_.erase(moved);
                fa.path = dest_utf8;
                fa.size_bytes = int64_t(fs::file_size(dest, ec));
                fa.modified = file_modified_iso(dest);
                fa.library_track_id = track_id;
                analysis_[dest_utf8] = fa;
            }
            TrackState s;
            s.status = TrackStatus::downloaded;
            s.local_path = dest_utf8;
            s.source = source;
            s.updated_at = iso_seconds_now();
            state_.tracks[*track_id] = s;
            log_locked("downloaded", describe_locked(*track_id) + " via " + source + " → Tracks/" + to_utf8(dest.filename()), track_id);
        }
        save_analysis();
        save();
        changed();
        return "Added " + describe(*track_id);
    } catch (const std::exception& e) {
        log("organise failed", name + ": " + e.what());
        save();
        return "Failed: " + name + " (" + e.what() + ")";
    }
}

// MARK: Tags

void LibraryStore::write_tags(const std::optional<std::vector<std::string>>& ids) {
    if (!begin_busy("Writing tags…")) return;
    std::vector<std::string> targets;
    if (ids) targets = *ids;
    else {
        std::lock_guard lock(m_);
        for (const auto& t : library_->tracks)
            if (const auto s = state_.tracks.find(t.id); s != state_.tracks.end() && s->second.status == TrackStatus::downloaded)
                targets.push_back(t.id);
    }
    size_t ok = 0, failed = 0;
    for (size_t i = 0; i < targets.size(); ++i) {
        set_busy(std::format("Writing tags {}/{}…", i + 1, targets.size()));
        const auto r = row(targets[i]);
        if (!r || !r->state || !r->state->local_path || !exists(*r->state->local_path)) continue;
        const std::string path = *r->state->local_path;
        const auto cover = ensure_artwork(r->track);
        json job = {{"path", path}, {"title", r->track.title}, {"artists", r->track.artists}};
        if (r->track.album) job["album"] = *r->track.album;
        if (r->track.year) job["year"] = *r->track.year;
        if (!r->genre.empty()) job["genre"] = r->genre;
        if (const auto b = r->bpm()) job["bpm"] = *b;
        if (r->file && r->file->key) job["key"] = *r->file->key;
        if (r->track.isrc) job["isrc"] = *r->track.isrc;
        if (cover) job["cover"] = to_utf8(*cover);
        if (const json res = write_tags_json(job); res.contains("error")) {
            ++failed;
            log("tag failed", basename(path) + ": " + res["error"].get<std::string>(), r->id());
        } else {
            ++ok;
            std::error_code ec;
            std::lock_guard lock(m_);
            if (const auto a = analysis_.find(path); a != analysis_.end()) {
                a->second.size_bytes = int64_t(fs::file_size(to_path(path), ec));
                a->second.modified = file_modified_iso(to_path(path));
            }
        }
    }
    log("tags written", std::format("{} files updated, {} failed", ok, failed));
    save_analysis();
    save();
    set_busy(std::nullopt);
}

}  // namespace wb
