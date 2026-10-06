#include "ui/artwork.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace wb::ui {
namespace {

// Decodes `file` to premultiplied BGRA at most `px` on its longer side. Runs on a worker thread.
ComPtr<IWICBitmap> decode(const std::filesystem::path& file, UINT px) {
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return nullptr;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)))
        return nullptr;
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (!w || !h) return nullptr;
    const double scale = std::min(1.0, double(px) / double(std::max(w, h)));
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> conv;
    ComPtr<IWICBitmap> out;
    if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), std::max(1u, UINT(w * scale)), std::max(1u, UINT(h * scale)), WICBitmapInterpolationModeFant)) ||
        FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(wic->CreateBitmapFromSource(conv.Get(), WICBitmapCacheOnLoad, &out)))
        return nullptr;
    return out;
}

}  // namespace

void ArtworkCache::begin_frame() {
    ++frame_;
    if (gfx_.generation() != generation_) {  // new render target: old bitmaps are unusable
        entries_.clear();
        generation_ = gfx_.generation();
    }
}

ID2D1Bitmap* ArtworkCache::get(const LibraryTrack& t, float dips) {
    // Size classes so a cover isn't decoded again for every slightly different size.
    const UINT need = UINT(std::ceil(dips * gfx_.dpi() / 96.f));
    UINT px = 64;
    while (px < need && px < 640) px *= 2;
    const std::string key = t.id + "@" + std::to_string(px);
    Entry& e = entries_[key];
    e.used = frame_;
    // A request still pending after ~200 paints was probably dropped from the newest-first queue (it scrolled away and
    // came back); ask again.
    if (e.bmp || e.missing || (e.loading && frame_ - e.requested < 200)) return e.bmp.Get();

    e.loading = true;
    e.requested = frame_;
    auto result = std::make_shared<ComPtr<IWICBitmap>>();
    jobs_.run_latest(
        [this, t, px, result] {
            if (const auto file = store_.ensure_artwork(t)) *result = decode(*file, px);
        },
        [this, key, result, gen = generation_] {
            const auto it = entries_.find(key);
            if (it == entries_.end() || gen != gfx_.generation()) return;
            it->second.loading = false;
            if (!*result || !gfx_.rt() || FAILED(gfx_.rt()->CreateBitmapFromWicBitmap(result->Get(), &it->second.bmp))) it->second.missing = true;
        });
    if (entries_.size() > 300) evict();
    return nullptr;
}

void ArtworkCache::evict() {
    std::vector<std::pair<uint64_t, std::string>> old;
    for (const auto& [k, e] : entries_)
        if (!e.loading && e.used + 2 < frame_) old.emplace_back(e.used, k);
    std::sort(old.begin(), old.end());
    for (size_t i = 0; i < old.size() && entries_.size() > 250; ++i) entries_.erase(old[i].second);
}

}  // namespace wb::ui
