// Immediate-mode UI on top of Gfx. Each paint draws the whole screen from current state and records clickable and
// scrollable regions; mouse input is matched against the regions from the last paint.
//
// Motion: things ease instead of jumping (a hover lights up, a card lifts, a page arrives). anim() and enter() hold
// the in-between values from paint to paint; while any of them is still moving, animating() is true and the window
// paints again a moment later. Once everything has settled nothing repaints — idle CPU is still zero, unless the
// moving backdrop is on (View decides that: not on battery, not when the window is in the background).
#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
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

    // MARK: Motion
    double now = 0;      // seconds, steady; the same for the whole paint
    float dt = 0;        // seconds since the paint before (capped, so a long pause doesn't make things jump)
    bool motion = true;  // false (Settings → Motion → off): everything is at its end value at once
    // A name for something that moves: any text, plus a number to tell apart many of the same kind.
    static uint64_t key(std::string_view name, uint64_t n = 0);
    static uint64_t key(const Rect& r, uint64_t n = 0);  // for widgets with no better name than where they are
    // `target`, approached smoothly (`speed`: higher is quicker; 16 settles in about a fifth of a second). The first
    // time a key is seen it is at its target already.
    float anim(uint64_t k, float target, float speed = 16);
    // 0 → 1 once, from the first paint this key is seen, after `delay` seconds and over `duration` (eased out).
    // For things arriving: give each item of a row a slightly later delay and they come in one after another.
    float enter(uint64_t k, float delay = 0, float duration = 0.42f);
    // True while something drawn in this paint hasn't settled: the window should paint again soon.
    bool animating() const { return animating_; }
    void keep_animating() { animating_ = true; }  // something outside anim()/enter() is moving (the backdrop, a spinner)
    // A press on r since the last paint? (the moment a click landed, for a ripple or a pop)
    float press(const Rect& r);  // 0–1, 1 right after a click inside r, fading over ~0.3 s

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
    // While on, what is drawn is only for show (the shelf fading out behind an opening crate): click(), slider(), tip()
    // and scroll_area() record nothing and hover() is false, so it can't take input. begin_frame() turns it off.
    void set_inert(bool on) { inert_ = on; }

    // MARK: Input (window procedure)
    bool mouse_down(float x, float y, bool right);  // true if a region took it (or a scrollbar drag started)
    bool mouse_drag(float x, float y);              // true while a scrollbar or slider is being dragged
    void mouse_up();
    bool wheel(float x, float y, float delta_dips);
    int hit_index(float x, float y) const;  // which region is under the mouse (to repaint when it changes)

    // MARK: Widgets (original equivalents in the original source)
    enum class Pill { glass, primary, smart };
    void glass(const Rect& r, float radius = 24, bool smart = false, bool solid = false);
    // A glass card that answers the pointer: it lifts, its edge lights up in the accent colours and a soft light
    // follows the pointer across it. Returns how far it is lit (0–1), for the caller's own touches (a cover's tilt).
    float card(const Rect& r, float radius, uint64_t k, bool interactive = true);
    // How far the pointer is from r's centre, −1…1 each way (0,0 when it isn't over r): what a tilting card turns by.
    D2D1_POINT_2F lean(const Rect& r) const;
    // The same as artwork(), turned in space by `tilt` (degrees about the horizontal and vertical middles).
    void artwork_3d(const LibraryTrack& t, const Rect& r, float radius, D2D1_POINT_2F tilt, float opacity = 1, float depth = 4);
    // A ring that fills clockwise from the top to `value` (0–1), in the accent colours.
    void ring(float cx, float cy, float radius, double value, float width = 5);
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
    void progress(const Rect& r, double value, uint64_t k = 0);  // k: its name, if it should fill and follow smoothly
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
    struct Moving {
        float value = 0;
        double born = 0;
        unsigned seen = 0;  // the paint it was last asked for
    };
    std::unordered_map<uint64_t, Moving> moving_;
    unsigned frame_ = 0;
    bool animating_ = false;
    bool inert_ = false;  // set_inert()
    float press_x_ = -1, press_y_ = -1;
    double press_at_ = -10;
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
