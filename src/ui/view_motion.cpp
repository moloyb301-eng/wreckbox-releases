// Motion: the living backdrop behind every page, the accent colours that follow the cover of what's playing, and the
// rule for how much of it this PC should be doing right now.
//
// The app used to paint only when something changed. It still does once everything has settled — unless the backdrop
// is moving, which costs a repaint about 30 times a second. So the backdrop moves only when it's worth it: motion set
// to Full (Settings → Motion), on mains power, not over Remote Desktop, and with this window in front. Otherwise it
// stands still (a still frame of the same picture) and the app is back to zero CPU at rest.
#include <array>
#include <cmath>
#include <format>

#include "dasvis/palette.h"
#include "model/settings.h"
#include "ui/view.h"

namespace wb::ui {

using namespace theme;

namespace {

// A steady pseudo-random number in 0–1 for item `i`, stream `s`: where a mote sits, how fast it drifts.
float scatter(uint32_t i, uint32_t s) {
    uint32_t h = i * 374761393u + s * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xFFFFFF) / float(0x1000000);
}

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
    const player::Item* cur = player_.current();
    const std::string id = cur && cur->track_id && player_.playing() ? *cur->track_id : std::string();
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
    // The three accent colours, each channel easing toward where it should be: a new song's colours wash in over a
    // couple of seconds rather than snapping.
    std::array<D2D1_COLOR_F, 3> accent;
    for (uint64_t i = 0; i < 3; ++i)
        accent[i] = {ui_.anim(Ui::key("accent-r", i), accent_target_[i].r, 1.6f), ui_.anim(Ui::key("accent-g", i), accent_target_[i].g, 1.6f),
                     ui_.anim(Ui::key("accent-b", i), accent_target_[i].b, 1.6f), 1};

    const bool moving = backdrop_moving();
    if (moving) backdrop_time_ += double(ui_.dt);  // stands still (at the frame it reached) whenever it mustn't move
    const float t = float(backdrop_time_);
    // The pointer shifts the layers by different amounts, nearer ones more: depth.
    const float px = ui_.mx < 0 ? 0 : ui_.anim(Ui::key("parallax-x"), ui_.mx / W - 0.5f, 3), py = ui_.my < 0 ? 0 : ui_.anim(Ui::key("parallax-y"), ui_.my / H - 0.5f, 3);
    const float unit = std::min(W, H);
    // While music plays the lights come up a little.
    const float live = ui_.anim(Ui::key("backdrop-live"), player_.playing() ? 1.f : 0.f, 1.2f);

    // Lights: big soft glows drifting on slow loops, each in one of the accent colours.
    struct Light {
        float x, y, size, alpha, speed, phase, depth;
        int colour;
    };
    static constexpr Light lights[] = {
        {0.92f, 0.06f, 0.95f, 0.300f, 0.050f, 0.0f, 18, 0}, {0.30f, 0.98f, 0.85f, 0.220f, 0.037f, 1.7f, 30, 1},
        {0.62f, 0.50f, 0.60f, 0.120f, 0.061f, 3.1f, 46, 2}, {0.06f, 0.20f, 0.55f, 0.150f, 0.043f, 4.4f, 24, 1},
        {0.80f, 0.90f, 0.55f, 0.170f, 0.055f, 5.6f, 38, 2},
    };
    for (const Light& l : lights) {
        const float cx = W * (l.x + 0.085f * std::sin(t * l.speed * 6.283f + l.phase)) - px * l.depth;
        const float cy = H * (l.y + 0.070f * std::cos(t * l.speed * 5.1f + l.phase * 1.3f)) - py * l.depth;
        g_.fill_glow(cx, cy, unit * l.size, with_alpha(accent[size_t(l.colour)], l.alpha * (1 + 0.45f * live)));
    }

