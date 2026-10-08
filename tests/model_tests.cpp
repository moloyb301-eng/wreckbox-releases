// Model tests: helper functions behave like the original source, and JSON round trips keep every field (including ones this
// build doesn't model). With WRECKBOX_TEST_LIBRARY set, also round-trips a COPY of a real library folder.
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "model/model.h"
#include "model/paths.h"
#include "model/settings.h"

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

static void normalising() {
    CHECK(wb::normalized("RÜFÜS DU SOL") == "rufus du sol", "%s", wb::normalized("RÜFÜS DU SOL").c_str());
    CHECK(wb::normalized("Beyoncé – Halo (Live)") == "beyonce halo live");
    CHECK(wb::normalized("U\xCC\x88" "ber") == "uber");  // "U" + combining diaeresis, as macOS stores it
    CHECK(wb::normalized("  Fred again..  ") == "fred again");
    CHECK(wb::normalized("AC/DC & Co") == "ac dc co");
}

static void file_names() {
    CHECK(wb::safe_file_name("AC/DC - Back: In \"Black\"?") == "AC_DC - Back_ In _Black__");
    CHECK(wb::safe_file_name("  Song...  ") == "Song");
    CHECK(wb::safe_file_name(std::string(300, 'x')).size() == 180);
}

static void library_round_trip() {
    const json in = {
        {"builtAt", "2026-01-02T03:04:05Z"},
        {"spotifyUser", "me"},
        {"fromMac", 42},
        {"tracks",
         {{{"id", "USRC1"}, {"artists", {"Skrillex", "BEAM"}}, {"title", "Selecta"}, {"isrc", "USRC1"}, {"spotifyIDs", {"abc"}},
           {"durationMs", 190000}, {"playlists", {"Bangers"}}, {"fileName", "Skrillex, BEAM - Selecta"}, {"status", "missing"},
           {"macOnly", "keep me"}}}},
        {"playlists", {{{"name", "Bangers"}, {"collaborative", false}, {"trackIDs", {"USRC1"}}}}},
    };
    const auto lib = wb::Library::from_json(in);
    CHECK(lib.tracks.size() == 1 && lib.tracks[0].artist() == "Skrillex, BEAM");
    CHECK(lib.tracks[0].duration_ms == 190000);
    const json out = lib.to_json();
    CHECK(out == in, "\nin:  %s\nout: %s", in.dump().c_str(), out.dump().c_str());
}

static void state_round_trip() {
    const json in = {
        {"tracks", {{"USRC1", {{"status", "downloaded"}, {"localPath", "C:\\x.mp3"}, {"source", "scan"}, {"updatedAt", "2026-01-02T03:04:05Z"}}},
                    {"USRC2", {{"status", "weird"}, {"updatedAt", "2026-01-02T03:04:05Z"}}}}},
        {"log", {{{"id", "a-1"}, {"date", "2026-01-02T03:04:05Z"}, {"event", "found"}, {"detail", "d"}, {"trackID", "USRC1"}}}},
        {"genreOverrides", {{"USRC1", "House"}}},
        {"scanFolders", {"C:\\Music"}},
        {"downloadPriority", json::array()},
        {"priorityOnly", true},
    };
    const auto s = wb::AppState::from_json(in);
    CHECK(s.tracks.at("USRC1").status == wb::TrackStatus::downloaded);
    CHECK(s.tracks.at("USRC2").status == wb::TrackStatus::missing);  // unknown status reads as missing, like the original app
    json expected = in;
    expected["tracks"]["USRC2"]["status"] = "missing";
    CHECK(s.to_json() == expected, "\n%s", s.to_json().dump().c_str());

    wb::AppState big;
    for (int i = 0; i < 6000; ++i) big.log.push_back(wb::LogEntry::make("e", std::to_string(i)));
    const json trimmed = big.to_json();
    CHECK(trimmed["log"].size() == 5000 && trimmed["log"][0]["detail"] == "1000");
    CHECK(big.log[0].id != big.log[1].id);
}

