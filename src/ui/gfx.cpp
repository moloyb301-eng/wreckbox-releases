#include "ui/gfx.h"

#include <algorithm>
#include <format>
#include <vector>

#include "../res/resource.h"

namespace wb::ui {
namespace {

std::wstring style_key(const TextStyle& st, bool wrap) {
    return std::format(L"{}|{}|{}|{}|{}", int(st.font), st.size, st.weight, int(st.align), wrap ? 1 : 0);
}

std::wstring color_key(std::initializer_list<D2D1_COLOR_F> colors) {
    std::wstring k;
    for (const auto& c : colors) k += std::format(L"{:.3f},{:.3f},{:.3f},{:.3f};", c.r, c.g, c.b, c.a);
    return k;
}

}  // namespace

bool Gfx::init(HWND hwnd) {
    hwnd_ = hwnd;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5), reinterpret_cast<IUnknown**>(dw_.GetAddressOf()))))
        return false;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
    load_fonts();  // without them DirectWrite falls back to Segoe UI — still usable
    dpi_ = float(GetDpiForWindow(hwnd));
    return true;
}

// Urbanist and Doto live in the exe as resources; register them as a private font collection.
bool Gfx::load_fonts() {
    ComPtr<IDWriteInMemoryFontFileLoader> loader;
    if (FAILED(dw_->CreateInMemoryFontFileLoader(&loader)) || FAILED(dw_->RegisterFontFileLoader(loader.Get()))) return false;
    ComPtr<IDWriteFontSetBuilder1> builder;
    if (FAILED(dw_->CreateFontSetBuilder(&builder))) return false;
    for (const int id : {IDR_FONT_URBANIST, IDR_FONT_DOTO}) {
        HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
        HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
        const void* data = mem ? LockResource(mem) : nullptr;
        if (!data) continue;
        ComPtr<IDWriteFontFile> file;
        // Resource memory lives as long as the process, so no owner object is needed.
        if (SUCCEEDED(loader->CreateInMemoryFontFileReference(dw_.Get(), data, SizeofResource(nullptr, res), nullptr, &file)))
            builder->AddFontFile(file.Get());  // variable fonts: adds every named instance (Regular, SemiBold, …)
    }
    ComPtr<IDWriteFontSet> set;
    return SUCCEEDED(builder->CreateFontSet(&set)) && SUCCEEDED(dw_->CreateFontCollectionFromFontSet(set.Get(), &fonts_));
}

bool Gfx::create_target() {
    if (rt_) return true;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    // WRECKBOX_SOFTWARE=1 draws on the CPU: an escape hatch for old PCs with broken or flaky graphics drivers.
    wchar_t sw[4]{};
    const bool software = GetEnvironmentVariableW(L"WRECKBOX_SOFTWARE", sw, 4) && sw[0] == L'1';
    const auto props = D2D1::RenderTargetProperties(software ? D2D1_RENDER_TARGET_TYPE_SOFTWARE : D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                    D2D1::PixelFormat(), dpi_, dpi_);
    // Normally don't block the UI thread waiting for the monitor's refresh (the desktop compositor already prevents
    // tearing); the moving visualizer does want to wait, so its frames are paced by the refresh (set_vsync).
    const auto hprops = D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(UINT(rc.right - rc.left), UINT(rc.bottom - rc.top)),
                                                         vsync_ ? D2D1_PRESENT_OPTIONS_NONE : D2D1_PRESENT_OPTIONS_IMMEDIATELY);
    if (FAILED(d2d_->CreateHwndRenderTarget(props, hprops, &rt_))) return false;
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);  // ClearType fringes look wrong on translucent fills
    rt_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &brush_);
    target_ = rt_;
    ++generation_;
    return true;
}

void Gfx::drop_target() {
    layouts_.clear();
    gradients_.clear();
    bitmap_brush_.Reset();
    bitmaps_.clear();
    dashes_.clear();
    radial_.Reset();
    stream_.Reset();
    layer_.Reset();
    brush_.Reset();
    target_.Reset();
    rt_.Reset();
}

void Gfx::resize(UINT w, UINT h) {
    if (rt_) rt_->Resize(D2D1::SizeU(w, h));
}

void Gfx::set_dpi(float dpi) {
    dpi_ = dpi;
    if (rt_) drop_target();  // cached bitmaps were rendered for the old scale
}

