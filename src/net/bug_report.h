// "Report a bug" (port of BugReport in app/lib/services.dart): title, description, up to 3 images, who is reporting and the
// last 40 log entries go to the bug-report relay, which files a GitHub issue. Blocking: a worker calls send().
#pragma once
#include <stdexcept>
#include <string>
#include <vector>

#include "library/store.h"

namespace wb::bugs {

struct Screenshot {
    std::string bytes, mime = "image/png";  // PNG or JPEG data
    std::string name;
};

struct Report {
    std::string title, description;
    std::vector<Screenshot> screenshots;  // at most 3 are sent; the relay drops any over 3 MB
    std::vector<std::string> extra_log;
};

constexpr size_t kMaxScreenshots = 3;
constexpr size_t kMaxScreenshotBytes = 3 * 1024 * 1024;

// WRECKBOX_BUG_RELAY replaces the relay's address (tests point it at a local fake).
std::string relay_url();
std::string platform_text();  // "windows 10.0 (Build 19045), native build"
// The JSON the relay receives.
json build(const Report& report, const LibraryStore& store);
// Sends it; returns the number of the GitHub issue. Throws std::runtime_error with a readable message.
int send(const Report& report, const LibraryStore& store);

}  // namespace wb::bugs
