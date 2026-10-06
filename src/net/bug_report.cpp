#include "net/bug_report.h"

#include <windows.h>

#include <cstdlib>
#include <format>

#include "engine/engine.h"
#include "model/settings.h"
#include "net/http_client.h"
#include "net/oauth.h"

namespace wb::bugs {
namespace {

// The relay's shared key: it only deters casual spam and is not a secret (the Flutter build ships the same one).
constexpr const char* kRelayUrl = "https://wreckbox-bug-relay.moloyb301.workers.dev";
constexpr const char* kRelayKey = "5f0221703ec26d88c14ba5578325fa57";

std::string base64(const std::string& bytes) {
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned v = (unsigned(uint8_t(bytes[i])) << 16) | (i + 1 < bytes.size() ? unsigned(uint8_t(bytes[i + 1])) << 8 : 0) |
                           (i + 2 < bytes.size() ? unsigned(uint8_t(bytes[i + 2])) : 0u);
        out += abc[v >> 18 & 63];
        out += abc[v >> 12 & 63];
        out += i + 1 < bytes.size() ? abc[v >> 6 & 63] : '=';
        out += i + 2 < bytes.size() ? abc[v & 63] : '=';
    }
    return out;
}

}  // namespace

std::string relay_url() {
    if (const char* v = std::getenv("WRECKBOX_BUG_RELAY"); v && *v) return v;
    return kRelayUrl;
}

std::string platform_text() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{sizeof v};
    if (const auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")); fn && fn(&v) == 0)
        return std::format("windows {}.{} (Build {}), native build", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    return "windows, native build";
}

json build(const Report& report, const LibraryStore& store) {
    std::string log = std::string("engine ") + engine_version();
    const auto entries = store.state_copy().log;
    size_t shown = 0;
    for (auto it = entries.rbegin(); it != entries.rend() && shown < 40; ++it, ++shown) log += "\n" + it->date + " " + it->event + ": " + it->detail;
    for (const auto& line : report.extra_log) log += "\n" + line;

    json shots = json::array();
    for (size_t i = 0; i < report.screenshots.size() && i < kMaxScreenshots; ++i)
        shots.push_back({{"type", report.screenshots[i].mime}, {"data", base64(report.screenshots[i].bytes)}});
    const auto& s = Settings::current();
    return {{"title", report.title},
            {"description", report.description},
            {"app", "WreckBox"},
            {"version", WB_VERSION},
            {"platform", platform_text()},
            {"reporter", s.reporter_name},
            {"contact", s.reporter_contact},
            {"logs", log},
            {"screenshots", shots}};
}

int send(const Report& report, const LibraryStore& store) {
    const auto res = http::request("POST", relay_url(), {{"content-type", "application/json"}, {"x-wreckbox-key", kRelayKey}},
                                   build(report, store).dump(), std::chrono::seconds(60));
    if (res.status == 0) throw std::runtime_error(res.error);
    const json j = json::parse(res.body, nullptr, false);
    if (res.status != 200 || !j.is_object() || !j.value("ok", false))
        throw std::runtime_error(j.is_object() && j.contains("error") && j["error"].is_string()
                                     ? j["error"].get<std::string>()
                                     : "the report service answered " + std::to_string(res.status));
    return j.value("issue", 0);
}

}  // namespace wb::bugs
