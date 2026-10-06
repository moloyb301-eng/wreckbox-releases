// A picture of the app's own window for bug reports: PrintWindow (which also gets Direct2D content) → a WIC PNG.
#pragma once
#include <windows.h>
#include <wincodec.h>

#include <string>

namespace wb::ui {

// PNG bytes of `hwnd` (scaled down to at most `max_width` pixels wide), or empty if it can't be captured.
std::string capture_window_png(HWND hwnd, IWICImagingFactory* wic, UINT max_width = 1600);

}  // namespace wb::ui
