// Update check (port of Updates in app/lib/services.dart): asks GitHub for the latest release of the public releases repo and
// says whether it is newer than this build. Blocking (a worker calls it); never throws.
#pragma once
#include <optional>
#include <string>

namespace wb::updates {

struct Info {
    std::string version, notes, url;  // url: the Windows download, else the release page
};

struct Result {
    std::optional<Info> newer;  // set when a newer release exists
    std::string error;          // why the check failed ("no internet connection."), empty if it worked
};

// "0.3.1" > "0.1.0"; a leading "v" and anything after the first three numbers are ignored.
bool is_newer(const std::string& latest, const std::string& current);

// WRECKBOX_UPDATE_URL replaces the releases API address (tests point it at a local fake).
std::string releases_url();

Result check(const std::string& current_version = WB_VERSION);

}  // namespace wb::updates
