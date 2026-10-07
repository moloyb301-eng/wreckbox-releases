// LibraryStore: the app's state (library, per-track status, analysis cache, activity log) and the operations on it —
// scanning, analysing, organising new files and writing tags. Port of app/lib/store.dart.
//
// Threading: one mutex guards the in-memory state and is held only for in-memory reads and writes — never during
// disk, network or analysis work — so the UI thread can always read promptly. Long operations (rescan, organise,
// write_tags) run on worker threads; rescan analyses files in parallel. `on_changed` fires (on any thread) after
// state changes; the UI turns it into a repaint.
#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "library/matcher.h"
#include "model/model.h"

namespace wb {

// "My folders" rows are files, not library tracks: their id is this prefix + the file's path.
inline constexpr const char* kFileIdPrefix = "file:";
inline bool is_file_id(const std::string& id) { return id.starts_with(kFileIdPrefix); }

struct TrackRow {
    LibraryTrack track;
    std::optional<TrackState> state;
    std::optional<FileAnalysis> file;
    std::string genre;
    json remote;  // phone: the computer's per-track summary from the account (status, bpm, key, energy)
    std::string file_id;  // "file:<path>" for a My folders row (track then keeps its library id, for the cover)

    const std::string& id() const { return file_id.empty() ? track.id : file_id; }
    std::string format() const;  // the file's type in capitals ("FLAC", "MP3"…), "" when there's no file
    bool is_flac() const { return format() == "FLAC"; }
    TrackStatus status() const { return state ? state->status : TrackStatus::missing; }
    std::optional<double> bpm() const;
    std::string camelot() const;
    std::optional<double> energy() const;
    std::string duration_text() const;
};

enum class ListFilter { all, missing, downloaded, ignored, on_pc };  // on_pc: every audio file in the library folders, except WreckBox's own

class LibraryStore {
public:
    std::function<void()> on_changed;

    // MARK: Load / save
    void load();
    void save();           // state.json (never overwrites a state file that couldn't be read)
    void save_analysis();  // _cache/analysis.json + catalogue_bpm.json
    void save_remote_crate();
    void adopt_library_json(const std::string& json_text);  // replace the library, keep this device's state

    // MARK: Status
    std::optional<std::string> load_error() const;
    std::optional<std::string> busy() const;
    void set_busy(std::optional<std::string> b);
    std::optional<std::string> focus() const;
    void set_focus(std::optional<std::string> id);
    void log(const std::string& event, const std::string& detail, std::optional<std::string> track_id = std::nullopt);
    void changed() const;

    // MARK: Queries (copies; safe to use after the call returns)
    std::shared_ptr<const Library> library() const;
    std::optional<LibraryTrack> track(const std::string& id) const;
    std::optional<TrackRow> row(const std::string& id) const;
    // Ids only — cheap enough to call on every keystroke; the UI then asks row(id) for the rows it shows.
    std::vector<std::string> row_ids(ListFilter filter = ListFilter::all, const std::optional<std::string>& playlist = std::nullopt,
                                     const std::string& search = "") const;
    std::vector<TrackRow> rows(ListFilter filter = ListFilter::all, const std::optional<std::string>& playlist = std::nullopt,
                               const std::string& search = "") const;
    size_t count(TrackStatus s) const;
    size_t downloaded_in(const LibraryPlaylist& pl) const;
    std::vector<TrackRow> mixes_with(const TrackRow& r) const;  // compatible key, tempo within ±6% (also half / double)
    std::string describe(const std::string& id) const;
    AppState state_copy() const;
    std::vector<std::string> scan_folders() const;  // cheap: just the folder list
    std::optional<FileAnalysis> analysis_of(const std::string& path) const;
    size_t analysis_count() const;
    std::vector<std::string> default_scan_folders() const;

    // MARK: Actions
    void set_status(const std::vector<std::string>& ids, TrackStatus s);
    void set_track_state(const std::string& id, TrackState s);
    void set_scan_folders(std::vector<std::string> folders);
    // Settings → library folders: one more folder to scan (also remembered in settings.json's extra folders, so the
    // defaults keep it), taking one away, or back to the defaults. Each saves; add / remove ignore case and a trailing slash.
    void add_scan_folder(const std::string& folder);
    void remove_scan_folder(const std::string& folder);
    void reset_scan_folders();
    // The Soulseek download order (keys "playlist:<name>" / "genre:<name>") and whether only those are downloaded.
    void set_download_priority(std::vector<std::string> priorities, std::optional<bool> priority_only = std::nullopt);

    // MARK: Artwork
    std::filesystem::path artwork_file(const LibraryTrack& t) const;
    std::optional<std::filesystem::path> ensure_artwork(const LibraryTrack& t);  // downloads the 640 px cover if needed

    // MARK: Work (call from worker threads)
    std::optional<FileAnalysis> analyze_file(const std::string& path, const std::optional<std::string>& track_id = std::nullopt);
    FileFacts facts(const std::string& path) const;
    void rescan();
    std::string organise(const std::string& path, const std::string& source, std::optional<std::string> track_id = std::nullopt);
    void write_tags(const std::optional<std::vector<std::string>>& ids = std::nullopt);

private:
    FileAnalysis reconcile_bpm(FileAnalysis a, const std::string& track_id);
    std::optional<TrackRow> row_locked(const std::string& id) const;
    std::optional<TrackRow> file_row_locked(const std::string& path) const;
    const LibraryTrack* track_locked(const std::string& id) const;
    std::string describe_locked(const std::string& id) const;
    void log_locked(const std::string& event, const std::string& detail, std::optional<std::string> track_id = std::nullopt);
    bool begin_busy(const std::string& label);  // false if another long operation is running

    mutable std::mutex m_;
    std::mutex save_m_;  // serialises file writes
    std::shared_ptr<const Library> library_;
    std::unordered_map<std::string, size_t> index_;
    std::vector<std::string> search_keys_;  // normalized("artist title album"), parallel to library_->tracks
    AppState state_;
    std::map<std::string, FileAnalysis> analysis_;
    std::map<std::string, double> catalogue_bpm_;  // track id → Deezer BPM (0 = none)
    json remote_crate_ = json::object();
    std::optional<std::string> load_error_, busy_, focus_;
    bool state_unreadable_ = false;
};

// Seconds-resolution ISO time of a file's last write, as the analysis cache stores it.
std::string file_modified_iso(const std::filesystem::path& p);

}  // namespace wb
