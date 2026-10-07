// Update check (port of Updates in app/lib/services.dart): asks GitHub for the releases of the public releases repo and says
// whether the newest one with this build's zip ("win-native") is newer than this build. The Flutter app's releases in the same
// repo are never offered. Blocking (a worker calls it); never throws.
#pragma once
#include <optional>
#include <string>

namespace wb::updates {

struct Info {
    std::string version, notes, url;  // url: this build's zip
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
