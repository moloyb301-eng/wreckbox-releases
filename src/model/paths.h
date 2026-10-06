// Where WreckBox keeps things (port of app/lib/paths.dart, desktop part).
//
//   <USERPROFILE>\Music\WreckBox              library.json, state.json, Tracks\, _inbox\, _cache\, _soulseek\
//   %APPDATA%\local.wreckbox\wreckbox          settings.json, wreckbox.log  (the Flutter build's folder)
#pragma once
#include <filesystem>
#include <optional>
#include <string>

namespace wb::paths {

namespace fs = std::filesystem;

// Call once at startup. `override_root` (tests) puts settings in <root>\_settings like the Dart tests do.
void init(std::optional<fs::path> override_root = std::nullopt);

const fs::path& root();
const fs::path& settings_dir();
const std::optional<fs::path>& downloads();  // none when the library folder was overridden (tests)
// True when the library folder was chosen with --root: a test or demo library, so nothing here should reach the network or
// the real Downloads folder on its own.
bool overridden();

inline fs::path library_file() { return root() / L"library.json"; }
inline fs::path state_file() { return root() / L"state.json"; }
inline fs::path tracks() { return root() / L"Tracks"; }
inline fs::path inbox() { return root() / L"_inbox"; }
inline fs::path cache() { return root() / L"_cache"; }
inline fs::path artwork() { return root() / L"_cache" / L"artwork"; }
inline fs::path analysis_cache() { return root() / L"_cache" / L"analysis.json"; }
inline fs::path soulseek_dir() { return root() / L"_soulseek"; }
inline fs::path settings_file() { return settings_dir() / L"settings.json"; }
inline fs::path app_log() { return settings_dir() / L"wreckbox.log"; }

bool is_audio(const fs::path& p);

// Writes a file atomically (temp file + rename) so a crash never leaves a half-written JSON file. Throws on failure.
void write_atomic(const fs::path& file, const std::string& contents);

// Whole file as bytes/UTF-8 text, or nullopt if it can't be read.
std::optional<std::string> read_file(const fs::path& file);

}  // namespace wb::paths
