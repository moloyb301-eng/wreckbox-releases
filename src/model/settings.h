// Per-user settings. settings.json lives in the app's own folder, never inside the
// shared library folder. Same keys as the original build, so both builds share one file.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace wb {

struct Settings {
    std::string spotify_client_id;
    std::optional<std::string> spotify_refresh_token;
    std::string google_client_id, google_client_secret;
    std::optional<std::string> google_refresh_token;
    std::string dropbox_app_key;
    std::optional<std::string> dropbox_refresh_token;
    std::string dropbox_folder = "/Music";
    std::string reporter_name, reporter_contact;
    std::vector<std::string> extra_scan_folders;
    bool organise_downloads = true;
    std::optional<std::string> paired_desktop, pair_token;  // phone side; kept so the file round-trips
    std::optional<std::string> desktop_pair_token;          // token phones must present
    bool onboarded = false;
    std::optional<std::string> account_token, account_email, device_id, connected_computer_id;
    std::string account_name;
    bool share_remotely = false;  // keep the tunnel up so phones can reach this computer anywhere
    nlohmann::json extra = nlohmann::json::object();  // keys this build doesn't know (e.g. window position later)

    static Settings& current();
    static void load();  // missing / unreadable file → defaults, like the original source
    void save() const;   // atomic; throws on failure
};

}  // namespace wb
