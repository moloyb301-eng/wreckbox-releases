// DasVis's colours. From the album cover: five colours found by k-means in Oklab (a colour space where distances match
// what we see) on a 64 × 64 thumbnail, then spread from dark to light so any cover gives a usable palette. Or, without
// a cover (or with a grey one), five colours around the song's key on the Camelot wheel.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace wb::dasvis {

using Rgb = std::array<float, 3>;      // linear RGB, 0–1
using Palette = std::array<Rgb, 5>;    // dark → light

// The cover's palette, or nullopt when the cover is (nearly) grey: its colours wouldn't carry a picture.
// `bgra`: opaque BGRA pixels, as WIC decodes a cover.
std::optional<Palette> cover_palette(const uint8_t* bgra, int w, int h);

// Five colours around the key ("8A"); minor keys cooler and softer. Without a key, around a hue from the song's seed.
Palette key_palette(const std::optional<std::string>& camelot, float seed);
Rgb key_tint(const std::optional<std::string>& camelot, float seed);

}  // namespace wb::dasvis
