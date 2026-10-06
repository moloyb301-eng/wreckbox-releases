// Port of core/src/decode.rs. WAV/AIFF, MP3 and FLAC go through dr_libs (fast single-file decoders; dr_wav also
// covers AIFF, which Media Foundation can't read). AAC/M4A/ALAC go through Media Foundation's Source Reader, which
// is built into Windows but has a high per-packet cost — fine for AAC, ~5x slower than dr_mp3 on MP3. Either way
// the same averaging resampler as the Rust engine brings it to mono at 22,050 Hz.
#include "engine/decode.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <stdexcept>
#include <string>

#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>
#define DR_MP3_IMPLEMENTATION
#include <dr_mp3.h>
#define DR_FLAC_IMPLEMENTATION
#include <dr_flac.h>

using Microsoft::WRL::ComPtr;

namespace wb {
namespace {

// Same state machine as decode.rs: sums source samples into the current output slot and emits their mean
// each time the source position passes the next boundary (simple averaging resampler; fine for analysis).
class Downmixer {
public:
    explicit Downmixer(uint32_t src_rate) : src_rate_(src_rate), ratio_(double(src_rate) / kAnalysisRate) {}

    void push(const float* interleaved, size_t frames, unsigned channels) {
        channels = std::max(channels, 1u);
        for (size_t f = 0; f < frames; ++f) {
            float mono = 0;
            for (unsigned c = 0; c < channels; ++c) mono += interleaved[f * channels + c];
            mono /= float(channels);
            acc_ += mono;
            ++count_;
            ++src_index_;
            if (double(src_index_) >= pos_ + ratio_) {
                out_.push_back(float(acc_ / double(count_)));
                acc_ = 0;
                count_ = 0;
                pos_ += ratio_;
            }
        }
    }

