#include "library/matcher.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <limits>
#include <regex>

namespace wb {
namespace {

std::string upper_ascii(std::string s) {
    for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string lower_ascii(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string join(const std::vector<std::string>& v, const std::string& sep, size_t from = 0, size_t to = std::string::npos) {
    std::string out;
    to = std::min(to, v.size());
    for (size_t i = from; i < to; ++i) out += (i > from ? sep : "") + v[i];
    return out;
}

std::vector<std::string> split(const std::string& s, const std::string& sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t at; (at = s.find(sep, start)) != std::string::npos; start = at + sep.size()) out.push_back(s.substr(start, at - start));
    out.push_back(s.substr(start));
    return out;
}

std::string stem_of(const std::string& utf8_path) {
    const std::filesystem::path p(std::u8string(utf8_path.begin(), utf8_path.end()));
    const auto u = p.stem().u8string();
    return {u.begin(), u.end()};
}

}  // namespace

TrackMatcher::TrackMatcher(const std::vector<LibraryTrack>& tracks) : tracks_(tracks) {
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i].isrc) by_isrc_[upper_ascii(*tracks[i].isrc)] = i;
        by_title_[clean_title(tracks[i].title)].push_back(i);
    }
}

std::string TrackMatcher::clean_title(const std::string& s) {
    // The patterns only involve ASCII, so ASCII lower-casing is enough here; normalized() handles the rest.
    static const std::regex patterns[] = {
        std::regex(R"([\(\[]\s*(feat|ft|with)\.?\s[^\)\]]*[\)\]])"),
        std::regex(R"(\s(feat|ft)\.?\s.*$)"),
        std::regex(R"([\(\[][^\)\]]*(official|visuali[sz]er|lyric|audio|video|free\s?d(ownload|l)|out now)[^\)\]]*[\)\]])"),
        std::regex(R"([\(\[]\s*original mix\s*[\)\]])"),
        std::regex(R"(\s-\s(remaster(ed)?|original mix).*$)"),
    };
    std::string t = lower_ascii(s);
    for (const auto& re : patterns) t = std::regex_replace(t, re, "");
    return normalized(t);
}

std::optional<size_t> TrackMatcher::match(const FileFacts& f) const {
    if (f.isrc)
        if (const auto it = by_isrc_.find(upper_ascii(*f.isrc)); it != by_isrc_.end()) return it->second;
    std::vector<std::pair<std::string, std::string>> guesses;  // (artist, title)
    if (!f.artists.empty() && f.title) guesses.emplace_back(join(f.artists, " "), *f.title);
    const auto parts = split(stem_of(f.path), " - ");
    if (parts.size() >= 2) {
        guesses.emplace_back(parts.front(), join(parts, " - ", 1));
        guesses.emplace_back(parts.back(), join(parts, " - ", 0, parts.size() - 1));
    } else if (!f.artists.empty()) {
        guesses.emplace_back(join(f.artists, " "), parts.front());
    }
    for (const auto& [artist, title] : guesses) {
        const std::string file_artist = normalized(artist);
        const auto it = by_title_.find(clean_title(title));
        if (it == by_title_.end()) continue;
        std::optional<size_t> best;
        double best_gap = std::numeric_limits<double>::infinity();
        for (const size_t i : it->second) {
            const bool artist_ok = std::any_of(tracks_[i].artists.begin(), tracks_[i].artists.end(), [&](const std::string& a) {
                const std::string n = normalized(a);
                return !n.empty() && (file_artist.find(n) != std::string::npos || n.find(file_artist) != std::string::npos);
            });
            if (!artist_ok) continue;
            if (const double g = gap(i, f); g < best_gap) {
                best_gap = g;
                best = i;
            }
        }
        if (best && (!f.duration_sec || !tracks_[*best].duration_ms || best_gap < 8)) return best;
    }
    return std::nullopt;
}

double TrackMatcher::gap(size_t i, const FileFacts& f) const {
    const auto& a = tracks_[i].duration_ms;
    if (!a || !f.duration_sec) return 0;
    return std::abs(double(*a) / 1000.0 - *f.duration_sec);
}

std::set<std::string> compatible_keys(const std::string& c) {
    if (c.size() < 2) return {};
    int n = 0;
    try {
        size_t used = 0;
        n = std::stoi(c.substr(0, c.size() - 1), &used);
        if (used != c.size() - 1) return {};
    } catch (const std::exception&) {
        return {};
    }
    const char letter = c.back(), other = letter == 'A' ? 'B' : 'A';
    const int up = n % 12 + 1, down = (n + 10) % 12 + 1;
    return {std::to_string(n) + letter, std::to_string(up) + letter, std::to_string(down) + letter, std::to_string(n) + other};
}

int camelot_order(const std::optional<std::string>& c) {
    if (!c || c->empty()) return 999;
    int n = 99;
    try {
        size_t used = 0;
        const int v = std::stoi(c->substr(0, c->size() - 1), &used);
        if (used == c->size() - 1) n = v;
    } catch (const std::exception&) {
    }
    return n * 2 + (c->back() == 'B' ? 1 : 0);
}

}  // namespace wb
