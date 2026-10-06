// Where decoded audio goes. libVLC decodes (and applies the equalizer / normalizer) and hands interleaved stereo float
// samples to a Sink; AudioOutput is the real one — it buffers them, plays them through miniaudio (WASAPI, follows the
// default device when headphones are unplugged) and keeps the last moments of what's audible for the visualizer.
// Tests use their own Sink, so they never make a sound.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace wb::player {

inline constexpr unsigned kRate = 48000;  // what libVLC delivers: stereo at 48 kHz (16-bit from VLC, float from here on)
inline constexpr unsigned kChannels = 2;

class Sink {
public:
    virtual ~Sink() = default;
    // Called on libVLC's audio thread. `frames` stereo frames. May block briefly while the buffer is full.
    virtual void play(const float* samples, unsigned frames) = 0;
    virtual void pause(bool paused) = 0;
    virtual void flush() = 0;  // seek / stop: drop what's buffered
    virtual void drain() {}    // end of stream: let the buffer play out
};

class AudioOutput : public Sink {
public:
    explicit AudioOutput(bool open_device = true);  // false: no sound device (tests drive render() themselves)
    ~AudioOutput() override;
    bool ok() const { return device_ok_; }

    void play(const float* samples, unsigned frames) override;
    void pause(bool paused) override;
    void flush() override;
    void drain() override;

    void set_volume(float v) { volume_ = v; }  // 0–1, perceptual (cubed when applied)
    void set_muted(bool m) { muted_ = m; }
    int64_t played_frames() const { return played_; }  // frames sent to the device since the last flush

    // The last `n` (≤ 4096) audible samples as a mono mix, newest last. Cheap; for the visualizer.
    void tap(float* out, size_t n) const;

    // For MilkDrop: the samples heard since `cursor` (start at 0), oldest first, at most `max`; advances the cursor. If
    // more than the ring holds was missed (a long pause), it skips ahead and returns just the newest `min(max, 4096)`.
    size_t take_tap(uint64_t& cursor, float* out, size_t max) const;

    // Called by the audio device (miniaudio's thread).
    void render(float* out, unsigned frames);

private:
    struct Device;
    std::unique_ptr<Device> dev_;
    bool device_ok_ = false;

    mutable std::mutex m_;
    std::condition_variable space_;
    // Interleaved stereo, 0.25 s. VLC runs ~1.3 s ahead of its clock if it can; a small buffer makes it wait, so the
    // equalizer and normalizer (applied inside VLC) are heard within a quarter second. Measured: no frames lost.
    std::vector<float> ring_;
    size_t read_ = 0, fill_ = 0;
    std::atomic<int64_t> played_{0};
    bool paused_ = false;
    std::atomic<float> volume_{0.8f};
    std::atomic<bool> muted_{false};

    mutable std::mutex tap_m_;
    std::vector<float> tap_;  // mono ring of audible samples
    size_t tap_at_ = 0;
    uint64_t tap_written_ = 0;  // samples ever written to tap_
};

}  // namespace wb::player
