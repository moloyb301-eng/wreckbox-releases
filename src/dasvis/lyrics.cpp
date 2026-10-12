#include "dasvis/lyrics.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace wb::dasvis {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// "mm:ss", "mm:ss.x", "mm:ss.xx", "mm:ss.xxx" (or ':' before the fraction) → seconds; < 0 if it isn't a time.
double stamp(std::string_view s) {
    const size_t colon = s.find(':');
    if (colon == std::string_view::npos || colon == 0) return -1;
    for (size_t i = 0; i < s.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(s[i])) && i != colon && s[i] != '.' && s[i] != ':') return -1;
    std::string rest(s.substr(colon + 1));
    if (const size_t c2 = rest.find(':'); c2 != std::string::npos) rest[c2] = '.';
    return std::atof(std::string(s.substr(0, colon)).c_str()) * 60 + std::atof(rest.c_str());
}

}  // namespace

int Lyrics::at(double t) const {
    const auto it = std::upper_bound(lines.begin(), lines.end(), t, [](double v, const Line& l) { return v < l.start; });
    if (it == lines.begin()) return -1;
    const auto& l = *(it - 1);
    return t < l.end ? int(it - 1 - lines.begin()) : -1;
}

Lyrics parse_lrc(std::string_view lrc) {
    struct Stamped {
        double t;
        std::string text;  // "" ends the line before
    };
    std::vector<Stamped> all;
    double offset = 0;
    while (!lrc.empty()) {
        const size_t nl = lrc.find('\n');
        std::string_view line = trim(lrc.substr(0, nl));
        lrc = nl == std::string_view::npos ? std::string_view{} : lrc.substr(nl + 1);
        std::vector<double> times;
        while (line.size() > 2 && line[0] == '[') {
            const size_t close = line.find(']');
            if (close == std::string_view::npos) break;
            const std::string_view in = line.substr(1, close - 1);
            if (const double t = stamp(in); t >= 0) times.push_back(t);
            else if (in.starts_with("offset:")) offset = std::atof(std::string(in.substr(7)).c_str()) / 1000;
            line = trim(line.substr(close + 1));
        }
        if (times.empty()) continue;
        std::string text;  // without word stamps
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '<') {
                const size_t close = line.find('>', i);
                if (close != std::string_view::npos && stamp(line.substr(i + 1, close - i - 1)) >= 0) {
                    i = close;
                    continue;
                }
            }
            text += line[i];
        }
        text = std::string(trim(text));
        for (const double t : times) all.push_back({std::max(0.0, t - offset), text});
    }
    std::stable_sort(all.begin(), all.end(), [](const Stamped& a, const Stamped& b) { return a.t < b.t; });
    Lyrics out;
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i].text.empty()) continue;
        const double next = i + 1 < all.size() ? all[i + 1].t : all[i].t + 8;
        out.lines.push_back({all[i].t, std::min(next, all[i].t + 8), all[i].text});
    }
    return out;
}

std::string lrclib_synced(std::string_view json_body) {
    const auto j = nlohmann::json::parse(json_body, nullptr, false);
    for (const auto& song : j.is_array() ? j : nlohmann::json::array({j})) {  // /api/search: the first that has them
        if (!song.is_object()) continue;
        const auto s = song.find("syncedLyrics");
        if (s != song.end() && s->is_string() && !s->get<std::string>().empty()) return s->get<std::string>();
    }
    return {};
}

}  // namespace wb::dasvis
