// WreckBox design tokens: dark glass surfaces, Urbanist for UI text, Doto dot-matrix
// for labels and readouts, pastel gradient for smart features.
#pragma once
#include <cmath>
#include <cstdint>
#include <string>

#include <d2d1.h>

namespace wb::theme {

constexpr D2D1_COLOR_F argb(uint32_t c) {
    return {float((c >> 16) & 255) / 255.f, float((c >> 8) & 255) / 255.f, float(c & 255) / 255.f, float(c >> 24) / 255.f};
}
constexpr D2D1_COLOR_F with_alpha(D2D1_COLOR_F c, float a) { return {c.r, c.g, c.b, a}; }

inline constexpr auto bg = argb(0xFF08080A);
inline constexpr auto bg_raised = argb(0xFF111115);
inline constexpr auto text = argb(0xF0FFFFFF);
inline constexpr auto text2 = argb(0x99FFFFFF);
inline constexpr auto text3 = argb(0x5CFFFFFF);
inline constexpr auto hairline = argb(0x14FFFFFF);
inline constexpr auto glass_fill = argb(0x0BFFFFFF);
inline constexpr auto glass_border = argb(0x1FFFFFFF);
inline constexpr auto hover = argb(0x0FFFFFFF);
inline constexpr auto selected = argb(0x1AFFFFFF);  // white @ 10 %
inline constexpr auto white = argb(0xFFFFFFFF);
inline constexpr auto black = argb(0xFF000000);
inline constexpr auto peach = argb(0xFFEFAF86);
inline constexpr auto lilac = argb(0xFFBB96DA);
inline constexpr auto light_blue = argb(0xFFA9C8F0);

inline constexpr const wchar_t* ui_font = L"Urbanist";
inline constexpr const wchar_t* dot_font = L"Doto";
inline constexpr const wchar_t* icon_font = L"Segoe MDL2 Assets";  // built into Windows 10 and 11

inline D2D1_COLOR_F hsv(float h, float s, float v) {
    const float c = v * s, x = c * (1 - std::abs(std::fmod(h / 60.f, 2.f) - 1)), m = v - c;
    float r = 0, g = 0, b = 0;
    if (h < 60) r = c, g = x;
    else if (h < 120) r = x, g = c;
    else if (h < 180) g = c, b = x;
    else if (h < 240) g = x, b = c;
    else if (h < 300) r = x, b = c;
    else r = c, b = x;
    return {r + m, g + m, b + m, 1};
}

// Camelot wheel colour: the hue steps around the wheel like DJ software key colours.
inline D2D1_COLOR_F camelot(const std::string& code) {
    if (code.size() < 2) return text3;
    int n = 0;
    for (size_t i = 0; i + 1 < code.size(); ++i) {
        if (code[i] < '0' || code[i] > '9') return text3;
        n = n * 10 + (code[i] - '0');
    }
    const float hue = float(((n - 1) * 30 + 165) % 360);
    return code.back() == 'B' ? hsv(hue, 0.55f, 1.0f) : hsv(hue, 0.45f, 0.88f);
}

// Stable pastel tint for artwork placeholders.
inline D2D1_COLOR_F tint(const std::string& seed) {
    uint32_t h = 0;
    for (const unsigned char c : seed) h = (h * 31 + c) & 0x7fffffff;
    return hsv(float(h % 360), 0.35f, 0.75f);
}

// Segoe MDL2 Assets glyphs standing in for the Material icons the original build uses.
namespace icon {
inline constexpr const wchar_t* home = L"";
inline constexpr const wchar_t* list = L"";
inline constexpr const wchar_t* check_circle = L"";
inline constexpr const wchar_t* circle = L"";
inline constexpr const wchar_t* block = L"";
inline constexpr const wchar_t* music = L"";
inline constexpr const wchar_t* people = L"";
inline constexpr const wchar_t* queue = L"";
inline constexpr const wchar_t* download = L"";
inline constexpr const wchar_t* phone = L"";
inline constexpr const wchar_t* settings = L"";
inline constexpr const wchar_t* bug = L"";
inline constexpr const wchar_t* search = L"";
inline constexpr const wchar_t* close = L"";
inline constexpr const wchar_t* check = L"";
inline constexpr const wchar_t* scan = L"";
inline constexpr const wchar_t* tag = L"";
inline constexpr const wchar_t* folder = L"";
inline constexpr const wchar_t* undo = L"";
inline constexpr const wchar_t* sparkle = L"";
inline constexpr const wchar_t* arrow_right = L"";
inline constexpr const wchar_t* up = L"";
inline constexpr const wchar_t* down = L"";
inline constexpr const wchar_t* play = L"";
inline constexpr const wchar_t* open = L"";
inline constexpr const wchar_t* upload = L"";
inline constexpr const wchar_t* video = L"";
inline constexpr const wchar_t* sync = L"";
inline constexpr const wchar_t* pause = L"\uE769";
inline constexpr const wchar_t* previous = L"\uE892";
inline constexpr const wchar_t* next = L"\uE893";
inline constexpr const wchar_t* volume = L"\uE767";
inline constexpr const wchar_t* mute = L"\uE74F";
inline constexpr const wchar_t* equalizer = L"\uE9E9";
inline constexpr const wchar_t* visualizer = L"\uE8D6";
inline constexpr const wchar_t* add = L"\uE710";
inline constexpr const wchar_t* globe = L"\uE774";
inline constexpr const wchar_t* chevron_left = L"";
inline constexpr const wchar_t* chevron_right = L"";
inline constexpr const wchar_t* shuffle = L"";
inline constexpr const wchar_t* lock = L"";
inline constexpr const wchar_t* exit_full = L"\uE73F";
inline constexpr const wchar_t* chevron_up = L"\uE70E";
inline constexpr const wchar_t* chevron_down = L"\uE70D";
inline constexpr const wchar_t* trash = L"\uE74D";
}  // namespace icon

}  // namespace wb::theme
