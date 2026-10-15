// The Web player: Spotify's and YouTube Music's own web players inside the app (WebView2, the Edge engine that comes
// with Windows). Each site is its own browser view, made the first time its tab opens and kept, so one keeps playing
// while you look at the other; sign-ins are kept between runs. Only one thing plays at a time: a site starting to play
// calls on_playing (the app pauses its own player), and the app starting pauses the sites.
#pragma once
#include <windows.h>
#include <wrl/client.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

struct ICoreWebView2Environment;
struct ICoreWebView2Controller;
struct ICoreWebView2;

namespace wb {

class WebPlayer {
public:
    enum Site { spotify, youtube, kSites };
    static const wchar_t* name(Site s);

    WebPlayer(HWND parent, std::filesystem::path data_dir);
    ~WebPlayer();

    std::function<void(Site)> on_playing;  // a site started playing sound (UI thread)
    std::function<void()> on_changed;      // a view became ready, or failed: repaint

    // Shows `site` in `px` (client pixels of the parent), hides the other; made on first use.
    void show(Site site, const RECT& px);
    void hide();
    void pause();                       // both sites
    void reload(Site site);
    void set_muted(bool muted);         // follows the app's mute
    bool playing(Site site) const { return views_[site].playing; }
    const std::wstring& error() const { return error_; }  // e.g. the WebView2 Runtime isn't installed
    bool loading(Site site) const { return !views_[site].ctl && error_.empty(); }

private:
    struct View {
        Microsoft::WRL::ComPtr<ICoreWebView2Controller> ctl;
        Microsoft::WRL::ComPtr<ICoreWebView2> wv;
        bool requested = false, playing = false;
    };
    void make(Site site);
    void place();

    HWND parent_;
    std::filesystem::path data_dir_;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> env_;
    bool env_requested_ = false, muted_ = false;
    View views_[kSites];
    Site shown_ = spotify;
    bool visible_ = false;
    RECT px_{};
    std::wstring error_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);  // false once gone: a late WebView2 callback does nothing
};

}  // namespace wb
