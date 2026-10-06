// libVLC — VLC's playback engine — behind a small interface. It plays anything VLC plays (files, http(s) streams,
// internet radio, playlists), applies VLC's equalizer and volume normalizer, and delivers the decoded audio to a Sink.
//
// libVLC calls its event callbacks on its own threads and must not be called back from inside them, so events are
// handed to `on_event`, which the owner forwards to its own thread.
#pragma once
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "player/audio_output.h"

struct libvlc_instance_t;
struct libvlc_media_player_t;
struct libvlc_media_t;
struct libvlc_equalizer_t;

namespace wb::player {

enum class Event { ended, error, length_changed, meta_changed, playing, paused };

struct Equalizer {
    bool enabled = false;
    int preset = -1;           // VLC preset index, or -1 for custom bands
    // VLC's preamp, −20…20. VLC treats 12 as unity gain (measured: 0 → −12 dB, 20 → about +6 dB), so the real
    // gain is preamp − 12 — what the UI shows.
    float preamp = 12;
    std::vector<float> bands;  // dB per band, −20…20 (10 bands)
};

class VlcEngine {
public:
    // VLC's plugins are in "plugins" next to libvlccore.dll (where libVLC looks). Never throws; check ok().
    explicit VlcEngine(Sink& sink);
    ~VlcEngine();
    VlcEngine(const VlcEngine&) = delete;
    VlcEngine& operator=(const VlcEngine&) = delete;

    bool ok() const { return mp_ != nullptr; }
    const std::string& error() const { return error_; }
    static std::string version();  // libVLC's version; needs no engine

    std::function<void(Event)> on_event;

    // A local file (UTF-8 path) or a URL (http, https, …). Starts playing. False if VLC couldn't open it.
    bool open(const std::string& location, bool normalize, int64_t start_ms = 0);
    void play();
    void pause(bool paused);
    void stop();
    void seek(int64_t ms);
    void set_rate(float rate);  // tests use this to play clips faster than real time
    int64_t time() const;       // ms
    int64_t length() const;     // ms; 0 for live streams
    bool seekable() const;
    bool playing() const;
    std::string now_playing() const;  // internet radio: the current song from the stream's metadata
    std::string meta_title() const, meta_artist() const;

    void set_equalizer(const Equalizer& eq);
    static std::vector<std::string> preset_names();
    static std::vector<float> band_frequencies();
    static Equalizer preset(int index);  // its preamp and band values, enabled

private:
    libvlc_media_t* make_media(const std::string& location) const;

    Sink& sink_;
    libvlc_instance_t* vlc_ = nullptr;
    libvlc_media_player_t* mp_ = nullptr;
    libvlc_equalizer_t* eq_ = nullptr;
    mutable std::mutex media_m_;
    libvlc_media_t* media_ = nullptr;
    std::string error_;
};

bool is_url(const std::string& location);

}  // namespace wb::player
