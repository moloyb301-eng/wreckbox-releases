// Phase 7 extras: the update check and the bug report (both against small local fakes of GitHub and the relay: nothing
// here reaches a real service), and the library-folder list behind the Settings page.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

#include <httplib.h>

#include "library/store.h"
#include "model/paths.h"
#include "model/settings.h"
#include "net/bug_report.h"
#include "net/updates.h"

namespace fs = std::filesystem;
using wb::json;
static int failures = 0;

#define CHECK(cond, ...)                                                         \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            std::fprintf(stderr, "FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond); \
            std::fprintf(stderr, "" __VA_ARGS__);                                \
            std::fputc('\n', stderr);                                            \
        }                                                                        \
    } while (0)

// A local HTTP server on any free port; `handler` is set per test.
struct Fake {
    httplib::Server svr;
    std::thread thread;
    int port = 0;
    std::mutex m;
    httplib::Request last;
    int status = 200;
    std::string body;
    explicit Fake(const char* method = "GET") {
        auto h = [this](const httplib::Request& req, httplib::Response& res) {
            std::lock_guard lock(m);
            last = req;
            res.status = status;
            res.set_content(body, "application/json");
        };
        if (std::string(method) == "POST") svr.Post(".*", h);
        else svr.Get(".*", h);
        port = svr.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { svr.listen_after_bind(); });
    }
    ~Fake() {
        svr.stop();
        thread.join();
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port) + "/x"; }
};

static void updates_tests() {
    using wb::updates::is_newer;
    CHECK(is_newer("0.3.1", "0.1.0") && is_newer("1.0.0", "0.9.9") && is_newer("0.2.10", "0.2.9") && is_newer("v0.3.0", "0.2.9"));
    CHECK(!is_newer("0.1.0", "0.1.0") && !is_newer("0.1.0", "0.3.1") && !is_newer("", "0.1.0") && !is_newer("junk", "0.1.0"));
    CHECK(is_newer("0.2.0+4", "0.2.0-rc1") == false && is_newer("0.2.1+1", "0.2.0+9"), "build numbers and suffixes are ignored");
    CHECK(is_newer("1.2", "1.1.9") && !is_newer("1.2", "1.2.0"));

    Fake gh;
    _putenv_s("WRECKBOX_UPDATE_URL", gh.url().c_str());
    CHECK(wb::updates::releases_url() == gh.url());
    // The shared releases repo: the original app's newer release (no win-native zip), a pre-release, then ours.
    const json other = {{"tag_name", "v0.6.0"},
                          {"body", "original."},
                          {"assets", json::array({{{"name", "WreckBox-0.6.0-windows-x64.zip"}, {"browser_download_url", "https://example.test/other.zip"}}})}};
    const json pre = {{"tag_name", "v0.5.0"},
                      {"prerelease", true},
                      {"assets", json::array({{{"name", "WreckBox-0.5.0-win-native-x64.zip"}, {"browser_download_url", "https://example.test/pre.zip"}}})}};
    const json ours = {{"tag_name", "v0.3.1"},
                       {"body", "Faster everything."},
                       {"assets", json::array({{{"name", "wreckbox-android.apk"}, {"browser_download_url", "https://example.test/a.apk"}},
                                               {{"name", "WreckBox-0.3.1-win-native-x64.zip"}, {"browser_download_url", "https://example.test/win.zip"}}})}};
    gh.body = json::array({other, pre, ours}).dump();
    auto r = wb::updates::check("0.1.0");
    CHECK(r.error.empty() && r.newer && r.newer->version == "0.3.1" && r.newer->notes == "Faster everything." && r.newer->url == "https://example.test/win.zip",
          "the newest release with our zip, not the original app's: %s", r.newer ? r.newer->version.c_str() : r.error.c_str());
    CHECK(gh.last.get_header_value("accept") == "application/vnd.github+json");
    r = wb::updates::check("0.3.1");
    CHECK(r.error.empty() && !r.newer, "up to date");
    gh.body = json::array({other, pre}).dump();
    r = wb::updates::check("0.1.0");
    CHECK(r.error.empty() && !r.newer, "only the original app's releases (and a pre-release): nothing to announce");
    gh.status = 403;
    r = wb::updates::check("0.1.0");
    CHECK(!r.newer && r.error == "GitHub is busy — try again in a few minutes.", "%s", r.error.c_str());
    gh.status = 500;
    r = wb::updates::check("0.1.0");
    CHECK(!r.newer && r.error == "the update server answered 500.", "%s", r.error.c_str());
    gh.status = 200, gh.body = "<html>not json";
    CHECK(!wb::updates::check("0.1.0").error.empty());
    _putenv_s("WRECKBOX_UPDATE_URL", "http://127.0.0.1:1/x");
    r = wb::updates::check("0.1.0");
    CHECK(!r.newer && r.error == "No internet connection.", "%s", r.error.c_str());
    _putenv_s("WRECKBOX_UPDATE_URL", "");
    CHECK(wb::updates::releases_url().find("api.github.com/repos/") != std::string::npos);
}

