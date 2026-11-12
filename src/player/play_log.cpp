#include "player/play_log.h"

#include <cmath>
#include <fstream>

#include "model/paths.h"

namespace fs = std::filesystem;

namespace wb::player {

PlayLog::PlayLog(fs::path dir, Clock clock) : dir_(std::move(dir)), clock_(std::move(clock)) {}

PlayLog::~PlayLog() { end("closed", 0); }

bool PlayLog::counts(double listened_sec, double length_sec) {
    if (listened_sec >= 30.0) return true;
    return length_sec > 0 && listened_sec >= length_sec / 2.0 && listened_sec >= 1.0;
}

void PlayLog::settle() {
    if (!current_ || !current_->playing) return;
    const auto now = clock_();
    current_->listened += std::chrono::duration<double>(now - current_->since).count();
    current_->since = now;
}

void PlayLog::begin(const std::string& key, const std::string& location, const std::string& title, const std::string& artist) {
    end("skipped", 0);
    last_length_ = 0;
    Listen l;
    l.key = key, l.location = location, l.title = title, l.artist = artist;
    l.started_at = iso_seconds_now();
    l.playing = true;
    l.since = clock_();
    current_ = std::move(l);
}

void PlayLog::set_playing(bool playing) {
    if (!current_ || current_->playing == playing) return;
    settle();
    current_->playing = playing;
    current_->since = clock_();
}

void PlayLog::end(const char* how, double length_sec) {
    if (!current_) return;
    settle();
    const Listen l = std::move(*current_);
    current_.reset();
    if (length_sec <= 0) length_sec = last_length_;
    const double listened = std::round(l.listened * 10.0) / 10.0;
    if (listened < 1.0) return;  // clicked past: not a listen at all
    const bool counted = counts(listened, length_sec);
    try {
        std::error_code ec;
        fs::create_directories(dir_, ec);
        json line = {{"at", l.started_at}, {"id", l.key}, {"path", l.location}, {"title", l.title}, {"artist", l.artist},
                     {"listenedSec", listened}, {"end", how}, {"counted", counted}};
        line["lengthSec"] = length_sec > 0 ? json(std::round(length_sec * 10.0) / 10.0) : json(nullptr);
        {
            std::ofstream out(events_file(), std::ios::binary | std::ios::app);
            out << line.dump() << '\n';
        }
        // The totals: read, add this listen, write back whole (atomically, so a crash can't leave half a file).
        json totals = json::object();
        if (const auto text = paths::read_file(counts_file())) {
            const json j = json::parse(*text, nullptr, false);
            if (j.is_object()) totals = j;
        }
        json& t = totals[l.key];
        if (!t.is_object()) t = {{"plays", 0}, {"listenedSec", 0.0}, {"firstPlayed", l.started_at}};
        t["listenedSec"] = std::round((t.value("listenedSec", 0.0) + listened) * 10.0) / 10.0;
        if (counted) {
            t["plays"] = t.value("plays", 0) + 1;
            t["lastPlayed"] = l.started_at;
        }
        t["title"] = l.title;
        t["artist"] = l.artist;
        t["path"] = l.location;
        paths::write_atomic(counts_file(), totals.dump(1));
    } catch (...) {
        // Never worth interrupting playback over.
    }
}

}  // namespace wb::player
