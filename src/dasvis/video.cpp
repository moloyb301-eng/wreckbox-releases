#include <windows.h>

#include "dasvis/video.h"

#include <codecapi.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <stdexcept>

#include "dasvis/dasvis.h"
#include "dasvis/features.h"

namespace wb::dasvis {

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT32 kRate = 48000, kChannels = 2;  // the audio as DasVis's analysis expects it, and as it's written

struct MfScope {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT mf = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    ~MfScope() {
        if (SUCCEEDED(mf)) MFShutdown();
        if (SUCCEEDED(com)) CoUninitialize();
    }
};

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char buf[160];
        snprintf(buf, sizeof buf, "%s (0x%08lX)", what, static_cast<unsigned long>(hr));
        throw std::runtime_error(buf);
    }
}

ComPtr<IMFMediaType> pcm_type() {
    ComPtr<IMFMediaType> t;
    check(MFCreateMediaType(&t), "media type");
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
    t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kChannels * 2);
    t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * kChannels * 2);
    return t;
}

void video_type(IMFMediaType* t, const VideoOptions& o) {
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(t, MF_MT_FRAME_SIZE, UINT32(o.width), UINT32(o.height));
    MFSetAttributeRatio(t, MF_MT_FRAME_RATE, UINT32(o.fps), 1);
    MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
}

// Writes an IMFSample of `bytes` (copied by `fill`) at `time` for `duration` (100 ns units).
void write(IMFSinkWriter* w, DWORD stream, size_t bytes, LONGLONG time, LONGLONG duration, const std::function<void(BYTE*)>& fill) {
    ComPtr<IMFMediaBuffer> buf;
    ComPtr<IMFSample> s;
    check(MFCreateMemoryBuffer(DWORD(bytes), &buf), "buffer");
    BYTE* p = nullptr;
    check(buf->Lock(&p, nullptr, nullptr), "buffer");
    fill(p);
    buf->Unlock();
    buf->SetCurrentLength(DWORD(bytes));
    check(MFCreateSample(&s), "sample");
    s->AddBuffer(buf.Get());
    s->SetSampleTime(time);
    s->SetSampleDuration(duration);
    check(w->WriteSample(stream, s.Get()), "writing");
}

}  // namespace

