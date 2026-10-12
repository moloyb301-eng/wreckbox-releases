// Windows DasVis draws into besides the app's own: the live wallpaper (a child of the desktop's WorkerW, the window
// behind the icons that draws the wallpaper) and, in full screen, the other screens. Each has a swap chain of its own on
// the app's Direct3D device and its own DasVis, so a frame is one render and one present, nothing copied.
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <memory>
#include <string>

#include "dasvis/dasvis.h"

namespace wb::ui {

// What a DasVis has been given for the song playing: its cover, the title card, the lyric line on screen.
struct DasShown {
    std::string song;  // the song these are for
    bool cover = false;
    int lyric = -1;
};

class DasWindow {
public:
    // `parent`: the window it's a child of, covering its client area (the wallpaper); nullptr: a popup over `screen` (in
    // screen coordinates) that never takes the focus (another screen in full screen).
    DasWindow(ID3D11Device* dev, HWND parent, const RECT& screen, const dasvis::Config& cfg);
    ~DasWindow();
    DasWindow(const DasWindow&) = delete;
    DasWindow& operator=(const DasWindow&) = delete;

    bool ok() const;
    HWND hwnd() const { return hwnd_; }
    SIZE size() const { return size_; }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back() const;
    void present();
    dasvis::DasVis& vis() { return *vis_; }
    DasShown shown;
    int generation = -1;  // the Gfx generation (device) it was made on

private:
    HWND hwnd_ = nullptr, parent_ = nullptr;
    SIZE size_{};
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_;
    std::unique_ptr<dasvis::DasVis> vis_;
};

// The desktop's WorkerW, asked of Progman (what live-wallpaper programs draw into), or nullptr.
HWND desktop_worker();
// The desktop can't be seen: a full-screen app or game, a presentation, or (on one screen) a maximised window in front.
bool desktop_hidden();

}  // namespace wb::ui