    Audio finish() {
        if (out_.empty()) throw std::runtime_error("no audio decoded");
        return Audio{std::move(out_), kAnalysisRate, double(src_index_) / src_rate_};
    }

private:
    uint32_t src_rate_;
    double ratio_;
    double acc_ = 0;
    size_t count_ = 0;
    double pos_ = 0;
    uint64_t src_index_ = 0;
    std::vector<float> out_;
};

std::string display(const std::filesystem::path& p) {
    auto u = p.u8string();
    return {u.begin(), u.end()};
}

// Pulls float frames from a dr_libs decoder until it runs dry.
template <class Read>
Audio drain(unsigned channels, unsigned rate, Read&& read) {
    if (rate == 0) throw std::runtime_error("unknown sample rate");
    Downmixer mix(rate);
    std::vector<float> buf(4096 * size_t(std::max(channels, 1u)));
    while (const auto got = read(4096, buf.data())) mix.push(buf.data(), size_t(got), channels);
    return mix.finish();
}

Audio decode_wav(const std::filesystem::path& path) {
    drwav wav;
    if (!drwav_init_file_w(&wav, path.c_str(), nullptr)) throw std::runtime_error("unsupported or corrupt audio file");
    struct Close { drwav* w; ~Close() { drwav_uninit(w); } } close{&wav};
    return drain(wav.channels, wav.sampleRate, [&](drwav_uint64 n, float* out) { return drwav_read_pcm_frames_f32(&wav, n, out); });
}

Audio decode_mp3(const std::filesystem::path& path) {
    drmp3 mp3;
    if (!drmp3_init_file_w(&mp3, path.c_str(), nullptr)) throw std::runtime_error("unsupported or corrupt audio file");
    struct Close { drmp3* m; ~Close() { drmp3_uninit(m); } } close{&mp3};
    return drain(mp3.channels, mp3.sampleRate, [&](drmp3_uint64 n, float* out) { return drmp3_read_pcm_frames_f32(&mp3, n, out); });
}

Audio decode_flac(const std::filesystem::path& path) {
    drflac* flac = drflac_open_file_w(path.c_str(), nullptr);
    if (!flac) throw std::runtime_error("unsupported or corrupt audio file");
    struct Close { drflac* f; ~Close() { drflac_close(f); } } close{flac};
    return drain(flac->channels, flac->sampleRate, [&](drflac_uint64 n, float* out) { return drflac_read_pcm_frames_f32(flac, n, out); });
}

// COM + Media Foundation for the calling thread, for the duration of one decode. Both are reference-counted.
struct MfScope {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT mf = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    ~MfScope() {
        if (SUCCEEDED(mf)) MFShutdown();
        if (SUCCEEDED(com)) CoUninitialize();
    }
};

Audio decode_mf(const std::filesystem::path& path) {
    MfScope scope;
    if (FAILED(scope.mf)) throw std::runtime_error("Media Foundation is not available");

    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader)))
        throw std::runtime_error("unsupported or corrupt audio file");
    const DWORD stream = DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (FAILED(reader->SetStreamSelection(stream, TRUE))) throw std::runtime_error("no audio track");

    // Ask for PCM at the decoder's own bit depth (no converter in the pipeline — much faster than asking for float),
    // falling back to float if the decoder only offers that.
    auto request = [&](const GUID& subtype) {
        ComPtr<IMFMediaType> want;
        MFCreateMediaType(&want);
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        want->SetGUID(MF_MT_SUBTYPE, subtype);
        return SUCCEEDED(reader->SetCurrentMediaType(stream, nullptr, want.Get()));
    };
    if (!request(MFAudioFormat_PCM) && !request(MFAudioFormat_Float)) throw std::runtime_error("unsupported or corrupt audio file");

    ComPtr<IMFMediaType> got;
    reader->GetCurrentMediaType(stream, &got);
    GUID subtype{};
    got->GetGUID(MF_MT_SUBTYPE, &subtype);
    const bool is_float = subtype == MFAudioFormat_Float;
    const UINT32 rate = MFGetAttributeUINT32(got.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
    const UINT32 channels = std::max(MFGetAttributeUINT32(got.Get(), MF_MT_AUDIO_NUM_CHANNELS, 1), 1u);
    const UINT32 bits = is_float ? 32 : MFGetAttributeUINT32(got.Get(), MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (rate == 0) throw std::runtime_error("unknown sample rate");
    if (!is_float && bits != 16 && bits != 24 && bits != 32) throw std::runtime_error("unsupported sample format");
    const UINT32 bytes_per_sample = bits / 8;

    Downmixer mix(rate);
    std::vector<float> converted;
    for (;;) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        // ponytail: a read error mid-file ends the decode (the part read so far is analysed); Symphonia skips
        // single bad frames and fails on anything else. Revisit if truncated files give odd results.
        if (FAILED(reader->ReadSample(stream, 0, nullptr, &flags, nullptr, &sample))) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!sample) continue;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) continue;
        BYTE* data = nullptr;
        DWORD bytes = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &bytes))) continue;
        const size_t samples = bytes / bytes_per_sample;
        if (is_float) {
            mix.push(reinterpret_cast<const float*>(data), samples / channels, channels);
        } else {
            // Same integer → float scaling as Symphonia (divide by 2^(bits-1)).
            converted.resize(samples);
            for (size_t s = 0; s < samples; ++s) {
                const BYTE* p = data + s * bytes_per_sample;
                if (bits == 16) converted[s] = float(int16_t(p[0] | p[1] << 8)) / 32768.0f;
                else if (bits == 24) converted[s] = float(int32_t(uint32_t(p[0] << 8 | p[1] << 16 | p[2] << 24)) >> 8) / 8388608.0f;
                else converted[s] = float(double(int32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24)) / 2147483648.0);
            }
            mix.push(converted.data(), samples / channels, channels);
        }
        buffer->Unlock();
    }
    return mix.finish();
}

}  // namespace

Audio decode_mono(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) throw std::runtime_error("can't open " + display(path));
    auto ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    if (ext == L".wav" || ext == L".aif" || ext == L".aiff") return decode_wav(path);
    if (ext == L".mp3") return decode_mp3(path);
    if (ext == L".flac") return decode_flac(path);
    return decode_mf(path);  // AAC / M4A / ALAC (and OGG/Opus where Windows has the codecs)
}

}  // namespace wb
