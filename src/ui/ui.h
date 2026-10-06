// Immediate-mode UI on top of Gfx. Each paint draws the whole screen from current state and records clickable and
// scrollable regions; mouse input is matched against the regions from the last paint. Nothing animates, so nothing
// repaints unless input or data changed — idle CPU stays at zero.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "model/model.h"
#include "ui/artwork.h"
#include "ui/gfx.h"
#include "ui/theme.h"

namespace wb::ui {

class Ui {
public:
    Ui(Gfx& g, ArtworkCache& art) : g(g), art(art) {}
    Gfx& g;
    ArtworkCache& art;

    // MARK: Frame
    void begin_frame();
    float mx = -1, my = -1;  // mouse position in DIPs (-1 when outside the window)

    // MARK: Regions
    bool hover(const Rect& r) const;  // mouse is over r and r isn't clipped away
    void click(const Rect& r, std::function<void()> on_click, std::function<void()> on_right = {});
    // A vertically scrolling area; `offset` lives in the caller's state. Draws the scrollbar.
    void scroll_area(const Rect& r, float& offset, float content_height);
    void push_clip(const Rect& r);
    void pop_clip();
    // A tooltip for r, shown once the pointer has rested on it for half a second (and hidden by a click until the
    // pointer moves to something else). Call draw_tooltip() last in the paint: it returns how many ms until a pending
    // tooltip is due (the caller sets a timer for that repaint), or 0.
    void tip(const Rect& r, std::wstring label);
    unsigned draw_tooltip();
    // A slider over r (0–1). `on_change(value, done)` fires on press and while dragging (done = false), then once on
    // release (done = true). Vertical sliders run bottom (0) to top (1); the fill starts at `origin` (0.5 = centred).
    void slider(const Rect& r, float value, std::function<void(float, bool)> on_change, bool vertical = false, float origin = 0);

    // MARK: Input (window procedure)
    bool mouse_down(float x, float y, bool right);  // true if a region took it (or a scrollbar drag started)
    bool mouse_drag(float x, float y);              // true while a scrollbar or slider is being dragged
    void mouse_up();
    bool wheel(float x, float y, float delta_dips);
    int hit_index(float x, float y) const;  // which region is under the mouse (to repaint when it changes)

    // MARK: Widgets (Flutter equivalents in reference/wreckbox/app/lib/ui/theme.dart)
    enum class Pill { glass, primary, smart };
    void glass(const Rect& r, float radius = 24, bool smart = false, bool solid = false);
    float dot_label(const std::wstring& s, float x, float cy, D2D1_COLOR_F c = theme::text3, float size = 11);  // returns width
    float pill_width(const std::wstring& label, const wchar_t* icon = nullptr);
    float pill(float x, float y, const std::wstring& label, const wchar_t* icon, Pill style, std::function<void()> on_click);  // height 36
    float chip_width(const std::wstring& label, std::optional<int> count = std::nullopt);
    float chip(float x, float y, const std::wstring& label, std::optional<int> count, bool on, bool smart, std::function<void()> on_click);
    float key_badge(float x, float cy, const std::string& camelot, bool unsure = false, bool large = false);  // returns width
    void bpm_readout(float x, float cy, std::optional<double> bpm, bool unsure = false, float size = 17);
    void energy_meter(float x, float bottom, std::optional<double> value, float height = 12);
    void status_dot(float cx, float cy, TrackStatus s, float size = 16);
    void artwork(const LibraryTrack& t, const Rect& r, float radius = 8, float opacity = 1);
    void logo(float x, float y, float size, bool with_tile = true);
    void progress(const Rect& r, double value);
    // A round icon button; `on` draws it lit, `filled` white (the play button). Disabled without on_click.
    void icon_button(const Rect& r, const wchar_t* glyph, float size, std::function<void()> on_click, bool on = false, bool filled = false,
                     std::wstring tooltip = {});

private:
    struct Hit {
        Rect r;
        std::function<void()> click, right;
        std::function<void(float, float, bool)> slide;  // a slider: (x, y, done)
    };
    struct Scroll {
        Rect r;
        float* offset;
        float max;
    };
    Rect clip_or_all() const;

    struct Drag {
        float* offset = nullptr;
        float max = 0, track = 0, grab_y = 0, grab_offset = 0;
        std::function<void(float, float, bool)> slide;  // the slider being dragged
    };
    std::vector<Hit> hits_;
    struct Tip {
        Rect r;
        std::wstring text;
    };
    std::vector<Tip> tips_;
    Rect tip_at_{};  // the region the pointer is resting on, since tip_since_ (ms tick)
    unsigned long long tip_since_ = 0;
    bool tip_dismissed_ = false;
    std::vector<Scroll> scrolls_;
    std::vector<Rect> clips_;
    Drag drag_;
};

std::wstring wide(const std::string& s);  // UTF-8 → UTF-16 for drawing

}  // namespace wb::ui
