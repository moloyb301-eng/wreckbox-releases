#include "player/audio_output.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING  // libVLC decodes; miniaudio only plays
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include <miniaudio.h>

namespace wb::player {

struct AudioOutput::Device {
    ma_device device{};
};

namespace {

constexpr size_t kTapSize = 4096;

void on_data(ma_device* d, void* out, const void*, ma_uint32 frames) {
    static_cast<AudioOutput*>(d->pUserData)->render(static_cast<float*>(out), frames);
}

}  // namespace

AudioOutput::AudioOutput(bool open_device) : dev_(std::make_unique<Device>()), ring_(size_t(kRate) * kChannels / 4), tap_(kTapSize, 0.f) {
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = kChannels;
    cfg.sampleRate = kRate;  // miniaudio converts to the device's own rate if it differs
    cfg.dataCallback = on_data;
    cfg.pUserData = this;
    cfg.performanceProfile = ma_performance_profile_conservative;  // larger periods: fewer wake-ups on weak PCs
    // Started on first play and stopped while paused / stopped, so an idle WreckBox costs no CPU.
    device_ok_ = open_device && ma_device_init(nullptr, &cfg, &dev_->device) == MA_SUCCESS;
    paused_ = true;
}

AudioOutput::~AudioOutput() {
    {
        std::lock_guard lock(m_);
        paused_ = true;
        fill_ = 0;
    }
    space_.notify_all();
    if (device_ok_) ma_device_uninit(&dev_->device);
}

void AudioOutput::play(const float* samples, unsigned frames) {
    std::unique_lock lock(m_);
    size_t n = size_t(frames) * kChannels;
    const size_t cap = ring_.size();
    while (n > 0) {
        // Back-pressure: libVLC can run ahead of real time; wait for room rather than dropping audio.
        if (!space_.wait_for(lock, std::chrono::milliseconds(500), [&] { return fill_ < cap; })) return;
        const size_t chunk = std::min(n, cap - fill_);
        for (size_t i = 0; i < chunk; ++i) ring_[(read_ + fill_ + i) % cap] = samples[i];
        fill_ += chunk;
        samples += chunk;
        n -= chunk;
    }
}

void AudioOutput::pause(bool paused) {
    {
        std::lock_guard lock(m_);
        if (paused_ == paused) return;
        paused_ = paused;
    }
    space_.notify_all();
    if (!device_ok_) return;
    if (paused) ma_device_stop(&dev_->device);
    else ma_device_start(&dev_->device);
}

void AudioOutput::flush() {
    {
        std::lock_guard lock(m_);
        fill_ = 0;
        played_ = 0;
    }
    space_.notify_all();
    std::lock_guard t(tap_m_);
    std::fill(tap_.begin(), tap_.end(), 0.f);
}

void AudioOutput::drain() {
    // Wait (bounded) until what's buffered has been played, so the next track doesn't cut the tail off.
    std::unique_lock lock(m_);
    space_.wait_for(lock, std::chrono::seconds(3), [&] { return fill_ == 0 || paused_; });
}

void AudioOutput::render(float* out, unsigned frames) {
    const size_t n = size_t(frames) * kChannels;
    size_t got = 0;
    {
        std::lock_guard lock(m_);
        if (!paused_) {
            const size_t cap = ring_.size();
            got = std::min(n, fill_);
            for (size_t i = 0; i < got; ++i) out[i] = ring_[(read_ + i) % cap];
            read_ = (read_ + got) % cap;
            fill_ -= got;
            played_ += int64_t(got / kChannels);
        }
    }
    space_.notify_all();
    std::fill(out + got, out + n, 0.f);  // underrun / paused: silence

    // Visualizer tap: what's actually heard, before volume, so the picture doesn't shrink when you turn it down.
    {
        std::lock_guard t(tap_m_);
        for (size_t f = 0; f < got / kChannels; ++f) {
            tap_[tap_at_] = 0.5f * (out[f * 2] + out[f * 2 + 1]);
            tap_at_ = (tap_at_ + 1) % kTapSize;
        }
        tap_written_ += got / kChannels;
    }
    const float v = muted_ ? 0.f : volume_ * volume_ * volume_;  // perceptual volume curve
    if (v != 1.f)
        for (size_t i = 0; i < got; ++i) out[i] *= v;
}

size_t AudioOutput::take_tap(uint64_t& cursor, float* out, size_t max) const {
    std::lock_guard t(tap_m_);
    if (cursor > tap_written_) cursor = tap_written_;
    size_t n = size_t(std::min<uint64_t>(tap_written_ - cursor, kTapSize + 1));
    if (n > kTapSize) cursor = tap_written_ - (n = std::min(max, kTapSize));  // missed more than the ring: newest only
    else n = std::min(n, max);
    for (size_t i = 0; i < n; ++i) out[i] = tap_[(tap_at_ + kTapSize - size_t(tap_written_ - cursor) + i) % kTapSize];
    cursor += n;
    return n;
}

void AudioOutput::tap(float* out, size_t n) const {
    n = std::min(n, kTapSize);
    std::lock_guard t(tap_m_);
    for (size_t i = 0; i < n; ++i) out[i] = tap_[(tap_at_ + kTapSize - n + i) % kTapSize];
}

}  // namespace wb::player
