// Fixture tool (scripts/make_audio_fixtures.ps1): encodes a 16-bit stereo 44.1 kHz WAV with Windows' built-in encoders.
//   mf_encode in.wav out.(flac|wma|m4a)
// .m4a = ALAC (lossless). WMA picks a type the encoder offers for 44.1 kHz stereo.
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
using Microsoft::WRL::ComPtr;
#define HR(x) do { HRESULT h_ = (x); if (FAILED(h_)) { std::printf("%s failed 0x%08lx\n", #x, (unsigned long)h_); return 1; } } while (0)

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<char> wav((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const char* pcm = wav.data() + 44;
    const size_t bytes = wav.size() - 44;
    const std::wstring out = argv[2];
    HR(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    HR(MFStartup(MF_VERSION));
    ComPtr<IMFMediaType> type;
    if (out.ends_with(L".wma")) {
        ComPtr<IMFCollection> types;
        HR(MFTranscodeGetAudioOutputAvailableTypes(MFAudioFormat_WMAudioV8, MFT_ENUM_FLAG_ALL, nullptr, &types));
        DWORD n = 0;
        types->GetElementCount(&n);
        for (DWORD i = 0; i < n && !type; ++i) {
            ComPtr<IUnknown> u;
            types->GetElement(i, &u);
            ComPtr<IMFMediaType> t;
            u.As(&t);
            if (MFGetAttributeUINT32(t.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0) == 44100 && MFGetAttributeUINT32(t.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0) == 2 &&
                MFGetAttributeUINT32(t.Get(), MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 0) >= 16000)
                type = t;
        }
        if (!type) { std::puts("no WMA type for 44.1 kHz stereo"); return 1; }
    } else {
        MFCreateMediaType(&type);
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        type->SetGUID(MF_MT_SUBTYPE, out.ends_with(L".flac") ? MFAudioFormat_FLAC : MFAudioFormat_ALAC);
        type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
        type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    }
    ComPtr<IMFSinkWriter> w;
    HR(MFCreateSinkWriterFromURL(out.c_str(), nullptr, nullptr, &w));
    DWORD stream = 0;
    HR(w->AddStream(type.Get(), &stream));
    ComPtr<IMFMediaType> pcmType;
    MFCreateMediaType(&pcmType);
    pcmType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pcmType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    pcmType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
    pcmType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    pcmType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    pcmType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    pcmType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 44100 * 4);
    HR(w->SetInputMediaType(stream, pcmType.Get(), nullptr));
    HR(w->BeginWriting());
    LONGLONG t = 0;
    for (size_t off = 0; off < bytes; off += 4096 * 4) {
        const DWORD n = DWORD(std::min<size_t>(4096 * 4, bytes - off));
        ComPtr<IMFMediaBuffer> buf;
        HR(MFCreateMemoryBuffer(n, &buf));
        BYTE* p;
        buf->Lock(&p, nullptr, nullptr);
        memcpy(p, pcm + off, n);
        buf->Unlock();
        buf->SetCurrentLength(n);
        ComPtr<IMFSample> s;
        MFCreateSample(&s);
        s->AddBuffer(buf.Get());
        const LONGLONG dur = LONGLONG(n / 4) * 10000000 / 44100;
        s->SetSampleTime(t);
        s->SetSampleDuration(dur);
        t += dur;
        HR(w->WriteSample(stream, s.Get()));
    }
    HR(w->Finalize());
    std::printf("wrote %ls\n", out.c_str());
    return 0;
}
