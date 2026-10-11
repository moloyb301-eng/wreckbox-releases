// DasVis video export: a song rendered offline into an MP4 (H.264 video, AAC audio), with the song's own audio. Every
// frame is exact (the song map gives the beat, parts and drops for any time), and the picture is always full quality
// and full size: a slower PC just takes longer. Media Foundation's encoders (part of Windows; the graphics card's when
// it has one) — no new dependency. Runs on its own Direct3D device, so it can work while the app is used.
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "dasvis/songmap.h"

namespace wb::dasvis {

struct VideoOptions {
    int width = 1920, height = 1080, fps = 60;
    int intensity = 1;                   // 0 chill, 1 normal, 2 insane
    double bpm = 0;                      // the library's tempo, if known
    std::optional<std::string> camelot;  // the song's key
    std::string song_id;                 // the seed (the same look as in the app)
    std::vector<uint8_t> cover;          // BGRA, cover_w × cover_h; empty: none
    int cover_w = 0, cover_h = 0;
    std::vector<uint8_t> title;          // the title card (DasVis::title_target's format), title_w × title_h; empty: none
    int title_w = 0, title_h = 0, title_letters = 0;
    float title_share = 0.6f;
    std::map<std::string, float> taste;  // your likes for the song's genre
    bool software = false;               // render on the CPU (WARP): for tests
};

// Renders `audio` with DasVis into `out` (.mp4). `map`: the song's map if it's at hand, else it's analysed first.
// `progress` gets 0–1 now and then; returning false from it cancels (the part-written file is deleted). Returns "" when
// done, else what went wrong.
std::string export_video(const std::filesystem::path& audio, const std::filesystem::path& out, const VideoOptions& opt, const SongMap* map,
                         const std::function<bool(double)>& progress);

}  // namespace wb::dasvis