static void analysis_names() {
    // Reads both spellings; writes the Mac app's (keyConfidence, loudnessLUFS).
    const auto a = wb::FileAnalysis::from_json({{"path", "p"}, {"sizeBytes", 10.0}, {"keyStrength", 0.7}, {"loudnessLufs", -8.2}, {"bpmConfidence", 0.1}});
    CHECK(a.size_bytes == 10 && a.key_strength == 0.7 && a.loudness_lufs == -8.2);
    const json j = a.to_json();
    CHECK(j["keyConfidence"] == 0.7 && j["loudnessLUFS"] == -8.2 && !j.contains("keyStrength") && !j.contains("loudnessLufs"), "%s",
          j.dump().c_str());
    CHECK(j["bpmAmbiguous"] == true && j["engine"] == "wreckbox" && j.contains("analyzedAt"));

    const json engine = {{"bpm", 128.0}, {"bpmConfidence", 0.5}, {"bpmAlternate", nullptr}, {"bpmCandidates", {64.0}}, {"key", "A minor"},
                         {"camelot", "8A"}, {"keyStrength", 0.8}, {"keyAgreement", 3}, {"energy", 0.6}, {"loudnessLufs", -7.0}, {"durationSec", 300.0}};
    const auto e = wb::FileAnalysis::from_engine(engine, "C:\\t.mp3", 123, "2026-01-02T03:04:05Z", "USRC1");
    CHECK(e.bpm == 128.0 && !e.bpm_alternate && e.key_agreement == 3 && e.library_track_id == "USRC1" && e.extra.empty());
}

static void times() {
    CHECK(wb::iso_seconds(0) == "1970-01-01T00:00:00Z", "%s", wb::iso_seconds(0).c_str());
    const auto now = wb::iso_seconds_now();
    CHECK(now.size() == 20 && now.back() == 'Z', "%s", now.c_str());
}

static void settings_round_trip() {
    const fs::path dir = fs::temp_directory_path() / "wreckbox-model-tests";
    fs::remove_all(dir);
    wb::paths::init(dir);
    wb::Settings::load();  // no file → defaults
    CHECK(wb::Settings::current().organise_downloads && wb::Settings::current().dropbox_folder == "/Music");
    wb::paths::write_atomic(wb::paths::settings_file(), R"({"reporterName":"Ana","shareRemotely":true,"futureKey":[1,2]})");
    wb::Settings::load();
    CHECK(wb::Settings::current().reporter_name == "Ana" && wb::Settings::current().share_remotely);
    wb::Settings::current().desktop_pair_token = "tok";
    wb::Settings::current().save();
    const json saved = json::parse(*wb::paths::read_file(wb::paths::settings_file()));
    CHECK(saved["futureKey"] == json({1, 2}) && saved["desktopPairToken"] == "tok" && saved["pairToken"].is_null());
    fs::remove_all(dir);
}

// Optional: a COPY of a real library (never the original): library.json / state.json / _cache/analysis.json.
static void real_library() {
    const char* src = std::getenv("WRECKBOX_TEST_LIBRARY");
    if (!src || !*src) {
        std::puts("(skipping real-library round trip: WRECKBOX_TEST_LIBRARY not set)");
        return;
    }
    for (const char* name : {"library.json", "state.json", "_cache/analysis.json"}) {
        const auto text = wb::paths::read_file(fs::path(src) / name);
        if (!text) continue;
        const json in = json::parse(*text);
        json out;
        if (std::string(name) == "library.json") out = wb::Library::from_json(in).to_json();
        else if (std::string(name) == "state.json") out = wb::AppState::from_json(in).to_json();
        else {
            out = json::object();
            for (const auto& [k, v] : in.items()) out[k] = wb::FileAnalysis::from_json(v).to_json();
        }
        // Everything except fields the original source also rewrites (analyzedAt, the alias names, trimmed log) survives.
        size_t lost = 0;
        if (in.is_object() && std::string(name) != "_cache/analysis.json")
            for (const auto& [k, v] : in.items())
                if (!out.contains(k)) ++lost;
        CHECK(lost == 0, "%s lost %zu top-level keys", name, lost);
        std::printf("round-tripped %s (%zu bytes)\n", name, text->size());
    }
}

int main() {
    normalising();
    file_names();
    library_round_trip();
    state_round_trip();
    analysis_names();
    times();
    settings_round_trip();
    real_library();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all model tests passed");
    return 0;
}