    // The deck: a great record lying flat in the lower right, seen at an angle — its grooves as rings, a label in the
    // accent colours at its heart, and glints that go round with the music (and drift slowly when there is none).
    {
        const float cx = W * 0.80f - px * 34, cy = H * 0.93f - py * 34, R = unit * 0.78f;
        const float turn = spin_ + t * 5;
        const float squash = 0.30f;  // a circle seen from the side is an ellipse
        // The grooves never change, so they are drawn once into a bitmap (already squashed) and only moved afterwards.
        const Rect plate = Rect::xywh(cx - R, cy - R * squash, R * 2, R * squash * 2);
        g_.cached(std::format(L"deck{:.0f}", R), plate, [&](Gfx& gg) {
            gg.push_transform(D2D1::Matrix3x2F::Scale(1, squash, {cx, cy}));
            gg.fill_circle(cx, cy, R, argb(0x30000000));
            const int grooves = 22;
            for (int k = 0; k <= grooves; ++k)
                gg.stroke_circle(cx, cy, R * (0.26f + 0.74f * float(k) / float(grooves)), with_alpha(white, k % 4 == 0 ? 0.075f : 0.035f),
                                 k == grooves ? 2.5f : 1.2f);
            gg.pop_transform();
        });
        g_.push_transform(D2D1::Matrix3x2F::Scale(1, squash, {cx, cy}));
        // Glints: a few arcs, turning with the record (arcs are the costly part, so only these move).
        for (int k = 1; k <= 22; k += 7) {
            const float rr = R * (0.26f + 0.74f * float(k) / 22.f);
            g_.arc(cx, cy, rr, turn + float(k) * 2, 34, with_alpha(accent[size_t(k % 3)], 0.20f + 0.14f * live), 1.8f);
            g_.arc(cx, cy, rr, turn + 180 + float(k) * 2, 34, with_alpha(accent[size_t((k + 1) % 3)], 0.14f + 0.10f * live), 1.8f);
        }
        g_.fill_circle(cx, cy, R * 0.22f, with_alpha(accent[0], 0.16f + 0.10f * live));
        g_.fill_circle(cx, cy, R * 0.12f, with_alpha(accent[2], 0.20f + 0.10f * live));
        g_.fill_circle(cx, cy, R * 0.025f, argb(0xC008080A));
        g_.pop_transform();
        g_.fill_glow(cx, cy, R * 0.5f, with_alpha(accent[0], 0.10f + 0.10f * live));
    }

    // Motes: small points of light rising slowly, the near ones bigger, faster and moved more by the pointer.
    if (motion_now() != Motion::off) {
        const int count = int(std::clamp(W * H / 26000.f, 18.f, 64.f));
        for (int i = 0; i < count; ++i) {
            const uint32_t u = uint32_t(i);
            const float depth = 0.25f + 0.75f * scatter(u, 1);  // 0.25 far … 1 near
            const float x0 = scatter(u, 2), y0 = scatter(u, 3);
            const float rise = t * (0.004f + 0.012f * depth);
            float y = y0 - rise;
            y -= std::floor(y);  // wraps: off the top, in again at the bottom
            const float x = x0 + 0.012f * std::sin(t * (0.11f + 0.2f * scatter(u, 4)) + 6.283f * scatter(u, 5));
            const float twinkle = 0.55f + 0.45f * std::sin(t * (0.5f + scatter(u, 6)) + 6.283f * scatter(u, 7));
            const float edge = std::min(1.f, std::min(y, 1 - y) * 8);  // fades at the top and bottom, so nothing pops
            g_.fill_circle(W * x - px * 60 * depth, H * y - py * 60 * depth, 0.8f + 1.8f * depth,
                           with_alpha(accent[size_t(i % 3)], (0.10f + 0.38f * depth) * twinkle * edge));
        }
    }
    backdrop_wants_frames_ = moving;
}

void View::schedule_motion() {
    if (fullscreen_) return;  // the visualizer paces itself
    // Easing things want the next frame at once (a 60 Hz timer); the backdrop alone is fine at 30.
    const UINT ms = ui_.animating() ? 15 : backdrop_wants_frames_ ? 33 : 0;
    if (ms) SetTimer(hwnd_, kMotionTimer, ms, nullptr);
}

}  // namespace wb::ui
