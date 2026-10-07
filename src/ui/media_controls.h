// Windows' System Media Transport Controls for the window: the keyboard's media keys work while WreckBox is in the
// background, and the volume flyout / lock screen show the title, artist and cover with play / pause / next / previous.
#pragma once
#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>

namespace wb::ui {

inline constexpr UINT WM_APP_MEDIA = WM_APP + 4;  // wParam: the button pressed (APPCOMMAND_MEDIA_*)

class MediaControls {
public:
    MediaControls();
    ~MediaControls();
    bool init(HWND hwnd);  // false if this Windows doesn't offer them (WreckBox works without)

    struct State {
        bool active = false, playing = false, can_previous = false, can_next = false;
        std::wstring title, artist;
        std::optional<std::filesystem::path> cover;  // an image file
        bool operator==(const State&) const = default;
    };
    void update(const State& s);  // cheap when nothing changed
    void shutdown();              // before the window is destroyed

private:
    struct Impl;
    Impl* impl_ = nullptr;
    State last_;
};

}  // namespace wb::ui
