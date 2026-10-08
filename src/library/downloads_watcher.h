// Watches the Downloads folder: each new audio file that belongs
// to your library is analysed, tagged with the Spotify data, renamed and moved into Tracks\. Files that don't match
// are remembered and left alone — and retried after the next playlist import.
#pragma once
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "library/store.h"

namespace wb {

class DownloadsWatcher {
public:
    DownloadsWatcher(LibraryStore& store, std::filesystem::path folder);
    ~DownloadsWatcher() { stop(); }

    void start(std::chrono::seconds every = std::chrono::seconds(30));  // runs once now, then on a timer thread
    void stop();
    int run_once();                  // one pass; returns how many files were filed into the library
    std::deque<std::string> recent() const;  // newest first, at most 50 status lines

private:
    std::filesystem::path seen_file() const;

    LibraryStore& store_;
    std::filesystem::path folder_;
    mutable std::mutex m_;
    std::set<std::string> seen_;
    bool loaded_ = false;
    std::deque<std::string> recent_;
    std::mutex run_m_;  // one pass at a time
    std::jthread timer_;
    std::condition_variable_any wake_;
    std::mutex wake_m_;
};

// the original app's DateTime.toIso8601String() for a UTC time read from library.json ("…:05Z" → "…:05.000Z"), so seen-file keys
// written by the original build still match.
std::string iso_millis(const std::string& iso);

}  // namespace wb
