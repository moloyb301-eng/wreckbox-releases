#include "net/updates.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "model/model.h"
#include "net/http_client.h"

namespace wb::updates {
namespace {

constexpr const char* kReleasesRepo = "moloyb301-eng/wreckbox-releases";  // the same public repo the original build checks

std::vector<int> parse(std::string v) {
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    std::vector<int> out;
    size_t i = 0;
    while (i <= v.size()) {
        size_t end = v.find_first_of(".+-", i);
        if (end == std::string::npos) end = v.size();
        const std::string part = v.substr(i, end - i);
        out.push_back(part.empty() || !std::isdigit(static_cast<unsigned char>(part[0])) ? 0 : std::atoi(part.c_str()));
        i = end + 1;
    }
    return out;
}

}  // namespace

bool is_newer(const std::string& latest, const std::string& current) {
    const auto a = parse(latest), b = parse(current);
    for (size_t i = 0; i < 3; ++i) {
        const int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

std::string releases_url() {
    if (const char* v = std::getenv("WRECKBOX_UPDATE_URL"); v && *v) return v;
    return std::string("https://api.github.com/repos/") + kReleasesRepo + "/releases?per_page=30";  // newest first
}

Result check(const std::string& current_version) {
    const auto res = http::get(releases_url(), {{"accept", "application/vnd.github+json"}}, std::chrono::seconds(15));
    if (res.status == 0) return {std::nullopt, res.error};
    if (res.status != 200)
        return {std::nullopt, res.status == 403 || res.status == 429 ? "GitHub is busy — try again in a few minutes."
                                                                       : "the update server answered " + std::to_string(res.status) + "."};
    const json j = json::parse(res.body, nullptr, false);
    if (!j.is_array()) return {std::nullopt, "the update server sent something unexpected."};
    // The releases repo also carries the original app's releases (its own version numbers, a "windows" zip). Only a
    // release with this build's zip, "WreckBox-<v>-win-native-x64.zip", is an update for it: the newest such one counts.
    // Drafts and pre-releases aren't offered.
    for (const auto& rel : j) {
        if (!rel.is_object() || rel.value("draft", false) || rel.value("prerelease", false) || !rel.contains("assets") ||
            !rel["assets"].is_array())
            continue;
        for (const auto& a : rel["assets"]) {
            std::string name = a.is_object() ? a.value("name", "") : "";
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            if (name.find("win-native") == std::string::npos || !a.contains("browser_download_url") || !a["browser_download_url"].is_string())
                continue;
            std::string latest = rel.value("tag_name", "");
            if (!latest.empty() && (latest[0] == 'v' || latest[0] == 'V')) latest.erase(0, 1);
            if (!is_newer(latest, current_version)) return {};
            return {Info{latest, rel.value("body", ""), a["browser_download_url"].get<std::string>()}, ""};
        }
    }
    return {};
}

}  // namespace wb::updates
