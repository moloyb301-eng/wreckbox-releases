// Drawing layer: Direct2D + DirectWrite with the app's embedded fonts. Everything is in DIPs (device-independent
// pixels; 1 DIP = 1 px at 100 % scaling), so layout code never thinks about DPI.
#pragma once
#include <d2d1_1.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <functional>
#include <string>
#include <unordered_map>

namespace wb::ui {

using Microsoft::WRL::ComPtr;

struct Rect {
    float l = 0, t = 0, r = 0, b = 0;
    float w() const { return r - l; }
    float h() const { return b - t; }
    bool contains(float x, float y) const { return x >= l && x < r && y >= t && y < b; }
    Rect inset(float d) const { return {l + d, t + d, r - d, b - d}; }
    Rect inset(float dx, float dy) const { return {l + dx, t + dy, r - dx, b - dy}; }
    Rect inset(float left, float top, float right, float bottom) const { return {l + left, t + top, r - right, b - bottom}; }
    D2D1_RECT_F d2d() const { return {l, t, r, b}; }
    static Rect xywh(float x, float y, float w, float h) { return {x, y, x + w, y + h}; }
};

enum class Font { ui, dot, icon };
enum class Align { left, center, right };

struct TextStyle {
    Font font = Font::ui;
    float size = 13;
    int weight = 400;
    D2D1_COLOR_F color{1, 1, 1, 0.94f};
    Align align = Align::left;
    float spacing = 0;  // extra letter spacing in DIPs (dot labels use 1.6)
};

class Gfx {
public:
    bool init(HWND hwnd);  // factories + fonts; false if Direct2D / DirectWrite are unavailable
    void resize(UINT w, UINT h);
    void set_dpi(float dpi);
    float dpi() const { return dpi_; }
    float width() const;   // client size in DIPs
    float height() const;

    bool begin();  // false if the render target couldn't be created
    void end();    // handles device loss by recreating the target next frame

    ID2D1RenderTarget* rt() const { return rt_.Get(); }
    IWICImagingFactory* wic() const { return wic_.Get(); }
    // Bumped whenever the render target is (re)created: bitmaps made on an older target must be dropped.
    int generation() const { return generation_; }

    // Shapes
    void fill(const Rect& r, D2D1_COLOR_F c);
    void fill_round(const Rect& r, float radius, D2D1_COLOR_F c);
    void stroke_round(const Rect& r, float radius, D2D1_COLOR_F c, float width = 1);
    void fill_circle(float cx, float cy, float radius, D2D1_COLOR_F c);
    void stroke_circle(float cx, float cy, float radius, D2D1_COLOR_F c, float width = 1, float dash = 0);
    void line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width = 1);
    // Horizontal / diagonal gradient through up to three colours.
    void fill_gradient(const Rect& r, float radius, std::initializer_list<D2D1_COLOR_F> colors, bool diagonal = false);
    // Fills r with a gradient running from `from` (first colour) to `to` (last), independent of r (spectrum bars).
    void fill_span(const Rect& r, D2D1_POINT_2F from, D2D1_POINT_2F to, std::initializer_list<D2D1_COLOR_F> colors);
    void fill_radial(const Rect& r, D2D1_POINT_2F center, float radius, D2D1_COLOR_F inner, D2D1_COLOR_F outer);
    void image(ID2D1Bitmap* bmp, const Rect& r, float radius, float opacity = 1);
    // A picture that changes every frame (MilkDrop): one BGRA bitmap, made again only when the size or the render target
    // changes, refilled with `upload`, drawn stretched over r (`flip`: the rows are bottom-up, as OpenGL reads them).
    void upload_stream(const uint8_t* bgra, int w, int h);
    bool draw_stream(const Rect& r, bool flip);  // false if nothing was uploaded yet
    void clear_stream() { stream_.Reset(); }

    // Text — one line, trimmed with an ellipsis to the rect width, vertically centred in the rect.
    void text(const std::wstring& s, const Rect& r, const TextStyle& st);
    // Multi-line text wrapped to the rect width, top-aligned; returns the height used.
    float paragraph(const std::wstring& s, const Rect& r, const TextStyle& st);
    float measure(const std::wstring& s, const TextStyle& st);  // single-line width
    float measure_height(const std::wstring& s, float width, const TextStyle& st);
    void icon(const wchar_t* glyph, float cx, float cy, float size, D2D1_COLOR_F c);

    // Everything drawn until pop_opacity() is composited at `a` (0–1): fades a whole panel, buttons and text included.
    void push_opacity(float a);
    void pop_opacity();

    void push_clip(const Rect& r);
    void pop_clip();

    // Draws something expensive (e.g. the pixel logo) once into a cached bitmap of `w`×`h` DIPs, then reuses it.
    void cached(const std::wstring& key, const Rect& r, const std::function<void(Gfx&)>& draw);

private:
    bool create_target();
    void drop_target();
    bool load_fonts();
    IDWriteTextFormat* format(const TextStyle& st, bool wrap);
    ComPtr<IDWriteTextLayout> layout(const std::wstring& s, const TextStyle& st, float w, float h, bool wrap);
    IDWriteTextLayout* cached_layout(const std::wstring& s, const TextStyle& st, float w, float h);
    ID2D1LinearGradientBrush* gradient(std::initializer_list<D2D1_COLOR_F> colors);

    HWND hwnd_ = nullptr;
    float dpi_ = 96;
    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory5> dw_;
    ComPtr<IDWriteFontCollection1> fonts_;
    ComPtr<IWICImagingFactory> wic_;
    ComPtr<ID2D1HwndRenderTarget> rt_;
    ComPtr<ID2D1RenderTarget> target_;  // what drawing goes to: the window, or a bitmap while cached() records
    ComPtr<ID2D1SolidColorBrush> brush_;
    int generation_ = 0;
    std::unordered_map<std::wstring, ComPtr<IDWriteTextFormat>> formats_;
    std::unordered_map<std::wstring, float> widths_;  // measure() cache
    // Per-target caches: creating Direct2D resources every frame is what makes a frame slow, so they're kept and only
    // repositioned. Cleared when the render target is recreated.
    std::unordered_map<std::wstring, ComPtr<IDWriteTextLayout>> layouts_;
    std::unordered_map<std::wstring, ComPtr<ID2D1LinearGradientBrush>> gradients_;
    std::unordered_map<ID2D1Bitmap*, ComPtr<ID2D1BitmapBrush>> bitmap_brushes_;
    std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap>> bitmaps_;
    std::unordered_map<int, ComPtr<ID2D1StrokeStyle>> dashes_;
    ComPtr<ID2D1RadialGradientBrush> radial_;
    ComPtr<ID2D1Bitmap> stream_;
    ComPtr<ID2D1Layer> layer_;
    UINT stream_w_ = 0, stream_h_ = 0;
};

}  // namespace wb::ui