float Gfx::width() const {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    return float(rc.right - rc.left) * 96.f / dpi_;
}

float Gfx::height() const {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    return float(rc.bottom - rc.top) * 96.f / dpi_;
}

void Gfx::set_vsync(bool on) {
    if (on == vsync_) return;
    vsync_ = on;
    drop_target();  // made again with the other present option on the next begin()
}

bool Gfx::begin() {
    if (!create_target()) return false;
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    // A screen needs ~200 layouts; keep a few screens' worth (scrolling back is common) and start over beyond that.
    if (layouts_.size() > 800) layouts_.clear();
    return true;
}

void Gfx::end() {
    if (rt_->EndDraw() == HRESULT(D2DERR_RECREATE_TARGET)) {
        // The GPU was reset or changed: drop everything tied to the old target; it's rebuilt on the next paint.
        drop_target();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

// MARK: Shapes

void Gfx::fill(const Rect& r, D2D1_COLOR_F c) {
    brush_->SetColor(c);
    target_->FillRectangle(r.d2d(), brush_.Get());
}

void Gfx::fill_round(const Rect& r, float radius, D2D1_COLOR_F c) {
    brush_->SetColor(c);
    target_->FillRoundedRectangle(D2D1::RoundedRect(r.d2d(), radius, radius), brush_.Get());
}

void Gfx::stroke_round(const Rect& r, float radius, D2D1_COLOR_F c, float width) {
    brush_->SetColor(c);
    // Half-pixel inset keeps a 1-DIP border crisp and inside the shape, like the original app's Border.all.
    const Rect in = r.inset(width / 2, width / 2);
    target_->DrawRoundedRectangle(D2D1::RoundedRect(in.d2d(), radius, radius), brush_.Get(), width);
}

void Gfx::fill_circle(float cx, float cy, float radius, D2D1_COLOR_F c) {
    brush_->SetColor(c);
    target_->FillEllipse(D2D1::Ellipse({cx, cy}, radius, radius), brush_.Get());
}

void Gfx::stroke_circle(float cx, float cy, float radius, D2D1_COLOR_F c, float width, float dash) {
    brush_->SetColor(c);
    ID2D1StrokeStyle* style = nullptr;
    if (dash > 0) {
        const float d = dash / width;  // dash lengths are in stroke-width units
        auto& s = dashes_[int(d * 100)];
        if (!s) {
            const float pattern[] = {d, d};
            d2d_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                                D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_CUSTOM),
                                    pattern, 2, &s);
        }
        style = s.Get();
    }
    target_->DrawEllipse(D2D1::Ellipse({cx, cy}, radius, radius), brush_.Get(), width, style);
}

void Gfx::line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width) {
    brush_->SetColor(c);
    target_->DrawLine({x0, y0}, {x1, y1}, brush_.Get(), width);
}

ID2D1LinearGradientBrush* Gfx::gradient(std::initializer_list<D2D1_COLOR_F> colors) {
    // Each gradient is a small texture on the GPU; album placeholders bring a new colour per album, so keep only a few.
    if (gradients_.size() > 48) gradients_.clear();
    auto& b = gradients_[color_key(colors)];
    if (!b) {
        std::vector<D2D1_GRADIENT_STOP> stops;
        const size_t n = colors.size();
        size_t i = 0;
        for (const auto& c : colors) stops.push_back({n == 1 ? 0.f : float(i++) / float(n - 1), c});
        ComPtr<ID2D1GradientStopCollection> coll;
        rt_->CreateGradientStopCollection(stops.data(), UINT32(stops.size()), &coll);
        rt_->CreateLinearGradientBrush({{0, 0}, {1, 0}}, coll.Get(), &b);
    }
    return b.Get();
}

void Gfx::fill_gradient(const Rect& r, float radius, std::initializer_list<D2D1_COLOR_F> colors, bool diagonal) {
    ID2D1LinearGradientBrush* b = gradient(colors);
    b->SetStartPoint({r.l, r.t});
    b->SetEndPoint({r.r, diagonal ? r.b : r.t});
    target_->FillRoundedRectangle(D2D1::RoundedRect(r.d2d(), radius, radius), b);
}

