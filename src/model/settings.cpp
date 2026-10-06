#include "model/settings.h"

#include "model/paths.h"

using nlohmann::json;

namespace wb {
namespace {

void get(const json& j, const char* k, std::string& out) {
    if (const auto it = j.find(k); it != j.end() && it->is_string()) out = it->get<std::string>();
}
void get(const json& j, const char* k, std::optional<std::string>& out) {
    if (const auto it = j.find(k); it != j.end() && it->is_string()) out = it->get<std::string>();
}
void get(const json& j, const char* k, bool& out) {
    if (const auto it = j.find(k); it != j.end() && it->is_boolean()) out = it->get<bool>();
}
void get(const json& j, const char* k, std::vector<std::string>& out) {
    if (const auto it = j.find(k); it != j.end() && it->is_array()) {
        out.clear();
        for (const auto& v : *it)
            if (v.is_string()) out.push_back(v.get<std::string>());
    }
}
json opt(const std::optional<std::string>& v) { return v ? json(*v) : json(nullptr); }

}  // namespace

Settings& Settings::current() {
    static Settings s;
    return s;
}

void Settings::load() {
    Settings s;
    const auto text = paths::read_file(paths::settings_file());
    const json j = text ? json::parse(*text, nullptr, false) : json();
    if (j.is_object()) {
        s.extra = j;
        get(j, "spotifyClientId", s.spotify_client_id);
        get(j, "spotifyRefreshToken", s.spotify_refresh_token);
        get(j, "googleClientId", s.google_client_id);
        get(j, "googleClientSecret", s.google_client_secret);
        get(j, "googleRefreshToken", s.google_refresh_token);
        get(j, "dropboxAppKey", s.dropbox_app_key);
        get(j, "dropboxRefreshToken", s.dropbox_refresh_token);
        get(j, "dropboxFolder", s.dropbox_folder);
        get(j, "reporterName", s.reporter_name);
        get(j, "reporterContact", s.reporter_contact);
        get(j, "extraScanFolders", s.extra_scan_folders);
        get(j, "organiseDownloads", s.organise_downloads);
        get(j, "pairedDesktop", s.paired_desktop);
        get(j, "pairToken", s.pair_token);
        get(j, "desktopPairToken", s.desktop_pair_token);
        get(j, "onboarded", s.onboarded);
        get(j, "accountToken", s.account_token);
        get(j, "accountEmail", s.account_email);
        get(j, "accountName", s.account_name);
        get(j, "deviceId", s.device_id);
        get(j, "connectedComputerId", s.connected_computer_id);
        get(j, "shareRemotely", s.share_remotely);
    }
    current() = std::move(s);
}

void Settings::save() const {
    json j = extra;
    j["spotifyClientId"] = spotify_client_id;
    j["spotifyRefreshToken"] = opt(spotify_refresh_token);
    j["googleClientId"] = google_client_id;
    j["googleClientSecret"] = google_client_secret;
    j["googleRefreshToken"] = opt(google_refresh_token);
    j["dropboxAppKey"] = dropbox_app_key;
    j["dropboxRefreshToken"] = opt(dropbox_refresh_token);
    j["dropboxFolder"] = dropbox_folder;
    j["reporterName"] = reporter_name;
    j["reporterContact"] = reporter_contact;
    j["extraScanFolders"] = extra_scan_folders;
    j["organiseDownloads"] = organise_downloads;
    j["pairedDesktop"] = opt(paired_desktop);
    j["pairToken"] = opt(pair_token);
    j["desktopPairToken"] = opt(desktop_pair_token);
    j["onboarded"] = onboarded;
    j["accountToken"] = opt(account_token);
    j["accountEmail"] = opt(account_email);
    j["accountName"] = account_name;
    j["deviceId"] = opt(device_id);
    j["connectedComputerId"] = opt(connected_computer_id);
    j["shareRemotely"] = share_remotely;
    paths::write_atomic(paths::settings_file(), j.dump(2));
}

}  // namespace wb