static void bug_report_tests(wb::LibraryStore& store) {
    auto& s = wb::Settings::current();
    s.reporter_name = "Test Person";
    s.reporter_contact = "test@example.com";
    // 50 activity entries, via the store's own log (the report keeps the newest 40).
    for (int i = 0; i < 50; ++i) store.log("event" + std::to_string(i), "detail " + std::to_string(i));

    wb::bugs::Report rep;
    rep.title = "It broke";
    rep.description = "I clicked, it crashed.\nSecond line.";
    rep.screenshots = {{"abc", "image/png", "a.png"}, {std::string("\xFF\xD8\xFF", 3), "image/jpeg", "b.jpg"}, {"x", "image/png", "c.png"}, {"y", "image/png", "d.png"}};
    rep.extra_log = {"soulseek: something"};
    const json j = wb::bugs::build(rep, store);
    CHECK(j["title"] == "It broke" && j["description"] == rep.description && j["app"] == "WreckBox" && j["version"] == WB_VERSION);
    CHECK(j["platform"].get<std::string>().rfind("windows", 0) == 0 && j["platform"].get<std::string>().find("native build") != std::string::npos, "%s",
          j["platform"].dump().c_str());
    CHECK(j["reporter"] == "Test Person" && j["contact"] == "test@example.com");
    const std::string logs = j["logs"];
    CHECK(logs.rfind("engine ", 0) == 0 && logs.find("event49: detail 49") != std::string::npos && logs.find("event10: detail 10") != std::string::npos &&
              logs.find("event9: ") == std::string::npos && logs.find("soulseek: something") != std::string::npos,
          "40 newest entries");
    CHECK(logs.find("event49") < logs.find("event48"), "newest first");
    CHECK(j["screenshots"].size() == 3 && j["screenshots"][0]["data"] == "YWJj" && j["screenshots"][1]["type"] == "image/jpeg" && j["screenshots"][1]["data"] == "/9j/",
          "at most 3, base64: %s", j["screenshots"].dump().c_str());

    Fake relay("POST");
    _putenv_s("WRECKBOX_BUG_RELAY", relay.url().c_str());
    relay.body = json{{"ok", true}, {"id", "ab12cd34"}, {"issue", 57}}.dump();
    CHECK(wb::bugs::send(rep, store) == 57);
    {
        std::lock_guard lock(relay.m);
        CHECK(relay.last.get_header_value("x-wreckbox-key").size() == 32 && relay.last.get_header_value("content-type") == "application/json");
        const json got = json::parse(relay.last.body);
        CHECK(got["title"] == "It broke" && got["screenshots"].size() == 3);
    }
    auto fails = [&](int status, const std::string& body) -> std::string {
        relay.status = status, relay.body = body;
        try {
            wb::bugs::send(rep, store);
        } catch (const std::runtime_error& e) {
            return e.what();
        }
        return "";
    };
    CHECK(fails(401, R"({"ok":false,"error":"unauthorised"})") == "unauthorised");
    CHECK(fails(429, R"({"ok":false,"error":"too many reports, try again later"})") == "too many reports, try again later");
    CHECK(fails(502, "<html>") == "the report service answered 502");
    CHECK(fails(200, R"({"ok":false})") == "the report service answered 200");
    _putenv_s("WRECKBOX_BUG_RELAY", "http://127.0.0.1:1/x");
    CHECK(fails(200, "") == "No internet connection.");
    _putenv_s("WRECKBOX_BUG_RELAY", "");
    CHECK(wb::bugs::relay_url().find("workers.dev") != std::string::npos);
}

static void folder_tests(wb::LibraryStore& store, const fs::path& root) {
    const auto before = store.default_scan_folders();
    const fs::path music = root / L"More Music";
    fs::create_directories(music);
    const std::string folder = reinterpret_cast<const char*>(music.u8string().c_str());
    store.add_scan_folder(folder);
    store.add_scan_folder(folder + "\\");  // the same folder again, with a slash
    store.add_scan_folder(folder);
    auto folders = store.state_copy().scan_folders;
    CHECK(std::count(folders.begin(), folders.end(), folder) == 1 && folders.size() == before.size() + 1 - (std::find(before.begin(), before.end(), folder) != before.end()),
          "%zu folders", folders.size());
    CHECK(wb::Settings::current().extra_scan_folders.size() == 1);
    wb::Settings::load();
    CHECK(wb::Settings::current().extra_scan_folders == std::vector<std::string>{folder}, "saved to settings.json");
    wb::LibraryStore again;
    again.load();
    folders = again.state_copy().scan_folders;
    CHECK(std::find(folders.begin(), folders.end(), folder) != folders.end(), "saved to state.json");

    store.remove_scan_folder(folder + "/");  // other slash, trailing
    folders = store.state_copy().scan_folders;
    CHECK(std::find(folders.begin(), folders.end(), folder) == folders.end() && wb::Settings::current().extra_scan_folders.empty());
    store.add_scan_folder(folder);
    store.reset_scan_folders();
    folders = store.state_copy().scan_folders;
    CHECK(folders == store.default_scan_folders() && wb::Settings::current().extra_scan_folders.empty(), "reset to the defaults");
}

int main() {
    const fs::path root = fs::temp_directory_path() / L"wreckbox-extras-tests";
    fs::remove_all(root);
    wb::paths::init(root);
    wb::Settings::load();
    CHECK(wb::paths::overridden() && !wb::paths::downloads());
    wb::LibraryStore store;
    store.load();
    updates_tests();
    bug_report_tests(store);
    folder_tests(store, root);
    fs::remove_all(root);
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all extras tests passed");
    return 0;
}
