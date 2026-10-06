#include "player/playlist.h"

#include <algorithm>
#include <filesystem>
#include <regex>

#include "model/paths.h"
#include "net/http_client.h"
#include "player/vlc_engine.h"

namespace fs = std::filesystem;

namespace wb::player {
namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

std::string extension_of(const std::string& location) {
    std::string path = location;
    if (const auto q = path.find_first_of("?#"); is_url(path) && q != std::string::npos) path.resize(q);
    const auto dot = path.find_last_of('.'), slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    return lower(path.substr(dot + 1));
}

std::string xml_unescape(std::string s) {
    for (const auto& [from, to] : {std::pair{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}})
        for (size_t at; (at = s.find(from)) != std::string::npos;) s.replace(at, std::string(from).size(), to);
    return s;
}

// file:///C:/Music/a%20b.mp3 → C:\Music\a b.mp3
std::string file_uri_to_path(const std::string& uri) {
    std::string u = uri.substr(uri.rfind("file:///", 0) == 0 ? 8 : 7), out;
    for (size_t i = 0; i < u.size(); ++i) {
        if (u[i] == '%' && i + 2 < u.size() && std::isxdigit(static_cast<unsigned char>(u[i + 1])) && std::isxdigit(static_cast<unsigned char>(u[i + 2]))) {
            out += char(std::stoi(u.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else out += u[i] == '/' ? '\\' : u[i];
    }
    return out;
}

std::string resolve(const std::string& entry, const std::string& base) {
    if (entry.rfind("file://", 0) == 0) return file_uri_to_path(entry);
    if (is_url(entry)) return entry;
    if (is_url(base)) {  // relative entry in a downloaded playlist
        const auto slash = base.find_last_of('/');
        return base.substr(0, slash + 1) + entry;
    }
    std::string e = entry;
    std::replace(e.begin(), e.end(), '/', '\\');
    const fs::path p(std::u8string(e.begin(), e.end()));
    if (p.is_absolute()) return e;
    const auto full = (fs::path(std::u8string(base.begin(), base.end())) / p).lexically_normal().u8string();
    return {full.begin(), full.end()};
}

}  // namespace

bool is_playlist(const std::string& location) {
    const std::string e = extension_of(location);
    return e == "m3u" || e == "m3u8" || e == "pls" || e == "xspf" || e == "asx";
}

std::vector<std::string> parse_playlist(const std::string& input, const std::string& base) {
    std::string text = input.rfind("\xEF\xBB\xBF", 0) == 0 ? input.substr(3) : input;
    std::vector<std::string> out;
    const std::string l = lower(text);
    if (l.find("<playlist") != std::string::npos || l.find("<asx") != std::string::npos) {
        // XSPF: <location>…</location>; ASX: <ref href="…"/>
        static const std::regex loc(R"(<location>\s*([^<]+?)\s*</location>)", std::regex::icase);
        static const std::regex href(R"re(<ref\s+href\s*=\s*"([^"]+)")re", std::regex::icase);
        for (const auto* re : {&loc, &href})
            for (std::sregex_iterator it(text.begin(), text.end(), *re), end; it != end; ++it) out.push_back(resolve(xml_unescape((*it)[1].str()), base));
        return out;
    }
    // PLS: File1=…, File2=… (other keys skipped). M3U / M3U8: one entry per line; # lines are comments or #EXTINF info.
    const bool pls = l.find("[playlist]") != std::string::npos;
    static const std::regex file(R"(File\d+\s*=\s*(.+))", std::regex::icase);
    for (size_t start = 0; start < text.size();) {
        const size_t nl = std::min(text.find('\n', start), text.size());
        const std::string line = trim(text.substr(start, nl - start));
        start = nl + 1;
        std::smatch m;
        if (pls) {
            if (std::regex_match(line, m, file)) out.push_back(resolve(trim(m[1].str()), base));
        } else if (!line.empty() && line[0] != '#') {
            out.push_back(resolve(line, base));
        }
    }
    return out;
}

std::vector<std::string> expand_playlist(const std::string& location) {
    if (!is_playlist(location)) return {location};
    std::string text, base;
    if (is_url(location)) {
        const auto r = http::get(location, {}, std::chrono::seconds(15));
        if (!r.ok()) return {location};  // let VLC try it directly
        text = r.body;
        base = location;
    } else {
        const fs::path p(std::u8string(location.begin(), location.end()));
        const auto t = paths::read_file(p);
        if (!t) return {location};
        text = *t;
        const auto dir = p.parent_path().u8string();
        base = {dir.begin(), dir.end()};
    }
    if (text.find("#EXT-X-") != std::string::npos) return {location};  // HLS: one stream, VLC's adaptive module plays it
    auto out = parse_playlist(text, base);
    return out.empty() ? std::vector<std::string>{location} : out;
}

}  // namespace wb::player
