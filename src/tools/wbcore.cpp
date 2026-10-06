// wbcore — command-line access to the WreckBox engine (same commands and output as the Rust wbcore).
//
//   wbcore analyze FILE…             one JSON line per file
//   wbcore tags FILE…                read tags as JSON
//   wbcore write-tags < jobs.json    [{"path": …, title, artists, …}] → one JSON result per job
//   wbcore version
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <string>

#include "engine/analysis.h"
#include "engine/decode.h"
#include "engine/engine.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string utf8(const fs::path& p) {
    auto u = p.u8string();
    return {u.begin(), u.end()};
}

void print(const json& j) {
    const std::string s = j.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    const std::wstring cmd = argc > 1 ? argv[1] : L"";
    if (cmd == L"analyze" || cmd == L"tags") {
        for (int i = 2; i < argc; ++i) {
            const fs::path p = argv[i];
            const json r = cmd == L"analyze" ? wb::analyze_file(p) : wb::read_tags_json(p);
            if (r.contains("error")) print({{"path", utf8(p)}, {"error", r["error"]}});
            else print({{"path", utf8(p)}, {cmd == L"analyze" ? "result" : "tags", r}});
        }
        return 0;
    }
    if (cmd == L"write-tags") {
        const std::string input{std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
        const json jobs = json::parse(input, nullptr, false);
        if (!jobs.is_array()) {
            std::fputs("write-tags expects a JSON array of jobs on stdin\n", stderr);
            return 2;
        }
        for (const auto& job : jobs) {
            const std::string path = job.is_object() ? job.value("path", "") : "";
            const json r = wb::write_tags_json(job);
            if (r.contains("error")) print({{"path", path}, {"ok", false}, {"error", r["error"]}});
            else print({{"path", path}, {"ok", true}});
        }
        return 0;
    }
    if (cmd == L"bench") {  // where analysis time goes: decode / tempo / key, per file
        for (int i = 2; i < argc; ++i) {
            using clock = std::chrono::steady_clock;
            auto ms = [](clock::time_point a, clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
            try {
                const auto t0 = clock::now();
                const wb::Audio audio = wb::decode_mono(argv[i]);
                const auto t1 = clock::now();
                wb::tempo(audio.samples, audio.rate);
                const auto t2 = clock::now();
                wb::key(audio.samples, audio.rate);
                const auto t3 = clock::now();
                print({{"path", utf8(argv[i])}, {"seconds", audio.duration}, {"decodeMs", ms(t0, t1)}, {"tempoMs", ms(t1, t2)}, {"keyMs", ms(t2, t3)}});
            } catch (const std::exception& e) {
                print({{"path", utf8(argv[i])}, {"error", e.what()}});
            }
        }
        return 0;
    }
    if (cmd == L"version") {
        print({{"version", wb::engine_version()}});
        return 0;
    }
    std::fputs("usage: wbcore analyze|tags FILE... | write-tags < jobs.json | version\n", stderr);
    return 2;
}
