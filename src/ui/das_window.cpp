#include "ui/das_window.h"

#include <shellapi.h>

#include <iterator>

namespace wb::ui {

namespace {

constexpr wchar_t kClass[] = L"WreckBoxDasVisWindow";

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;  // another screen in full screen: the app keeps the keys
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: return ValidateRect(hwnd, nullptr), 0;  // DasVis fills it
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

DasWindow::DasWindow(ID3D11Device* dev, HWND parent, const RECT& screen, const dasvis::Config& cfg) : parent_(parent) {
    static const bool registered = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        return RegisterClassW(&wc) != 0;
    }();
    if (!registered || !dev) return;
    RECT r = screen;
    if (parent) GetClientRect(parent, &r);
    size_ = {r.right - r.left, r.bottom - r.top};
    if (size_.cx <= 0 || size_.cy <= 0) return;
    // A child of another process's window (the desktop's) is allowed, and how every live wallpaper works.
    hwnd_ = parent ? CreateWindowExW(WS_EX_NOACTIVATE, kClass, L"", WS_CHILD | WS_VISIBLE | WS_DISABLED, 0, 0, size_.cx, size_.cy, parent, nullptr,
                                     GetModuleHandleW(nullptr), nullptr)
                   : CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kClass, L"WreckBox DasVis", WS_POPUP, r.left, r.top, size_.cx, size_.cy,
                                     nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd_) return;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
        return;
    DXGI_SWAP_CHAIN_DESC1 desc{};  // as the app's own window's (Gfx): one buffer, copied to the screen by the compositor
    // The wallpaper's buffer is 3/4 of the desktop's size, stretched by the compositor: half the GPU time (Intel UHD:
    // 6.2 → 3.4 ms a frame) and no softer to look at, since Low draws the scene at about a third of the size anyway.
    const float k = parent ? 0.75f : 1.f;
    desc.Width = UINT(size_.cx * k), desc.Height = UINT(size_.cy * k);
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 1;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    if (FAILED(factory->CreateSwapChainForHwnd(dev, hwnd_, &desc, nullptr, nullptr, &swap_))) return;
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_WINDOW_CHANGES);
    vis_ = std::make_unique<dasvis::DasVis>(dev, cfg);
    if (!parent) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

DasWindow::~DasWindow() {
    vis_.reset();
    swap_.Reset();
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
    // The desktop doesn't paint over the last picture by itself: its wallpaper is set again, the same file, for this
    // session only (nothing saved, nothing announced; "" for a plain colour sets that again too).
    // ponytail: after a crash the last picture stays until the wallpaper next changes (or the live one runs again).
    if (parent_) {
        wchar_t path[MAX_PATH * 2]{};
        if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, UINT(std::size(path)), path, 0)) SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, path, 0);
    }
}

bool DasWindow::ok() const { return swap_ && vis_ && vis_->ok() && IsWindow(hwnd_); }

Microsoft::WRL::ComPtr<ID3D11Texture2D> DasWindow::back() const {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> t;
    if (swap_) swap_->GetBuffer(0, IID_PPV_ARGS(&t));
    return t;
}

void DasWindow::present() {
    if (swap_) swap_->Present(0, 0);  // no waiting on the refresh: the app's own frame (or the wallpaper's timer) paces it
}

HWND desktop_worker() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return nullptr;
    DWORD_PTR ignored = 0;
    // Asks the desktop to put a WorkerW between the icons and the wallpaper (as when a slideshow fades), and to keep it.
    SendMessageTimeoutW(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, &ignored);
    HWND worker = nullptr;
    EnumWindows(
        [](HWND top, LPARAM lp) -> BOOL {
            if (FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr))  // the icons' window: the WorkerW after it
                *reinterpret_cast<HWND*>(lp) = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
            return TRUE;
        },
        LPARAM(&worker));
    // ponytail: Windows 11 24H2 moved the WorkerW inside Progman; found here, but only checked on Windows 10.
    if (!worker) worker = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
    return worker;
}

bool desktop_hidden() {
    wchar_t v[4]{};  // WRECKBOX_RENDER_HIDDEN=1: draw anyway (checking it while working in a maximised window)
    if (GetEnvironmentVariableW(L"WRECKBOX_RENDER_HIDDEN", v, 4) && v[0] == L'1') return false;
    QUERY_USER_NOTIFICATION_STATE s{};
    if (SUCCEEDED(SHQueryUserNotificationState(&s)) && (s == QUNS_BUSY || s == QUNS_RUNNING_D3D_FULL_SCREEN || s == QUNS_PRESENTATION_MODE))
        return true;
    const HWND fg = GetForegroundWindow();
    return GetSystemMetrics(SM_CMONITORS) == 1 && fg && IsZoomed(fg);
}

}  // namespace wb::ui
