#include "player/vlc_engine.h"

#include <windows.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>

#include <vlc/vlc.h>

#include "model/model.h"

namespace wb::player {
namespace {

// libVLC's audio callbacks → the Sink. VLC 3's amem only delivers S16N whatever format is asked for (float output came
// in VLC 4), so convert here; the EQ and normalizer have already run in float inside VLC.
void cb_play(void* data, const void* samples, unsigned count, int64_t) {
    thread_local std::vector<float> buf;
    const auto* s = static_cast<const int16_t*>(samples);
    buf.resize(size_t(count) * kChannels);
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = s[i] * (1.0f / 32768.0f);
    static_cast<Sink*>(data)->play(buf.data(), count);
}
void cb_pause(void* data, int64_t) { static_cast<Sink*>(data)->pause(true); }
void cb_resume(void* data, int64_t) { static_cast<Sink*>(data)->pause(false); }
void cb_flush(void* data, int64_t) { static_cast<Sink*>(data)->flush(); }
void cb_drain(void* data) { static_cast<Sink*>(data)->drain(); }

void on_vlc_event(const libvlc_event_t* e, void* data) {
    auto* self = static_cast<VlcEngine*>(data);
    if (!self->on_event) return;
    switch (e->type) {
        case libvlc_MediaPlayerEndReached: self->on_event(Event::ended); break;
        case libvlc_MediaPlayerEncounteredError: self->on_event(Event::error); break;
        case libvlc_MediaPlayerLengthChanged: self->on_event(Event::length_changed); break;
        case libvlc_MediaPlayerPlaying: self->on_event(Event::playing); break;
        case libvlc_MediaPlayerPaused: self->on_event(Event::paused); break;
        case libvlc_MediaMetaChanged: self->on_event(Event::meta_changed); break;
        default: break;
    }
}

// WRECKBOX_VLC_LOG=1: VLC's debug log to stderr (which modules load, why a stream fails).
void on_vlc_log(void*, int level, const libvlc_log_t*, const char* fmt, va_list args) {
    char line[1024];
    std::vsnprintf(line, sizeof line, fmt, args);
    std::fprintf(stderr, "vlc[%d] %s\n", level, line);
}

std::string meta(libvlc_media_t* m, libvlc_meta_t which) {
    if (!m) return "";
    char* v = libvlc_media_get_meta(m, which);
    std::string out = v ? v : "";
    libvlc_free(v);
    return out;
}

}  // namespace

bool is_url(const std::string& location) {
    const auto p = location.find("://");
    return p != std::string::npos && p > 1 && p < 12;  // "http://", "https://", "mms://" … but not "C:\"
}

VlcEngine::VlcEngine(Sink& sink) : sink_(sink) {
    // Core options only: libVLC refuses to start on options that belong to plugins we don't ship (e.g. --no-lua).
    // Resampler: VLC otherwise ranks "ugly" (nearest sample) first, which also leaves one garbage sample per block.
    const char* args[] = {"--no-video", "--quiet", "--no-osd", "--no-stats", "--ignore-config", "--audio-resampler=speex_resampler"};
    vlc_ = libvlc_new(int(std::size(args)), args);
    if (!vlc_) {
        error_ = "VLC's engine didn't start (its files next to WreckBox may be missing).";
        return;
    }
    if (const char* v = std::getenv("WRECKBOX_VLC_LOG"); v && *v == '1') libvlc_log_set(vlc_, on_vlc_log, nullptr);
    mp_ = libvlc_media_player_new(vlc_);
    if (!mp_) {
        error_ = "VLC's engine couldn't create a player.";
        return;
    }
    libvlc_audio_set_format(mp_, "S16N", kRate, kChannels);
    libvlc_audio_set_callbacks(mp_, cb_play, cb_pause, cb_resume, cb_flush, cb_drain, &sink_);
    libvlc_event_manager_t* em = libvlc_media_player_event_manager(mp_);
    for (const auto type : {libvlc_MediaPlayerEndReached, libvlc_MediaPlayerEncounteredError, libvlc_MediaPlayerLengthChanged,
                            libvlc_MediaPlayerPlaying, libvlc_MediaPlayerPaused})
        libvlc_event_attach(em, type, on_vlc_event, this);
    eq_ = libvlc_audio_equalizer_new();
}

VlcEngine::~VlcEngine() {
    if (mp_) {
        libvlc_media_player_stop(mp_);
        libvlc_media_player_release(mp_);
    }
    if (media_) libvlc_media_release(media_);
    if (eq_) libvlc_audio_equalizer_release(eq_);
    if (vlc_) libvlc_release(vlc_);
}

std::string VlcEngine::version() { return libvlc_get_version(); }

libvlc_media_t* VlcEngine::make_media(const std::string& location) const {
    if (is_url(location)) return libvlc_media_new_location(vlc_, location.c_str());
    // VLC turns a local path into a file:/// address and only understands Windows paths with backslashes.
    std::string path = location;
    std::replace(path.begin(), path.end(), '/', '\\');
    return libvlc_media_new_path(vlc_, path.c_str());
}

bool VlcEngine::open(const std::string& location, bool normalize, int64_t start_ms) {
    if (!ok()) return false;
    libvlc_media_t* m = make_media(location);
    if (!m) return false;
    if (normalize) libvlc_media_add_option(m, ":audio-filter=normvol");
    if (start_ms > 0) libvlc_media_add_option(m, (":start-time=" + std::to_string(double(start_ms) / 1000.0)).c_str());
    libvlc_event_attach(libvlc_media_event_manager(m), libvlc_MediaMetaChanged, on_vlc_event, this);
    stop();
    libvlc_media_player_set_media(mp_, m);
    {
        std::lock_guard lock(media_m_);
        if (media_) libvlc_media_release(media_);
        media_ = m;
    }
    return libvlc_media_player_play(mp_) == 0;
}

void VlcEngine::play() {
    if (ok()) libvlc_media_player_set_pause(mp_, 0);
}

void VlcEngine::pause(bool paused) {
    if (ok()) libvlc_media_player_set_pause(mp_, paused ? 1 : 0);
}

void VlcEngine::stop() {
    if (!ok()) return;
    sink_.flush();  // unblocks libVLC if it's waiting for buffer room, so stopping can't stall
    libvlc_media_player_stop(mp_);
    sink_.flush();
}

void VlcEngine::seek(int64_t ms) {
    if (ok()) libvlc_media_player_set_time(mp_, std::max<int64_t>(0, ms));
}

void VlcEngine::set_rate(float rate) {
    if (ok()) libvlc_media_player_set_rate(mp_, rate);
}

int64_t VlcEngine::time() const { return ok() ? std::max<int64_t>(0, libvlc_media_player_get_time(mp_)) : 0; }
int64_t VlcEngine::length() const { return ok() ? std::max<int64_t>(0, libvlc_media_player_get_length(mp_)) : 0; }
bool VlcEngine::seekable() const { return ok() && libvlc_media_player_is_seekable(mp_); }
bool VlcEngine::playing() const { return ok() && libvlc_media_player_is_playing(mp_); }

std::string VlcEngine::now_playing() const {
    std::lock_guard lock(media_m_);
    return meta(media_, libvlc_meta_NowPlaying);
}
std::string VlcEngine::meta_title() const {
    std::lock_guard lock(media_m_);
    return meta(media_, libvlc_meta_Title);
}
std::string VlcEngine::meta_artist() const {
    std::lock_guard lock(media_m_);
    return meta(media_, libvlc_meta_Artist);
}

void VlcEngine::set_equalizer(const Equalizer& eq) {
    if (!ok() || !eq_) return;
    if (!eq.enabled) {
        libvlc_media_player_set_equalizer(mp_, nullptr);
        return;
    }
    libvlc_audio_equalizer_set_preamp(eq_, eq.preamp);
    for (unsigned i = 0; i < eq.bands.size() && i < libvlc_audio_equalizer_get_band_count(); ++i)
        libvlc_audio_equalizer_set_amp_at_index(eq_, eq.bands[i], i);
    libvlc_media_player_set_equalizer(mp_, eq_);  // VLC copies the settings; changes apply live
}

std::vector<std::string> VlcEngine::preset_names() {
    std::vector<std::string> out;
    for (unsigned i = 0; i < libvlc_audio_equalizer_get_preset_count(); ++i) out.push_back(libvlc_audio_equalizer_get_preset_name(i));
    return out;
}

std::vector<float> VlcEngine::band_frequencies() {
    std::vector<float> out;
    for (unsigned i = 0; i < libvlc_audio_equalizer_get_band_count(); ++i) out.push_back(libvlc_audio_equalizer_get_band_frequency(i));
    return out;
}

Equalizer VlcEngine::preset(int index) {
    Equalizer e;
    e.enabled = true;
    e.preset = index;
    if (libvlc_equalizer_t* p = libvlc_audio_equalizer_new_from_preset(unsigned(index))) {
        e.preamp = libvlc_audio_equalizer_get_preamp(p);
        for (unsigned i = 0; i < libvlc_audio_equalizer_get_band_count(); ++i) e.bands.push_back(libvlc_audio_equalizer_get_amp_at_index(p, i));
        libvlc_audio_equalizer_release(p);
    }
    return e;
}

}  // namespace wb::player
