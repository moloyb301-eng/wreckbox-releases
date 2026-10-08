#include "library/downloads_watcher.h"

#include <regex>
#include <vector>

#include "model/paths.h"
#include "model/settings.h"

namespace fs = std::filesystem;

namespace wb {
namespace {

std::string to_utf8(const fs::path& p) {
    const auto u = p.u8string();
    return {u.begin(), u.end()};
}

}  // namespace

std::string iso_millis(const std::string& iso) {
    static const std::regex whole_seconds(R"(^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$)");
    return std::regex_match(iso, whole_seconds) ? iso.substr(0, 19) + ".000Z" : iso;
}

DownloadsWatcher::DownloadsWatcher(LibraryStore& store, fs::path folder) : store_(store), folder_(std::move(folder)) {}

fs::path DownloadsWatcher::seen_file() const { return paths::cache() / L"organiser_seen.json"; }

void DownloadsWatcher::start(std::chrono::seconds every) {
    if (timer_.joinable()) return;
    timer_ = std::jthread([this, every](std::stop_token stop) {
        while (!stop.stop_requested()) {
            run_once();
            std::unique_lock lock(wake_m_);
            wake_.wait_for(lock, stop, every, [] { return false; });
        }
    });
}

void DownloadsWatcher::stop() {
    if (!timer_.joinable()) return;
    timer_.request_stop();
    wake_.notify_all();
    timer_.join();
}

std::deque<std::string> DownloadsWatcher::recent() const {
    std::lock_guard lock(m_);
    return recent_;
}

int DownloadsWatcher::run_once() {
    std::lock_guard run(run_m_);
    const auto lib = store_.library();
    if (folder_.empty() || !lib || store_.busy() || !Settings::current().organise_downloads) return 0;
    {
        std::lock_guard lock(m_);
        if (!loaded_) {
            if (const auto text = paths::read_file(seen_file())) {
                const json j = json::parse(*text, nullptr, false);
                if (j.is_array())
                    for (const auto& k : j)
                        if (k.is_string()) seen_.insert(k.get<std::string>());
            }
            loaded_ = true;
        }
    }
    std::error_code ec;
    if (!fs::is_directory(folder_, ec)) return 0;
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(folder_, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec))
        if (it->is_regular_file(ec) && paths::is_audio(it->path()) && it->path().filename().wstring()[0] != L'.') files.push_back(it->path());

    const std::string built = iso_millis(lib->built_at);
    int filed = 0;
    for (const auto& f : files) {
        const auto size1 = fs::file_size(f, ec);
        if (ec) continue;
        const std::string key = to_utf8(f) + "|" + std::to_string(size1);
        {
            std::lock_guard lock(m_);
            if (seen_.contains(key) || seen_.contains(key + "|" + built)) continue;
        }
        // Skip files still being written (size changing).
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        if (!fs::exists(f, ec) || fs::file_size(f, ec) != size1) continue;
        const std::string msg = store_.organise(to_utf8(f), "downloads");
        if (msg.starts_with("Added")) ++filed;
        std::lock_guard lock(m_);
        recent_.push_front(msg);
        if (recent_.size() > 50) recent_.pop_back();
        // A file that isn't in the library yet is retried after the next playlist import (the key includes the
        // library's build time); anything else is handled once.
        seen_.insert(msg.starts_with("Not in your library") ? key + "|" + built : key);
    }
    std::string text;
    {
        std::lock_guard lock(m_);
        text = json(seen_).dump();
    }
    try {
        paths::write_atomic(seen_file(), text);
    } catch (const std::exception& e) {
        store_.log("organiser", std::string("couldn't save organiser_seen.json: ") + e.what());
    }
    return filed;
}

}  // namespace wb
