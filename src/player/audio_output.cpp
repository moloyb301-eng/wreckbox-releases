#include "player/audio_output.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
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

constexpr size_t kTapSize = kRate;  // 1 s: the device's queue plus a Bluetooth delay, with room to spare

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
    // 20 ms periods: the visualizers get fresh audio every 20 ms and the device holds at most ~60 ms. (miniaudio's
    // "conservative" profile is 100 ms × 3: the pictures saw audio in 100 ms lumps, up to 300 ms before it was heard,
    // and missed most kicks.) 50 wake-ups a second is still nothing for a weak PC.
    cfg.periodSizeInMilliseconds = 20;
    cfg.periods = 3;
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
    if (paused) {
        ma_device_stop(&dev_->device);
    } else if (ma_device_start(&dev_->device) == MA_SUCCESS) {
        // What the device queues ahead of the frame being heard, in our 48 kHz frames (WASAPI may run at another rate).
        const auto& pb = dev_->device.playback;
        if (pb.internalSampleRate)
            latency_ = int64_t(pb.internalPeriodSizeInFrames) * pb.internalPeriods * kRate / pb.internalSampleRate;
    }
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

void AudioOutput::render_at(float* out, unsigned frames, Clock::time_point now) {
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

    // Visualizer tap: every frame sent (silence on an underrun too, so the tap keeps the device's time), before volume,
    // so the picture doesn't shrink when you turn it down.
    {
        std::lock_guard t(tap_m_);
        for (size_t f = 0; f < frames; ++f) {
            tap_[tap_at_] = 0.5f * (out[f * 2] + out[f * 2 + 1]);
            tap_at_ = (tap_at_ + 1) % kTapSize;
        }
        tap_written_ += frames;
        sent_at_ = now;
    }
    const float v = muted_ ? 0.f : volume_ * volume_ * volume_;  // perceptual volume curve
    if (v != 1.f)
        for (size_t i = 0; i < got; ++i) out[i] *= v;
}

int64_t AudioOutput::heard_index(Clock::time_point at) const {
    std::lock_guard t(tap_m_);
    if (!tap_written_) return 0;
    const double since = std::chrono::duration<double>(at - sent_at_).count();
    const int64_t i = tap_written_ - latency_ + int64_t(since * kRate);
    // Never past what was sent (paused: the device stopped, the clock with it), never older than the tap.
    return std::clamp(i, std::max<int64_t>(0, tap_written_ - int64_t(kTapSize)), tap_written_);
}

void AudioOutput::tap_until(int64_t end, float* out, size_t n) const {
    std::lock_guard t(tap_m_);
    const int64_t oldest = tap_written_ - int64_t(kTapSize);
    for (size_t k = 0; k < n; ++k) {
        const int64_t i = end - int64_t(n) + int64_t(k);
        out[k] = i < 0 || i < oldest || i >= tap_written_ ? 0.f : tap_[(tap_at_ + kTapSize - size_t(tap_written_ - i)) % kTapSize];
    }
}

bool default_output_is_bluetooth() {
    using Microsoft::WRL::ComPtr;
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    ComPtr<IPropertyStore> props;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
        FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) || FAILED(dev->OpenPropertyStore(STGM_READ, &props)))
        return false;
    PROPVARIANT v;
    PropVariantInit(&v);
    bool bt = false;
    // PKEY_Device_EnumeratorName (devpkey.h), spelled out to keep the device-property headers out of this file.
    const PROPERTYKEY enumerator_name{{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 24};
    if (SUCCEEDED(props->GetValue(enumerator_name, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
        bt = std::wstring(v.pwszVal).starts_with(L"BTH");  // BTHENUM (A2DP), BTHHFENUM (hands-free)
    PropVariantClear(&v);
    return bt;
}

}  // namespace wb::player