std::string export_video(const std::filesystem::path& audio, const std::filesystem::path& out, const VideoOptions& opt, const SongMap* map,
                         const std::function<bool(double)>& progress) {
    bool done = false;
    std::string error;
    {
        MfScope mf;
        ComPtr<IMFSinkWriter> writer;
        try {
            if (FAILED(mf.mf)) throw std::runtime_error("Media Foundation is not available");
            if (opt.width < 16 || opt.height < 16 || opt.fps < 1) throw std::runtime_error("bad video size");
            std::optional<SongMap> own;
            if (!map || map->beats.size() < 16) {
                std::string why;
                own = analyse_file(audio, opt.bpm, &why);
                if (!own) throw std::runtime_error("couldn't analyse the song: " + why);
                map = &*own;
            }

            // The song, as 48 kHz stereo (Media Foundation converts whatever the file is).
            ComPtr<IMFSourceReader> reader;
            check(MFCreateSourceReaderFromURL(audio.c_str(), nullptr, &reader), "couldn't open the song");
            const DWORD astream = DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
            reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
            check(reader->SetStreamSelection(astream, TRUE), "no audio in the song");
            check(reader->SetCurrentMediaType(astream, nullptr, pcm_type().Get()), "couldn't decode the song as 48 kHz stereo");
            PROPVARIANT dur;
            PropVariantInit(&dur);
            double seconds = map->duration;
            if (SUCCEEDED(reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &dur))) seconds = double(dur.uhVal.QuadPart) / 1e7;
            PropVariantClear(&dur);
            if (seconds <= 0) throw std::runtime_error("the song has no length");

            // The file: H.264 (on the graphics card when it can) and AAC.
            ComPtr<IMFAttributes> attrs;
            MFCreateAttributes(&attrs, 2);
            attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
            check(MFCreateSinkWriterFromURL(out.c_str(), nullptr, attrs.Get(), &writer), "couldn't create the video file");
            ComPtr<IMFMediaType> vout, vin, aout;
            check(MFCreateMediaType(&vout), "media type");
            video_type(vout.Get(), opt);
            vout->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
            vout->SetUINT32(MF_MT_AVG_BITRATE, UINT32(std::min(100e6, double(opt.width) * opt.height * opt.fps * 0.12)));  // ~15 Mbps at 1080p60
            vout->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
            DWORD vstream = 0, aw = 0;
            check(writer->AddStream(vout.Get(), &vstream), "no H.264 encoder");
            check(MFCreateMediaType(&vin), "media type");
            video_type(vin.Get(), opt);
            vin->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            vin->SetUINT32(MF_MT_DEFAULT_STRIDE, UINT32(opt.width * 4));  // top row first
            check(writer->SetInputMediaType(vstream, vin.Get(), nullptr), "the H.264 encoder won't take the picture");
            check(MFCreateMediaType(&aout), "media type");
            aout->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            aout->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            aout->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
            aout->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
            aout->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            aout->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);  // 192 kbps
            check(writer->AddStream(aout.Get(), &aw), "no AAC encoder");
            check(writer->SetInputMediaType(aw, pcm_type().Get(), nullptr), "the AAC encoder won't take the sound");
            check(writer->BeginWriting(), "couldn't start writing");

            // Its own Direct3D device and DasVis, at High, full size.
            ComPtr<ID3D11Device> dev;
            ComPtr<ID3D11DeviceContext> ctx;
            const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
            check(D3D11CreateDevice(nullptr, opt.software ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                    levels, UINT(std::size(levels)), D3D11_SDK_VERSION, &dev, nullptr, &ctx),
                  "no Direct3D 11 graphics");
            Config cfg;
            cfg.start = Tier::high, cfg.auto_tier = false, cfg.fixed_scale = 1;
            DasVis dv(dev.Get(), cfg);
            if (!dv.ok()) throw std::runtime_error("DasVis: " + dv.error());
            if (!opt.cover.empty()) dv.set_cover(opt.cover.data(), opt.cover_w, opt.cover_h);
            if (!opt.title.empty())
                if (ID3D11Texture2D* t = dv.title_target(opt.title_w, opt.title_h)) {
                    ctx->UpdateSubresource(t, 0, nullptr, opt.title.data(), UINT(opt.title_w) * 4, 0);
                    dv.show_title(opt.title_letters, opt.title_share);
                }
            D3D11_TEXTURE2D_DESC td{UINT(opt.width), UINT(opt.height), 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
            ComPtr<ID3D11Texture2D> frame, staging;
            check(dev->CreateTexture2D(&td, nullptr, &frame), "frame");
            td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            check(dev->CreateTexture2D(&td, nullptr, &staging), "frame");

            // Frame by frame: the sound up to the frame's time goes into the file and into the analysis window.
            Features f;
            std::deque<float> mono;  // the decoded sound not yet behind the analysis window, from sample `mono_at`
            int64_t mono_at = 0, decoded = 0, written = 0;
            bool audio_end = false;
            std::vector<float> window(Features::kFft);
            const auto read_audio = [&](int64_t until) {
                while (!audio_end && decoded < until) {
                    DWORD flags = 0;
                    ComPtr<IMFSample> sample;
                    if (FAILED(reader->ReadSample(astream, 0, nullptr, &flags, nullptr, &sample)) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
                        audio_end = true;
                        break;
                    }
                    if (!sample) continue;
                    ComPtr<IMFMediaBuffer> buf;
                    BYTE* p = nullptr;
                    DWORD bytes = 0;
                    if (FAILED(sample->ConvertToContiguousBuffer(&buf)) || FAILED(buf->Lock(&p, nullptr, &bytes))) continue;
                    const int64_t n = bytes / (kChannels * 2);
                    const auto* pcm = reinterpret_cast<const int16_t*>(p);
                    for (int64_t i = 0; i < n; ++i) mono.push_back((float(pcm[2 * i]) + float(pcm[2 * i + 1])) / 65536.f);
                    write(writer.Get(), aw, size_t(bytes), written * 10000000 / kRate, n * 10000000 / kRate, [&](BYTE* d) { std::memcpy(d, p, bytes); });
                    buf->Unlock();
                    written += n, decoded += n;
                }
            };
            const int64_t frames = int64_t(std::ceil(seconds * opt.fps));
            const LONGLONG frame_dur = 10000000 / opt.fps;
            Inputs in;
            in.f = &f, in.dt = 1.f / float(opt.fps), in.budget_ms = 1000;  // no time limit: every scene fits
            in.song = opt.song_id, in.camelot = opt.camelot, in.intensity = opt.intensity, in.map = map, in.taste = &opt.taste;
            for (int64_t i = 0; i < frames; ++i) {
                const double t = double(i) / opt.fps;
                const int64_t end = int64_t(t * kRate);  // the window ends at the frame's time
                read_audio(end);
                for (size_t k = 0; k < Features::kFft; ++k) {
                    const int64_t s = end - int64_t(Features::kFft) + int64_t(k);
                    window[k] = s >= mono_at && s - mono_at < int64_t(mono.size()) ? mono[size_t(s - mono_at)] : 0.f;
                }
                while (mono_at < end - int64_t(Features::kFft) && !mono.empty()) mono.pop_front(), ++mono_at;
                f.update(window.data(), in.dt, map->bpm);
                in.song_time = t;
                dv.render(in, frame.Get());
                ctx->CopyResource(staging.Get(), frame.Get());
                D3D11_MAPPED_SUBRESOURCE m;
                check(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m), "reading the frame");
                write(writer.Get(), vstream, size_t(opt.width) * opt.height * 4, i * frame_dur, frame_dur, [&](BYTE* d) {
                    for (int y = 0; y < opt.height; ++y)
                        std::memcpy(d + size_t(y) * opt.width * 4, static_cast<const BYTE*>(m.pData) + size_t(y) * m.RowPitch, size_t(opt.width) * 4);
                });
                ctx->Unmap(staging.Get(), 0);
                if ((i % 15 == 0 || i + 1 == frames) && progress && !progress(double(i + 1) / double(frames))) throw std::runtime_error("cancelled");
            }
            read_audio(INT64_MAX);  // the rest of the sound
            check(writer->Finalize(), "finishing the file");
            done = true;
        } catch (const std::exception& e) {
            error = e.what();
        }
        writer.Reset();  // closed before the file can be deleted
    }
    if (!done) {
        std::error_code ec;
        std::filesystem::remove(out, ec);
    }
    return done ? std::string() : error;
}

}  // namespace wb::dasvis
