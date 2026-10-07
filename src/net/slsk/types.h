// What the Soulseek client hands the matcher and the sync loop: one user's answer to a search, and the files in it.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace wb::slsk {

// Attribute keys in a search result's file entry.
constexpr uint32_t kAttrBitrate = 0, kAttrDuration = 1;

struct FileEntry {
    std::string filename;  // the peer's path, with backslashes
    uint64_t size = 0;
    std::string extension;
    std::map<uint32_t, uint32_t> attributes;  // key → value (bitrate in kbps, duration in seconds, …)
};

struct UserResult {
    std::string username;
    uint32_t ticket = 0;
    std::vector<FileEntry> files;
    bool free_slots = false;
    uint32_t avg_speed = 0;
    uint32_t queue_size = 0;
};

}  // namespace wb::slsk
