#include "ui/screenshot.h"

#include <wrl/client.h>

namespace wb::ui {

using Microsoft::WRL::ComPtr;

std::string capture_window_png(HWND hwnd, IWICImagingFactory* wic, UINT max_width) {
    RECT rc;
    if (!wic || !GetWindowRect(hwnd, &rc)) return {};
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return {};

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    ReleaseDC(nullptr, screen);
    HGDIOBJ old = SelectObject(mem, bmp);
    const BOOL printed = PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT);
    SelectObject(mem, old);
    DeleteDC(mem);

    std::string out;
    ComPtr<IWICBitmap> src;
    if (printed && SUCCEEDED(wic->CreateBitmapFromHBITMAP(bmp, nullptr, WICBitmapIgnoreAlpha, &src))) {
        ComPtr<IWICBitmapSource> image = src;
        UINT iw = UINT(w), ih = UINT(h);
        if (iw > max_width) {
            ComPtr<IWICBitmapScaler> scaler;
            ih = UINT(double(h) * max_width / w);
            iw = max_width;
            if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) && SUCCEEDED(scaler->Initialize(src.Get(), iw, ih, WICBitmapInterpolationModeFant)))
                image = scaler;
            else iw = UINT(w), ih = UINT(h);
        }
        ComPtr<IStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) && SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
            SUCCEEDED(frame->Initialize(nullptr))) {
            WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
            frame->SetSize(iw, ih);
            frame->SetPixelFormat(&format);
            if (SUCCEEDED(frame->WriteSource(image.Get(), nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit())) {
                HGLOBAL mem_block = nullptr;
                if (SUCCEEDED(GetHGlobalFromStream(stream.Get(), &mem_block))) {
                    STATSTG st{};
                    stream->Stat(&st, STATFLAG_NONAME);  // the PNG's real length (the block can be bigger)
                    const SIZE_T n = SIZE_T(st.cbSize.QuadPart);
                    if (const void* p = GlobalLock(mem_block)) {
                        out.assign(static_cast<const char*>(p), n);
                        GlobalUnlock(mem_block);
                    }
                }
            }
        }
    }
    DeleteObject(bmp);
    return out;
}

}  // namespace wb::ui