void Gfx::fill_span(const Rect& r, D2D1_POINT_2F from, D2D1_POINT_2F to, std::initializer_list<D2D1_COLOR_F> colors) {
    ID2D1LinearGradientBrush* b = gradient(colors);
    b->SetStartPoint(from);
    b->SetEndPoint(to);
    target_->FillRectangle(r.d2d(), b);
}

void Gfx::fill_radial(const Rect& r, D2D1_POINT_2F center, float radius, D2D1_COLOR_F inner, D2D1_COLOR_F outer) {
    if (!radial_) {
        const D2D1_GRADIENT_STOP stops[] = {{0, inner}, {1, outer}};
        ComPtr<ID2D1GradientStopCollection> coll;
        rt_->CreateGradientStopCollection(stops, 2, &coll);
        rt_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(center, {0, 0}, radius, radius), coll.Get(), &radial_);
    }
    radial_->SetCenter(center);
    radial_->SetRadiusX(radius);
    radial_->SetRadiusY(radius);
    target_->FillRectangle(r.d2d(), radial_.Get());
}

void Gfx::push_opacity(float a) {
    if (!layer_) target_->CreateLayer(nullptr, &layer_);
    target_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a),
                       layer_.Get());
}
void Gfx::pop_opacity() { target_->PopLayer(); }

void Gfx::upload_stream(const uint8_t* bgra, int w, int h) {
    if (!rt_ || w <= 0 || h <= 0) return;
    if (!stream_ || stream_w_ != UINT(w) || stream_h_ != UINT(h)) {
        const auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi_, dpi_);
        stream_.Reset();
        if (FAILED(rt_->CreateBitmap(D2D1::SizeU(UINT(w), UINT(h)), nullptr, 0, props, &stream_))) return;
        stream_w_ = UINT(w), stream_h_ = UINT(h);
    }
    stream_->CopyFromMemory(nullptr, bgra, UINT(w) * 4);
}

bool Gfx::draw_stream(const Rect& r, bool flip, float zoom) {
    if (!stream_) return false;
    const auto centre = D2D1::Point2F((r.l + r.r) / 2, (r.t + r.b) / 2);
    target_->SetTransform(D2D1::Matrix3x2F::Scale(zoom, flip ? -zoom : zoom, centre));
    target_->DrawBitmap(stream_.Get(), r.d2d(), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    return true;
}

void Gfx::image(ID2D1Bitmap* bmp, const Rect& r, float radius, float opacity) {
    const auto size = bmp->GetSize();
    // Cover-fit: scale to fill the rect, centred, then clip to the rounded shape via a bitmap brush.
    const float scale = std::max(r.w() / size.width, r.h() / size.height);
    const float dx = r.l + (r.w() - size.width * scale) / 2, dy = r.t + (r.h() - size.height * scale) / 2;
    // One brush, pointed at each bitmap as it's drawn: a brush per cover would keep covers alive (on the GPU) after the
    // artwork cache let them go.
    auto& brush = bitmap_brush_;
    if (!brush)
        rt_->CreateBitmapBrush(bmp, D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                                                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
                               &brush);
    else
        brush->SetBitmap(bmp);
    brush->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(dx, dy));
    brush->SetOpacity(opacity);
    target_->FillRoundedRectangle(D2D1::RoundedRect(r.d2d(), radius, radius), brush.Get());
}

void Gfx::cached(const std::wstring& key, const Rect& r, const std::function<void(Gfx&)>& draw) {
    auto& bmp = bitmaps_[key];
    if (!bmp) {
        ComPtr<ID2D1BitmapRenderTarget> off;
        if (FAILED(rt_->CreateCompatibleRenderTarget(D2D1::SizeF(r.w(), r.h()), &off))) return draw(*this);
        auto saved = target_;
        target_ = off;
        off->BeginDraw();
        off->Clear(D2D1::ColorF(0, 0, 0, 0));
        off->SetTransform(D2D1::Matrix3x2F::Translation(-r.l, -r.t));
        draw(*this);
        off->EndDraw();
        off->GetBitmap(&bmp);
        target_ = saved;
    }
    target_->DrawBitmap(bmp.Get(), r.d2d());
}

// MARK: Text

