#include "net/slsk/match.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <regex>
#include <set>

namespace wb::slsk {
namespace {

const std::set<std::string> kAudioExt = {"flac", "wav", "aiff", "aif", "alac", "m4a", "mp3", "aac", "ogg", "opus"};
const std::map<std::string, int> kLosslessRank = {{"flac", 5}, {"aiff", 4}, {"aif", 4}, {"wav", 4}, {"alac", 4}};
const std::vector<std::string> kVariantWords = {"remix", "rmx", "live", "acapella", "acappella", "instrumental", "karaoke", "cover", "edit", "extended", "vip",
                                                "bootleg", "rework", "sped", "slowed", "nightcore", "reverb", "8d", "mashup", "flip", "dub", "version",
                                                "demo", "radio"};

std::vector<std::string> split_words(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        const size_t end = s.find(' ', i);
        out.push_back(s.substr(i, end == std::string::npos ? std::string::npos : end - i));
        if (end == std::string::npos) break;
        i = end + 1;
    }
    out.erase(std::remove(out.begin(), out.end(), std::string()), out.end());
    return out;
}

bool has(const std::vector<std::string>& words, const std::string& w) { return std::find(words.begin(), words.end(), w) != words.end(); }

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The last `n` "/"-separated parts of a peer path (peers use backslashes).
std::vector<std::string> path_parts(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    std::vector<std::string> parts;
    size_t i = 0;
    for (;;) {
        const size_t end = path.find('/', i);
        parts.push_back(path.substr(i, end == std::string::npos ? std::string::npos : end - i));
        if (end == std::string::npos) break;
        i = end + 1;
    }
    return parts;
}

std::string last_parts(const std::vector<std::string>& parts, size_t n) {
    std::string out;
    for (size_t i = parts.size() > n ? parts.size() - n : 0; i < parts.size(); ++i) out += (out.empty() ? "" : " ") + parts[i];
    return out;
}

std::string without_extension(const std::string& name) {
    const auto dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

}  // namespace

std::string norm(const std::string& s) {
    // NFKD, then keep only ASCII: that folds accents and drops letters with no ASCII form, as Python's
    // unicodedata.normalize("NFKD").encode("ascii", "ignore") does.
    std::wstring w = widen(s);
    std::wstring d;
    if (!w.empty()) {
        d.resize(w.size() * 4 + 16);
        const int n = NormalizeString(NormalizationKD, w.c_str(), int(w.size()), d.data(), int(d.size()));
        d.resize(n > 0 ? size_t(n) : w.size());
        if (n <= 0) d = w;
    }
    std::string out;
    bool in_word = false;
    const auto put = [&](char c) {
        const bool word = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (word) {
            if (!in_word && !out.empty()) out += ' ';
            out += c;
        }
        in_word = word;
    };
    for (const wchar_t wc : d) {
        if (wc > 127) continue;  // dropped, without breaking the word it is in (accents inside a word)
        char c = char(std::tolower(static_cast<unsigned char>(wc)));
        if (c == '&') {
            for (const char x : std::string(" and ")) put(x);
            continue;
        }
        put(c);
    }
    return out;
}

std::string clean_title(const std::string& title) {
    using std::regex;
    static const regex feat_paren(R"([\(\[]\s*(feat|ft|with)\.?\s[^\)\]]*[\)\]])", regex::icase);
    static const regex feat_tail(R"(\s(feat|ft)\.?\s.*$)", regex::icase);
    static const regex dash_noise(R"(\s-\s.*(remaster|original mix|radio edit|mono|stereo).*$)", regex::icase);
    static const regex paren_noise(R"([\(\[]\s*(original mix|remaster(ed)?[^\)\]]*)[\)\]])", regex::icase);
    std::string t = std::regex_replace(title, feat_paren, "");
    t = std::regex_replace(t, feat_tail, "");
    t = std::regex_replace(t, dash_noise, "");
    t = std::regex_replace(t, paren_noise, "");
    const auto a = t.find_first_not_of(" \t\r\n"), b = t.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : t.substr(a, b - a + 1);
}

std::vector<std::string> search_queries(const LibraryTrack& track, const std::string& custom_query) {
    static const std::regex bracketed(R"([\(\[].*?[\)\]]|\s-\s.*$)");
    std::string custom = custom_query;
    custom.erase(0, custom.find_first_not_of(" \t\r\n"));
    custom.erase(custom.find_last_not_of(" \t\r\n") + 1);
    const std::string artist = track.artists.empty() ? "" : norm(track.artists[0]);
    const std::string title = norm(clean_title(track.title));
    const std::string bare = norm(std::regex_replace(track.title, bracketed, ""));
    auto join = [](const std::string& a, const std::string& b) {
        std::string s = a + " " + b;
        const auto x = s.find_first_not_of(' '), y = s.find_last_not_of(' ');
        return x == std::string::npos ? std::string() : s.substr(x, y - x + 1);
    };
    std::vector<std::string> qs;
    if (!custom.empty()) qs.push_back(norm(custom));
    qs.push_back(join(artist, title));
    if (!bare.empty() && bare != title) qs.push_back(join(artist, bare));
    return qs;
}

std::string Candidate::label() const {
    std::string up = ext;
    for (auto& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
    std::string q = up + (bitrate && *bitrate && !kLosslessRank.contains(ext) ? " " + std::to_string(*bitrate) + "kbps" : "");
    char mb[32];
    std::snprintf(mb, sizeof mb, "%.1f", double(size) / 1048576.0);
    return q + "  " + mb + "MB  " + username + "  " + (free_slot ? "free" : "queue " + std::to_string(queue)) + "  " + std::to_string(speed / 1024) + "KB/s";
}

std::optional<double> quality_of(const std::string& ext, std::optional<int> bitrate, int min_kbps, bool smaller) {
    const int lossless = smaller ? 0 : 10;
    if (const auto it = kLosslessRank.find(ext); it != kLosslessRank.end()) return lossless + it->second;
    if (ext == "m4a" && (!bitrate || *bitrate > 500)) return lossless + 3;  // Apple Lossless in an .m4a container
    if (!bitrate) return std::nullopt;                            // lossy file of unknown quality
    if (*bitrate < min_kbps) return std::nullopt;
    // Lossy tops out at 320 kbps; higher claims are mislabelled, so they never outrank lossless (10+).
    const int br = std::min(*bitrate, 320);
    const std::map<std::string, double> base = {{"mp3", 0.0}, {"m4a", -0.5}, {"aac", -0.5}, {"ogg", -1.0}, {"opus", -1.0}};
    const auto b = base.find(ext);
    return (b == base.end() ? -2.0 : b->second) + double(br) / 32;
}

bool file_matches(const LibraryTrack& track, const std::string& path, std::optional<int> duration, int tolerance_seconds) {
    const auto parts = path_parts(path);
    const auto hay = split_words(norm(last_parts(parts, 3)));
    const auto name = split_words(norm(without_extension(parts.back())));
    const auto title_words = split_words(norm(clean_title(track.title)));
    if (title_words.empty()) return false;
    for (const auto& w : title_words)
        if (!has(name, w) && !has(hay, w)) return false;
    std::set<std::string> artist_words;
    for (const auto& a : track.artists)
        for (const auto& w : split_words(norm(a)))
            if (w.size() > 1) artist_words.insert(w);
    if (!artist_words.empty() && std::none_of(artist_words.begin(), artist_words.end(), [&](const std::string& w) { return has(hay, w); })) return false;
    const auto wanted_list = split_words(norm(track.title));
    for (const auto& w : kVariantWords)
        if (has(name, w) && !has(wanted_list, w)) return false;
    if (duration && *duration && track.duration_ms && *track.duration_ms)
        if (std::fabs(double(*duration) - double(*track.duration_ms) / 1000.0) > tolerance_seconds) return false;
    return true;
}

std::vector<Candidate> rank(const LibraryTrack& track, const std::vector<UserResult>& results, const MatchConfig& cfg, const std::optional<std::string>& loose) {
    std::vector<Candidate> cands;
    for (const auto& r : results)
        for (const auto& f : r.files) {
            std::string ext = f.extension;
            if (ext.empty()) {
                const auto dot = f.filename.rfind('.');
                ext = dot == std::string::npos ? f.filename : f.filename.substr(dot + 1);
            }
            ext = lower(ext);
            ext.erase(0, ext.find_first_not_of('.'));
            if (!kAudioExt.contains(ext)) continue;
            std::optional<int> bitrate, duration;
            if (const auto it = f.attributes.find(kAttrBitrate); it != f.attributes.end()) bitrate = int(it->second);
            if (const auto it = f.attributes.find(kAttrDuration); it != f.attributes.end()) duration = int(it->second);
            const auto q = quality_of(ext, bitrate, cfg.min_lossy_kbps, cfg.prefer_smaller);
            if (!q || f.size < 500000) continue;
            if (loose) {
                const auto hay = split_words(norm(last_parts(path_parts(f.filename), 3)));
                bool all = true;
                for (const auto& w : split_words(norm(*loose))) all = all && has(hay, w);
                if (!all) continue;
                if (duration && *duration && track.duration_ms && *track.duration_ms &&
                    std::fabs(double(*duration) - double(*track.duration_ms) / 1000.0) > cfg.duration_tolerance_seconds * 3)
                    continue;
            } else if (!file_matches(track, f.filename, duration, cfg.duration_tolerance_seconds)) {
                continue;
            }
            Candidate c;
            c.username = r.username, c.path = f.filename, c.ext = ext, c.size = f.size, c.bitrate = bitrate, c.duration = duration;
            c.free_slot = r.free_slots, c.speed = int(r.avg_speed), c.queue = int(r.queue_size), c.quality = *q;
            cands.push_back(std::move(c));
        }
    // Best format first; within a format prefer peers that can send right now, then speed.
    std::stable_sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
        return std::tuple(a.quality, a.free_slot, -std::min(a.queue, 50), a.speed) > std::tuple(b.quality, b.free_slot, -std::min(b.queue, 50), b.speed);
    });
    // One file per user, so a failed peer doesn't eat every retry.
    std::set<std::string> seen;
    std::vector<Candidate> unique;
    for (auto& c : cands)
        if (seen.insert(c.username).second) unique.push_back(std::move(c));
    return unique;
}

}  // namespace wb::slsk
