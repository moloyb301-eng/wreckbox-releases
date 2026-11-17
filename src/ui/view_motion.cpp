// Motion: the backdrop behind every page, the accent colours that follow the cover of the song in the player, and the
// rule for how much motion this PC should be doing right now.
//
// The backdrop is quiet on purpose, so the shelf in front of it is the one bold thing: the dark page and three soft,
// dim lights in the accent colours. The app paints only when something changes, and the lights are no exception: they
// drift only while music plays, and then only with motion set to Full (Settings → Motion), on mains power, not over
// Remote Desktop and with this window in front, which costs a repaint about 30 times a second. Otherwise they stand
// where they got to, nothing here asks for another frame, and the app is back to zero CPU at rest.
#include <algorithm>
#include <array>
#include <cmath>

#include "dasvis/palette.h"
#include "model/settings.h"
#include "ui/view.h"

namespace wb::ui {

using namespace theme;

namespace {

// A cover's colour, made fit to light a dark page: its hue kept, never too dim or too grey to see.
D2D1_COLOR_F as_light(const dasvis::Rgb& linear) {
    float c[3];
    for (int i = 0; i < 3; ++i) c[i] = std::pow(std::clamp(linear[size_t(i)], 0.f, 1.f), 1 / 2.2f);
    const float hi = std::max({c[0], c[1], c[2]}), lo = std::min({c[0], c[1], c[2]});
    if (hi < 0.02f) return lilac;
    const float lift = std::max(1.f, 0.85f / hi);
    for (float& v : c) v = std::min(1.f, v * lift);
    if (hi - lo < 0.12f * hi)  // nearly grey: lean it toward the app's lilac so it still reads as a colour
        return {0.6f * c[0] + 0.4f * lilac.r, 0.6f * c[1] + 0.4f * lilac.g, 0.6f * c[2] + 0.4f * lilac.b, 1};
    return {c[0], c[1], c[2], 1};
}

bool env_set(const wchar_t* name) {
    wchar_t v[4]{};
    return GetEnvironmentVariableW(name, v, 4) > 0;
}

}  // namespace

View::Motion View::motion_setting() const {
    const std::string m = Settings::current().extra.value("motion", "full");
    return m == "off" ? Motion::off : m == "reduced" ? Motion::reduced : Motion::full;
}

void View::set_motion(Motion m) {
    Settings::current().extra["motion"] = m == Motion::off ? "off" : m == Motion::reduced ? "reduced" : "full";
    try {
        Settings::current().save();
    } catch (const std::exception&) {
        // The choice still holds until the app closes.
    }
}

View::Motion View::motion_now() const {
    const Motion m = motion_setting();
    if (m != Motion::full) return m;
    SYSTEM_POWER_STATUS power{};
    const bool saving = GetSystemPowerStatus(&power) && (power.ACLineStatus == 0 || power.SystemStatusFlag == 1);
    return saving || GetSystemMetrics(SM_REMOTESESSION) ? Motion::reduced : Motion::full;
}

bool View::backdrop_moving() const {
    if (fullscreen_ || motion_now() != Motion::full || IsIconic(hwnd_)) return false;
    static const bool always = env_set(L"WRECKBOX_RENDER_HIDDEN");  // a test window behind others: move anyway
    return always || GetForegroundWindow() == hwnd_;
}

void View::follow_cover() {
    // The loaded song's colours, playing or paused: a pause (often Space, many times a day) dims the lights but doesn't
    // wash the colours away and back. They return to the app's own when the player stops.
    const player::Item* cur = player_.current();
    const std::string id = cur && cur->track_id ? *cur->track_id : std::string();
    if (id == accent_for_) return;
    accent_for_ = id;
    if (id.empty()) {
        accent_target_ = {lilac, light_blue, peach};
        return;
    }
    const auto t = store_.track(id);
    if (!t) return;
    const auto file = store_.artwork_file(*t);
    auto found = std::make_shared<std::optional<std::array<D2D1_COLOR_F, 3>>>();
    jobs_.run(
        [file, found] {
            const auto image = decode_image(file, 64);
            if (!image) return;
            UINT w = 0, h = 0;
            image->GetSize(&w, &h);
            ComPtr<IWICBitmapLock> lock;
            WICRect all{0, 0, INT(w), INT(h)};
            if (!w || !h || FAILED(image->Lock(&all, WICBitmapLockRead, &lock))) return;
            UINT size = 0, stride = 0;
            BYTE* px = nullptr;
            if (FAILED(lock->GetDataPointer(&size, &px)) || FAILED(lock->GetStride(&stride)) || !px) return;
            std::vector<uint8_t> tight(size_t(w) * h * 4);  // rows without padding, as the palette code reads them
            for (UINT y = 0; y < h; ++y) std::copy_n(px + size_t(y) * stride, size_t(w) * 4, tight.data() + size_t(y) * w * 4);
            if (const auto p = dasvis::cover_palette(tight.data(), int(w), int(h))) *found = {as_light((*p)[3]), as_light((*p)[4]), as_light((*p)[2])};
        },
        [this, id, found] {
            if (*found && accent_for_ == id) accent_target_ = **found;
        });
}

void View::backdrop(float W, float H) {
    follow_cover();
    g_.fill(Rect{0, 0, W, H}, bg);
    // Under full motion the lights' colour and brightness ease, which repaints the whole window at 60 fps until they
    // settle. Otherwise (reduced is forced on battery) the backdrop stands still: they are simply where they should be.
    const bool full = motion_now() == Motion::full;
    auto ease = [&](uint64_t k, float target, float speed) { return full ? ui_.anim(k, target, speed) : target; };
    // The three accent colours, each channel easing toward where it should be: a new song's colours wash in over a
    // couple of seconds rather than snapping.
    std::array<D2D1_COLOR_F, 3> accent;
    for (uint64_t i = 0; i < 3; ++i)
        accent[i] = {ease(Ui::key("accent-r", i), accent_target_[i].r, 1.6f), ease(Ui::key("accent-g", i), accent_target_[i].g, 1.6f),
                     ease(Ui::key("accent-b", i), accent_target_[i].b, 1.6f), 1};

    // The lights drift while music plays, where the backdrop may move at all; otherwise they stand where they got to.
    // No pointer parallax: it would ease for a second or two after every hover, repainting the whole window for nothing.
    const bool drifting = player_.playing() && backdrop_moving();
    if (drifting) {
        if (drift_at_ >= 0) backdrop_time_ += std::clamp(ui_.now - drift_at_, 0.0, 0.1);  // a long gap (a stall) is not a jump
        drift_at_ = ui_.now;
    } else {
        drift_at_ = -1;
    }
    const float t = float(backdrop_time_);
    const float unit = std::min(W, H);
    // While music plays the lights come up a little: the room answers the music, then settles when it stops. Over about
    // a second: this follows play / pause, so a slower swell would keep the window repainting long after each press.
    const float live = ease(Ui::key("backdrop-live"), player_.playing() ? 1.f : 0.f, 3);

    // Three big soft glows on slow loops, each in one of the accent colours: two in opposite corners, framing the page,
    // and a fainter one between them. Dim enough that text and crates in front read as if on the plain page.
    struct Light {
        float x, y, size, alpha, speed, phase;
        int colour;
    };
    static constexpr Light lights[] = {
        {0.90f, 0.08f, 0.90f, 0.150f, 0.031f, 0.0f, 0},
        {0.10f, 0.92f, 0.85f, 0.110f, 0.024f, 1.7f, 1},
        {0.56f, 0.50f, 0.60f, 0.050f, 0.038f, 3.1f, 2},
    };
    for (const Light& l : lights) {
        const float cx = W * (l.x + 0.06f * std::sin(t * l.speed * 6.283f + l.phase));
        const float cy = H * (l.y + 0.05f * std::cos(t * l.speed * 5.1f + l.phase * 1.3f));
        g_.fill_glow(cx, cy, unit * l.size, with_alpha(accent[size_t(l.colour)], l.alpha * (1 + 0.3f * live)));
    }
    backdrop_wants_frames_ = drifting;
}

void View::schedule_motion() {
    if (fullscreen_) return;  // the visualizer paces itself
    // Easing things want the next frame at once (a 60 Hz timer); the backdrop alone is fine at 30.
    const UINT ms = ui_.animating() ? 15 : backdrop_wants_frames_ ? 33 : 0;
    if (ms) SetTimer(hwnd_, kMotionTimer, ms, nullptr);
}

}  // namespace wb::ui