IDWriteTextFormat* Gfx::format(const TextStyle& st, bool wrap) {
    const std::wstring key = style_key(st, wrap);
    if (const auto it = formats_.find(key); it != formats_.end()) return it->second.Get();
    const wchar_t* family = st.font == Font::ui ? L"Urbanist" : st.font == Font::dot ? L"Doto" : L"Segoe MDL2 Assets";
    IDWriteFontCollection* coll = st.font == Font::icon ? nullptr : fonts_.Get();
    ComPtr<IDWriteTextFormat> f;
    dw_->CreateTextFormat(family, coll, DWRITE_FONT_WEIGHT(st.weight), DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, st.size, L"en-us", &f);
    f->SetTextAlignment(st.align == Align::left ? DWRITE_TEXT_ALIGNMENT_LEADING
                        : st.align == Align::center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                    : DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (wrap) {
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    } else {
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ComPtr<IDWriteInlineObject> sign;
        dw_->CreateEllipsisTrimmingSign(f.Get(), &sign);
        const DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        f->SetTrimming(&trim, sign.Get());
    }
    formats_[key] = f;
    return f.Get();
}

ComPtr<IDWriteTextLayout> Gfx::layout(const std::wstring& s, const TextStyle& st, float w, float h, bool wrap) {
    ComPtr<IDWriteTextLayout> l;
    dw_->CreateTextLayout(s.data(), UINT32(s.size()), format(st, wrap), std::max(w, 0.f), std::max(h, 0.f), &l);
    if (l && st.spacing != 0) {
        ComPtr<IDWriteTextLayout1> l1;
        if (SUCCEEDED(l.As(&l1))) l1->SetCharacterSpacing(0, st.spacing, 0, {0, UINT32(s.size())});
    }
    return l;
}

// Text layout is the expensive part of drawing text; rows on screen repeat from frame to frame, so keep layouts.
IDWriteTextLayout* Gfx::cached_layout(const std::wstring& s, const TextStyle& st, float w, float h) {
    const std::wstring key = std::format(L"{}|{}|{:.1f}|{:.1f}|", style_key(st, false), st.spacing, w, h) + s;
    auto& l = layouts_[key];
    if (!l) l = layout(s, st, w, h, false);
    return l.Get();
}

void Gfx::text(const std::wstring& s, const Rect& in, const TextStyle& st) {
    if (s.empty() || in.w() <= 0) return;
    // 2 DIPs of slack on the trimming side: a box sized to measure() would otherwise lose its last character to rounding.
    Rect r = in;
    if (st.align == Align::left) r.r += 2;
    else if (st.align == Align::right) r.l -= 2;
    else r.l -= 1, r.r += 1;
    brush_->SetColor(st.color);
    if (IDWriteTextLayout* l = cached_layout(s, st, r.w(), r.h()))
        target_->DrawTextLayout({r.l, r.t}, l, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Gfx::paragraph(const std::wstring& s, const Rect& r, const TextStyle& st) {
    if (s.empty()) return 0;
    auto l = layout(s, st, r.w(), 10000, true);
    if (!l) return 0;
    brush_->SetColor(st.color);
    target_->DrawTextLayout({r.l, r.t}, l.Get(), brush_.Get());
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    return m.height;
}

float Gfx::measure(const std::wstring& s, const TextStyle& st) {
    const std::wstring key = style_key(st, false) + L"|" + std::to_wstring(st.spacing) + L"|" + s;
    if (const auto it = widths_.find(key); it != widths_.end()) return it->second;
    auto l = layout(s, st, 100000, 1000, false);
    DWRITE_TEXT_METRICS m{};
    if (l) l->GetMetrics(&m);
    if (widths_.size() > 4000) widths_.clear();
    return widths_[key] = m.widthIncludingTrailingWhitespace;
}

float Gfx::measure_height(const std::wstring& s, float width, const TextStyle& st) {
    auto l = layout(s, st, width, 10000, true);
    DWRITE_TEXT_METRICS m{};
    if (l) l->GetMetrics(&m);
    return m.height;
}

void Gfx::icon(const wchar_t* glyph, float cx, float cy, float size, D2D1_COLOR_F c) {
    TextStyle st{Font::icon, size, 400, c, Align::center};
    text(glyph, Rect{cx - size, cy - size, cx + size, cy + size}, st);
}

void Gfx::push_clip(const Rect& r) { target_->PushAxisAlignedClip(r.d2d(), D2D1_ANTIALIAS_MODE_ALIASED); }
void Gfx::pop_clip() { target_->PopAxisAlignedClip(); }

}  // namespace wb::ui
