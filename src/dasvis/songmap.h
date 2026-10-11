// DasVis's song map (docs/DASVIS-PLAN.md §6): what's in a song and when, worked out once from the whole file, so the
// pictures can anticipate (tighten through a build-up, let go on the drop's first beat) instead of only reacting.
//
// One streaming pass (MapBuilder): STFT (2,048 / 512 at 22.05 kHz) → harmonic–percussive separation (medians over a
// sliding window of 17 frames and 17 bins) → five pseudo-stems (kick, snare, hats from the percussive part; bass, lead
// from the harmonic) → their envelopes and onsets. Then, from those per-frame summaries: the tempo (the library's BPM,
// or the onsets' autocorrelation), a beat grid (dynamic programming, Ellis 2007), downbeats (the beat offset where the
// bass and the harmony change), 16-bar phrases, sections (self-similarity novelty plus energy changes, snapped to
// 4-bar boundaries, labelled from energy and onset density), drops and the quiet bars before them.
// Memory: a few MB for a whole song (the samples stream through; nothing keeps the song). About 60 KB on disk.
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace wb::dasvis {

enum class Section : uint8_t { intro, verse, build, drop, breakdown, chorus, outro };
const char* section_name(Section s);  // "Intro", "Build-up", …

struct SongMap {
    static constexpr uint32_t kVersion = 2;  // a better analysis bumps it, and old maps are made again (2: parts start on a
                                             // clear step in loudness)
    static constexpr double kRate = 22050, kHop = 512;
    static constexpr double kFps = kRate / kHop;  // envelope frames a second (~43)
    enum Stem { kick, snare, hats, bass, lead };

    double duration = 0, bpm = 0;
    std::vector<double> beats;  // seconds
    int downbeat = 0;           // the index of the first downbeat in `beats` (0–3)
    struct Part {
        double start = 0, end = 0;
        Section kind = Section::verse;
        float energy = 0;  // 0–1, relative to the loudest part
    };
    std::vector<Part> parts;    // the whole song, in order
    std::vector<double> drops;  // when each drop lands
    std::vector<double> gaps;   // for each drop: when its quiet bars start, or −1
    std::array<std::vector<uint8_t>, 5> env;   // per frame, each stem: 0–255
    std::array<std::vector<double>, 3> hits;   // kick, snare, hats onsets (seconds)

    // Where the song is at `t` seconds.
    struct State {
        double beats = 0;         // continuous: beat index + phase
        double beat_phase = 0, bar_phase = 0, phrase_phase = 0;  // 0→1 across a beat / bar / 16-bar phrase
        long bar = 0;             // from the first downbeat
        Section section = Section::verse;
        float section_t = 0;      // 0→1 through it
        float build = 0;          // 0 outside a build-up; rises to 1 at the drop
        float drop = 0;           // 1 on the drop's first beat, gone a bar later
        float to_drop = 99;       // bars until the next drop (99: none ahead)
        bool gap = false;         // in the quiet bars just before a drop
        float energy = 0;         // the part's
        std::array<float, 5> stems{};  // the envelopes, 0–1
    };
    State at(double t) const;
    double bar_seconds() const { return bpm > 0 ? 240 / bpm : 2; }
    // Onsets of `stem` (kick, snare, hats) in (t0, t1].
    int hits_between(Stem stem, double t0, double t1) const;

    bool save(const std::filesystem::path& file) const;
    static std::optional<SongMap> load(const std::filesystem::path& file);  // nullopt: missing, damaged or old version
};

// Builds a map from 22.05 kHz mono samples pushed in any chunks (as the decoder delivers them).
class MapBuilder {
public:
    explicit MapBuilder(double known_bpm = 0);  // the library's analysed tempo, 0 if unknown
    ~MapBuilder();
    MapBuilder(const MapBuilder&) = delete;
    MapBuilder& operator=(const MapBuilder&) = delete;
    void push(const float* samples, size_t n);
    SongMap finish();

private:
    struct Impl;
    Impl* p_;
};

// Decodes `audio` (streaming) and builds its map. nullopt with `error` set if the file can't be read or is too short.
std::optional<SongMap> analyse_file(const std::filesystem::path& audio, double known_bpm, std::string* error = nullptr);

// The cache file for a song: named from the track id and the file's size and date, so a changed file gets a new map.
std::filesystem::path map_cache_file(const std::filesystem::path& dir, const std::string& track_id, const std::filesystem::path& audio);

}  // namespace wb::dasvis
