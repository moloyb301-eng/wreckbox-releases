#include "sources/youtube.h"

#include <map>
#include <regex>
#include <stdexcept>
#include <tuple>

#include "model/settings.h"
#include "net/http_client.h"
#include "net/oauth.h"
#include "sources/sources.h"
#include "sources/spotify.h"

namespace wb::youtube {
namespace {

constexpr const char* kScope = "https://www.googleapis.com/auth/youtube.readonly";

std::wstring wtrim(const std::wstring& s) {
    const auto a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    return s.substr(a, s.find_last_not_of(L" \t\r\n") - a + 1);
}

std::string token(const std::map<std::string, std::string>& body) {
    auto& s = Settings::current();
    auto fields = body;
    fields["client_id"] = s.google_client_id;
    // Google's "Desktop app" clients come with a secret that isn't confidential (Google's own wording); the token
    // endpoint still wants it.
    if (!s.google_client_secret.empty()) fields["client_secret"] = s.google_client_secret;
    const auto res = http::request("POST", "https://oauth2.googleapis.com/token", {{"Content-Type", "application/x-www-form-urlencoded"}},
                                   oauth::form(fields));
    if (res.status == 0) throw std::runtime_error(res.error);
    const json j = json::parse(res.body, nullptr, false);
    if (!j.is_object() || !j.contains("access_token"))
        throw std::runtime_error("Google sign-in failed: " +
                                 (j.is_object() ? j.value("error_description", j.value("error", std::to_string(res.status))) : std::to_string(res.status)));
    if (j.contains("refresh_token") && j["refresh_token"].is_string()) {
        s.google_refresh_token = j["refresh_token"].get<std::string>();
        s.save();
    }
    return j["access_token"].get<std::string>();
}

std::string sign_in() {
    const auto& s = Settings::current();
    if (s.google_client_id.empty()) throw std::runtime_error("Add your Google client ID in Settings first.");
    oauth::Loopback lb(0);  // Google accepts any loopback port for "Desktop app" clients
    if (!lb.ok()) throw std::runtime_error(lb.error());
    const std::string redirect = "http://127.0.0.1:" + std::to_string(lb.port());
    const std::string verifier = oauth::random_string(64), state = oauth::random_string(16);
    oauth::open_in_browser("https://accounts.google.com/o/oauth2/v2/auth?" + oauth::form({{"client_id", s.google_client_id},
                                                                                          {"redirect_uri", redirect},
                                                                                          {"response_type", "code"},
                                                                                          {"scope", kScope},
                                                                                          {"code_challenge", oauth::pkce_challenge(verifier)},
                                                                                          {"code_challenge_method", "S256"},
                                                                                          {"access_type", "offline"},
                                                                                          {"prompt", "consent"},
                                                                                          {"state", state}}));
    const auto cb = lb.wait("/", "WreckBox is connected to YouTube.");
    if (!cb) throw std::runtime_error("Google sign-in timed out.");
    if (!cb->contains("state") || cb->at("state") != state) throw std::runtime_error("Sign-in was interrupted — try again.");
    if (!cb->contains("code")) throw std::runtime_error("Google said: " + (cb->contains("error") ? cb->at("error") : std::string("no code")));
    return token({{"grant_type", "authorization_code"}, {"code", cb->at("code")}, {"redirect_uri", redirect}, {"code_verifier", verifier}});
}

std::string access() {
    if (const auto rt = Settings::current().google_refresh_token) {
        try {
            return token({{"grant_type", "refresh_token"}, {"refresh_token", *rt}});
        } catch (const std::exception&) {
            // expired or revoked → sign in again
        }
    }
    return sign_in();
}

json api_get(const std::string& tok, const std::string& path, const std::map<std::string, std::string>& q) {
    const auto res = http::get("https://www.googleapis.com/youtube/v3/" + path + "?" + oauth::form(q), {{"Authorization", "Bearer " + tok}});
    if (res.status == 0) throw std::runtime_error(res.error);
    const json j = json::parse(res.body, nullptr, false);
    if (res.status != 200) {
        std::string msg = std::to_string(res.status);
        if (j.is_object() && j.contains("error") && j["error"].is_object()) msg = j["error"].value("message", msg);
        throw std::runtime_error(msg.find("quota") != std::string::npos ? "YouTube daily quota used up — try again tomorrow." : "YouTube: " + msg);
    }
    return j;
}

std::vector<json> pages(const std::string& tok, const std::string& path, std::map<std::string, std::string> q) {
    std::vector<json> out;
    q["maxResults"] = "50";
    for (;;) {
        const json j = api_get(tok, path, q);
        if (j.contains("items") && j["items"].is_array())
            for (const auto& i : j["items"]) out.push_back(i);
        if (!j.contains("nextPageToken") || !j["nextPageToken"].is_string()) break;
        q["pageToken"] = j["nextPageToken"].get<std::string>();
    }
    return out;
}

std::string str_at(const json& j, std::initializer_list<const char*> path) {
    const json* p = &j;
    for (const char* k : path) {
        if (!p->is_object() || !p->contains(k)) return "";
        p = &(*p)[k];
    }
    return p->is_string() ? p->get<std::string>() : "";
}

}  // namespace

std::pair<std::vector<std::string>, std::string> parse_title(const std::string& video_title, const std::string& channel_utf8) {
    using std::wregex;
    static const wregex noise(
        LR"(\s*[\(\[\{][^\)\]\}]*(official|video|audio|lyric|visuali[sz]er|music video|mv|hd|4k|hq|explicit|clean|full song|out now|free d(ownload|l)|premiere)[^\)\]\}]*[\)\]\}])",
        wregex::icase);
    static const wregex pipe(LR"(\s*\|.*$)"), spaces(LR"(\s+)"), dash(LR"(^(.+?)\s+[-–—]\s+(.+)$)"),
        suffix(LR"(\s*(VEVO|Official|Music)$)", wregex::icase), quotes(LR"(^["“]|["”]$)"), seps(LR"(\s*(,|&| x | X | feat\.? | ft\.? )\s*)");
    std::wstring title = std::regex_replace(widen(video_title), noise, L"");
    title = wtrim(std::regex_replace(std::regex_replace(title, pipe, L""), spaces, L" "));
    const std::wstring channel = widen(channel_utf8);
    std::wstring artist;
    if (channel.size() >= 8 && channel.ends_with(L" - Topic")) {
        artist = channel.substr(0, channel.size() - 8);
    } else if (std::wsmatch m; std::regex_match(title, m, dash)) {
        artist = m[1].str();
        title = m[2].str();
    } else {
        artist = wtrim(std::regex_replace(channel, suffix, L""));
    }
    title = wtrim(std::regex_replace(title, quotes, L""));
    std::vector<std::string> artists;
    for (std::wsregex_token_iterator it(artist.begin(), artist.end(), seps, -1), end; it != end; ++it)
        if (const auto a = wtrim(it->str()); !a.empty()) artists.push_back(narrow(a));
    if (artists.empty()) artists.push_back(narrow(artist));
    return {artists, narrow(title)};
}

std::optional<int64_t> parse_iso_duration(const std::string& s) {
    static const std::regex iso(R"(^PT(?:(\d+)H)?(?:(\d+)M)?(?:(\d+)S)?$)");
    std::smatch m;
    if (!std::regex_match(s, m, iso)) return std::nullopt;
    auto n = [&](int i) { return m[i].matched ? std::stoll(m[i].str()) : 0LL; };
    return ((n(1) * 60 + n(2)) * 60 + n(3)) * 1000;
}

namespace {

// (id, title, channel, added at)
using Video = std::tuple<std::string, std::string, std::string, std::optional<std::string>>;

// A Spotify token when Spotify is connected (YouTube songs then get its metadata), else nothing.
std::optional<std::string> spotify_if_connected() {
    if (!Settings::current().spotify_refresh_token) return std::nullopt;
    try {
        return spotify::access_token();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<SourceTrack> to_tracks(const std::string& tok, const std::optional<std::string>& sp, const std::vector<Video>& videos, bool music_only) {
    // Duration and category, 50 videos per request.
    std::map<std::string, std::pair<std::optional<int64_t>, std::string>> details;
    for (size_t i = 0; i < videos.size(); i += 50) {
        std::string ids;
        for (size_t k = i; k < std::min(i + 50, videos.size()); ++k) ids += (k > i ? "," : "") + std::get<0>(videos[k]);
        const json j = api_get(tok, "videos", {{"part", "contentDetails,snippet"}, {"id", ids}});
        if (j.contains("items"))
            for (const auto& v : j["items"])
                details[v.value("id", "")] = {parse_iso_duration(str_at(v, {"contentDetails", "duration"})), str_at(v, {"snippet", "categoryId"})};
    }
    std::vector<SourceTrack> out;
    for (const auto& [id, title, channel, added] : videos) {
        if (title == "Deleted video" || title == "Private video") continue;
        const auto d = details.contains(id) ? details[id] : std::pair<std::optional<int64_t>, std::string>{};
        if (music_only && d.second != "10") continue;  // category 10 = Music
        auto [artists, name] = parse_title(title, channel);
        std::optional<SourceTrack> match;
        if (sp) match = spotify::search(*sp, artists.front(), name);
        SourceTrack t;
        if (match) {
            t = *match;
        } else {
            t.name = name;
            t.artists = artists;
            t.duration_ms = d.first;
            t.artwork_url = "https://i.ytimg.com/vi/" + id + "/hqdefault.jpg";
        }
        t.youtube_id = id;
        t.added_at = added;
        out.push_back(std::move(t));
    }
    return out;
}

// One playlist ("LL" = liked music videos) as it is now.
SourcePlaylist fetch(const std::string& tok, const std::optional<std::string>& sp, const std::string& id, const std::string& title) {
    std::vector<Video> videos;
    if (id == "LL") {
        for (const auto& v : pages(tok, "videos", {{"part", "snippet"}, {"myRating", "like"}}))
            videos.emplace_back(v.value("id", ""), str_at(v, {"snippet", "title"}), str_at(v, {"snippet", "channelTitle"}), std::nullopt);
        return SourcePlaylist{"YT: Liked music", "LL", false, to_tracks(tok, sp, videos, true)};
    }
    for (const auto& i : pages(tok, "playlistItems", {{"part", "snippet,contentDetails"}, {"playlistId", id}})) {
        const std::string vid = str_at(i, {"contentDetails", "videoId"});
        if (vid.empty()) continue;
        const std::string added = str_at(i, {"snippet", "publishedAt"});  // when it was added to the playlist
        videos.emplace_back(vid, str_at(i, {"snippet", "title"}), str_at(i, {"snippet", "videoOwnerChannelTitle"}),
                            added.empty() ? std::nullopt : std::optional(added));
    }
    return SourcePlaylist{"YT: " + (title.empty() ? std::string("Untitled") : title), id, false, to_tracks(tok, sp, videos, false)};
}

}  // namespace

SourcePlaylist fetch_playlist(const std::string& id) {
    const std::string tok = access();
    const auto sp = spotify_if_connected();
    std::string title;
    if (id != "LL") {
        const json j = api_get(tok, "playlists", {{"part", "snippet"}, {"id", id}});
        if (!j.contains("items") || j["items"].empty()) throw std::runtime_error("This playlist is no longer on your YouTube account.");
        title = str_at(j["items"][0], {"snippet", "title"});
    }
    return fetch(tok, sp, id, title);
}

Library run(const std::function<void(const std::string&)>& log) {
    const std::string tok = access();
    const auto sp = spotify_if_connected();
    log("Fetching your YouTube playlists…");
    std::vector<SourcePlaylist> sources_out;
    for (const auto& pl : pages(tok, "playlists", {{"part", "snippet"}, {"mine", "true"}})) {
        sources_out.push_back(fetch(tok, sp, pl.value("id", ""), str_at(pl, {"snippet", "title"})));
        log("  " + sources_out.back().name + ": " + std::to_string(sources_out.back().tracks.size()) + " tracks");
    }
    log("Fetching liked music…");
    sources_out.push_back(fetch(tok, sp, "LL", ""));
    log("  YT: Liked music: " + std::to_string(sources_out.back().tracks.size()) + " tracks");

    Library lib = sources::save("youtube", "", sources_out);
    log("Library: " + std::to_string(lib.tracks.size()) + " unique tracks from " + std::to_string(lib.playlists.size()) + " playlists");
    return lib;
}

}  // namespace wb::youtube
