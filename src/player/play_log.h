// What was played, when, and for how long — kept in the library folder so it can be put to use later.
//
//   <library>\_stats\plays.jsonl        one line per listen, added as each one ends. Nothing is ever rewritten:
//       {"at": "2026-11-12T10:31:07Z", "id": "USRC12345678", "path": "C:\\…\\Artist - Title.flac", "title": "…",
//        "artist": "…", "listenedSec": 187.4, "lengthSec": 215.0, "end": "ended", "counted": true}
//   <library>\_stats\play_counts.json   the running totals per song, rebuilt from the same events:
//       {"USRC12345678": {"plays": 12, "listenedSec": 2240.5, "firstPlayed": "…", "lastPlayed": "…", "title": "…",
//                         "artist": "…", "path": "…"}}
//
// A song is keyed by its library id, or by "file:<path>" for a file that isn't in the library, or by its address for
// a radio stream. A listen counts as a play when at least 30 seconds were heard, or at least half of a shorter
// track — so skipping through a list doesn't inflate the numbers. Listened time is time actually playing: pauses
// don't count, and seeking back and forth doesn't either. The raw lines keep every listen, counted or not, so the
// rule can be changed later and the totals worked out again.
//
// Used from the UI thread only (like the player). A failure to write is swallowed: it must never stop the music.
#pragma once
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "model/model.h"

namespace wb::player {

class PlayLog {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    // `dir` is where the two files live (created when the first listen is written). `clock` is for tests.
    explicit PlayLog(std::filesystem::path dir, Clock clock = [] { return std::chrono::steady_clock::now(); });
    ~PlayLog();  // a listen still open is written as "closed"

    // A song starts (ending the one before as "skipped" if it's still open). `key` identifies the song.
    void begin(const std::string& key, const std::string& location, const std::string& title, const std::string& artist);
    void set_playing(bool playing);  // paused / resumed
    // The song stops: `how` is "ended", "skipped", "stopped", "error" or "closed". `length_sec` ≤ 0 when unknown.
    void end(const char* how, double length_sec);
    bool open() const { return current_.has_value(); }

    static bool counts(double listened_sec, double length_sec);

    std::filesystem::path events_file() const { return dir_ / L"plays.jsonl"; }
    std::filesystem::path counts_file() const { return dir_ / L"play_counts.json"; }

private:
    struct Listen {
        std::string key, location, title, artist, started_at;
        double listened = 0;  // seconds, up to the last pause
        bool playing = false;
        std::chrono::steady_clock::time_point since;  // when it last started playing
    };
    void settle();  // moves the time since `since` into `listened`

    std::filesystem::path dir_;
    Clock clock_;
    std::optional<Listen> current_;
    double last_length_ = 0;  // the length last reported, for a listen that ends without one
public:
    void note_length(double length_sec) {
        if (length_sec > 0) last_length_ = length_sec;
    }
};

}  // namespace wb::player
