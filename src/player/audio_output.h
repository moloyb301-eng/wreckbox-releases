// Where decoded audio goes. libVLC decodes (and applies the equalizer / normalizer) and hands interleaved stereo float
// samples to a Sink; AudioOutput is the real one — it buffers them, plays them through miniaudio (WASAPI, follows the
// default device when headphones are unplugged) and keeps the last moments of what's audible for the visualizer.
// Tests use their own Sink, so they never make a sound.
#pragma once
#include <atomic>
#include <chrono>
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

    // The visualizers' tap: a mono mix of every frame sent to the device (silence included, so its index runs with the
    // device's clock), before volume, 1 s deep. Index i = the i-th frame since the device first started.
    using Clock = std::chrono::steady_clock;
    // The tap index of the frame coming out of the speakers at `at`: what was last sent, minus what's still queued in
    // the device, plus the time since. Pictures drawn for `at` should show the audio up to here.
    int64_t heard_index(Clock::time_point at) const;
    // The `n` tap samples just before index `end`, oldest first (zeros for any part outside the tap).
    void tap_until(int64_t end, float* out, size_t n) const;
    int64_t latency_frames() const { return latency_; }
    void set_latency_frames(int64_t frames) { latency_ = frames; }  // measured from the device on start; tests set it

    // Called by the audio device (miniaudio's thread). render_at takes the callback's time (tests pass their own).
    void render(float* out, unsigned frames) { render_at(out, frames, Clock::now()); }
    void render_at(float* out, unsigned frames, Clock::time_point now);

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
    std::vector<float> tap_;  // mono ring of what the device was sent
    size_t tap_at_ = 0;
    int64_t tap_written_ = 0;           // samples ever written to tap_
    Clock::time_point sent_at_{};       // when the last device callback ran (tap_written_ was current then)
    std::atomic<int64_t> latency_{0};   // frames queued in the device ahead of the one being heard
};

// Is Windows' default speaker a Bluetooth device? Its radio adds ~150–250 ms that WASAPI doesn't report, so the
// visualizers draw that much later. Needs COM on the calling thread.
bool default_output_is_bluetooth();

}  // namespace wb::player
