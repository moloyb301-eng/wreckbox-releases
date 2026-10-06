#include "sources/spotify.h"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <thread>

#include "model/settings.h"
#include "net/http_client.h"
#include "net/oauth.h"

namespace wb::spotify {
namespace {

std::string token(const std::map<std::string, std::string>& body) {
    auto& s = Settings::current();
    auto fields = body;
    fields["client_id"] = s.spotify_client_id;
    const auto res = http::request("POST", "https://accounts.spotify.com/api/token", {{"Content-Type", "application/x-www-form-urlencoded"}},
                                   oauth::form(fields));
    if (res.status == 0) throw std::runtime_error(res.error);
    const json j = json::parse(res.body, nullptr, false);
    if (res.status != 200 || !j.is_object() || !j.contains("access_token"))
        throw std::runtime_error("Spotify sign-in failed: " +
                                 (j.is_object() ? j.value("error_description", j.value("error", std::to_string(res.status))) : std::to_string(res.status)));
    if (j.contains("refresh_token") && j["refresh_token"].is_string()) {
        s.spotify_refresh_token = j["refresh_token"].get<std::string>();
        s.save();
    }
    return j["access_token"].get<std::string>();
}

std::string sign_in() {
    const auto& s = Settings::current();
    if (s.spotify_client_id.empty()) throw std::runtime_error("Add your Spotify client ID in Settings first.");
    oauth::Loopback lb(8888);
    if (!lb.ok()) throw std::runtime_error(lb.error());
    const std::string verifier = oauth::random_string(64), state = oauth::random_string(16);
    oauth::open_in_browser("https://accounts.spotify.com/authorize?" + oauth::form({{"client_id", s.spotify_client_id},
                                                                                    {"response_type", "code"},
                                                                                    {"redirect_uri", kRedirect},
                                                                                    {"scope", kScopes},
                                                                                    {"code_challenge_method", "S256"},
                                                                                    {"code_challenge", oauth::pkce_challenge(verifier)},
                                                                                    {"state", state}}));
    const auto cb = lb.wait("/callback", "WreckBox is connected to Spotify.");
    if (!cb) throw std::runtime_error("Sign-in timed out.");
    if (!cb->contains("state") || cb->at("state") != state) throw std::runtime_error("Sign-in was interrupted — try again.");
    if (!cb->contains("code")) throw std::runtime_error("Spotify said: " + (cb->contains("error") ? cb->at("error") : std::string("no code")));
    return token({{"grant_type", "authorization_code"}, {"code", cb->at("code")}, {"redirect_uri", kRedirect}, {"code_verifier", verifier}});
}

json api_get(const std::string& tok, const std::string& path_or_url) {
    const std::string url = path_or_url.rfind("http", 0) == 0 ? path_or_url : "https://api.spotify.com/v1/" + path_or_url;
    for (int attempt = 0; attempt < 5; ++attempt) {
        const auto res = http::get(url, {{"Authorization", "Bearer " + tok}});
        if (res.status == 429) {  // rate limited: wait as long as Spotify asks
            const int wait = std::max(1, std::atoi(res.header("retry-after").c_str()));
            std::this_thread::sleep_for(std::chrono::seconds(res.header("retry-after").empty() ? 2 : wait));
            continue;
        }
        if (res.status == 0) throw std::runtime_error(res.error);
        if (res.status != 200) throw std::runtime_error("Spotify API " + std::to_string(res.status) + " for " + path_or_url);
        return json::parse(res.body, nullptr, false);
    }
    throw std::runtime_error("Spotify kept rate-limiting; try again in a minute.");
}

std::vector<json> pages(const std::string& tok, const std::string& path) {
    std::vector<json> items;
    std::string next = path;
    while (!next.empty()) {
        const json page = api_get(tok, next);
        if (page.contains("items") && page["items"].is_array())
            for (const auto& i : page["items"]) items.push_back(i);
        next = page.contains("next") && page["next"].is_string() ? page["next"].get<std::string>() : "";
    }
    return items;
}

std::optional<SourceTrack> parse_item(const json& item) {
    if (!item.is_object()) return std::nullopt;
    const json& t = item.contains("track") ? item["track"] : item.contains("item") ? item["item"] : json();
    return parse_track(t, item.contains("added_at") && item["added_at"].is_string() ? std::optional(item["added_at"].get<std::string>()) : std::nullopt);
}

}  // namespace

std::string access_token() {
    if (const auto rt = Settings::current().spotify_refresh_token) {
        try {
            return token({{"grant_type", "refresh_token"}, {"refresh_token", *rt}});
        } catch (const std::exception&) {
            // expired or revoked → sign in again
        }
    }
    return sign_in();
}

std::optional<SourceTrack> parse_track(const json& t, const std::optional<std::string>& added_at) {
    if (!t.is_object() || t.value("type", "track") != "track") return std::nullopt;
    const json album = t.value("album", json::object());
    SourceTrack s;
    // The cover closest to 300 px wide.
    if (album.contains("images") && album["images"].is_array() && !album["images"].empty()) {
        auto images = album["images"];
        std::stable_sort(images.begin(), images.end(), [](const json& a, const json& b) {
            const auto w = [](const json& x) { return std::abs((x.contains("width") && x["width"].is_number() ? x["width"].get<int>() : 640) - 300); };
            return w(a) < w(b);
        });
        if (images[0].contains("url") && images[0]["url"].is_string()) s.artwork_url = images[0]["url"].get<std::string>();
    }
    if (t.contains("id") && t["id"].is_string()) s.spotify_id = t["id"].get<std::string>();
    s.name = t.value("name", "");
    if (t.contains("artists") && t["artists"].is_array())
        for (const auto& a : t["artists"])
            if (a.is_object() && a.contains("name") && a["name"].is_string()) s.artists.push_back(a["name"].get<std::string>());
    if (album.contains("name") && album["name"].is_string()) s.album = album["name"].get<std::string>();
    if (album.contains("release_date") && album["release_date"].is_string()) s.release_date = album["release_date"].get<std::string>();
    if (t.contains("external_ids") && t["external_ids"].is_object() && t["external_ids"].contains("isrc") && t["external_ids"]["isrc"].is_string())
        s.isrc = t["external_ids"]["isrc"].get<std::string>();
    if (t.contains("duration_ms") && t["duration_ms"].is_number()) s.duration_ms = t["duration_ms"].get<int64_t>();
    s.is_local = t.value("is_local", false);
    s.added_at = added_at;
    return s;
}

std::optional<SourceTrack> search(const std::string& tok, const std::string& artist, const std::string& title) {
    const std::string q = artist.empty() ? title : "track:" + title + " artist:" + artist;
    try {
        const json j = api_get(tok, "search?type=track&limit=5&q=" + http::url_encode(q));
        if (!j.contains("tracks") || !j["tracks"].contains("items")) return std::nullopt;
        const std::string want = normalized(title), want_artist = normalized(artist);
        for (const auto& it : j["tracks"]["items"]) {
            const auto st = parse_track(it);
            if (!st) continue;
            const std::string got = normalized(st->name);
            const bool artist_ok = artist.empty() || std::any_of(st->artists.begin(), st->artists.end(), [&](const std::string& a) {
                const std::string n = normalized(a);
                return want_artist.find(n) != std::string::npos || n.find(want_artist) != std::string::npos;
            });
            if (artist_ok && (got == want || got.rfind(want, 0) == 0 || want.rfind(got, 0) == 0)) return st;
        }
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

SourcePlaylist fetch_playlist(const std::string& tok, const std::optional<std::string>& id) {
    SourcePlaylist pl{"Liked Songs", std::nullopt, false, {}};
    std::vector<json> items;
    if (!id) {
        items = pages(tok, "me/tracks?limit=50");
    } else {
        const json meta = api_get(tok, "playlists/" + *id + "?fields=name,collaborative");
        pl.name = meta.value("name", "");
        pl.name.erase(0, pl.name.find_first_not_of(" \t"));
        pl.name.erase(pl.name.find_last_not_of(" \t") + 1);
        pl.id = id;
        pl.collaborative = meta.value("collaborative", false);
        try {
            items = pages(tok, "playlists/" + *id + "/items?limit=50");
        } catch (const std::exception&) {
            items = pages(tok, "playlists/" + *id + "/tracks?limit=50");  // older API path
        }
    }
    for (const auto& i : items)
        if (auto t = parse_item(i)) pl.tracks.push_back(std::move(*t));
    return pl;
}

Library run(const std::function<void(const std::string&)>& log) {
    const std::string tok = access_token();
    const json me = api_get(tok, "me");
    const std::string user = me.value("id", "");
    log("Signed in as " + (me.contains("display_name") && me["display_name"].is_string() ? me["display_name"].get<std::string>() : user));
    log("Fetching Liked Songs…");
    std::vector<SourcePlaylist> out{fetch_playlist(tok, std::nullopt)};
    log("  " + std::to_string(out[0].tracks.size()) + " liked songs");
    for (const auto& p : pages(tok, "me/playlists?limit=50")) {
        const json owner = p.value("owner", json::object());
        const bool mine = owner.value("id", "") == user, collab = p.value("collaborative", false);
        if (!(mine || collab)) continue;  // personal playlists only (Spotify blocks reading others' anyway)
        try {
            out.push_back(fetch_playlist(tok, p.value("id", "")));
            log("  " + out.back().name + ": " + std::to_string(out.back().tracks.size()) + " tracks");
        } catch (const std::exception& e) {
            log("  skipped " + p.value("name", "") + ": " + e.what());
        }
    }
    Library lib = sources::save("spotify", user, out);
    log("Library: " + std::to_string(lib.tracks.size()) + " unique tracks from " + std::to_string(lib.playlists.size()) + " playlists");
    return lib;
}

}  // namespace wb::spotify
