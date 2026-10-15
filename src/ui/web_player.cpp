#include "ui/web_player.h"

#include <WebView2.h>
#include <wrl.h>

#include <fstream>

#include "model/model.h"
#include "model/paths.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace wb {

namespace {

void app_log(const std::string& line) { std::ofstream(paths::app_log(), std::ios::app) << iso_seconds_now() << " " << line << "\n"; }

const wchar_t* url(WebPlayer::Site s) { return s == WebPlayer::spotify ? L"https://open.spotify.com/" : L"https://music.youtube.com/"; }

// Spotify's own pause button keeps its player in step (its audio element may not be in the page); YouTube Music follows
// its <video> element.
constexpr const wchar_t* kPause = LR"((() => {
  const b = document.querySelector('[data-testid="control-button-playpause"]');
  if (b) b.click(); else document.querySelectorAll('video, audio').forEach(m => m.pause());
})())";

}  // namespace

const wchar_t* WebPlayer::name(Site s) { return s == spotify ? L"Spotify" : L"YouTube Music"; }

WebPlayer::WebPlayer(HWND parent, std::filesystem::path data_dir) : parent_(parent), data_dir_(std::move(data_dir)) {}

WebPlayer::~WebPlayer() {
    *alive_ = false;
    for (auto& v : views_)
        if (v.ctl) v.ctl->Close();
}

void WebPlayer::show(Site site, const RECT& px) {
    shown_ = site, visible_ = true, px_ = px;
    if (!views_[site].requested) make(site);
    place();
}

void WebPlayer::hide() {
    if (!visible_) return;
    visible_ = false;
    place();
}

void WebPlayer::place() {
    for (int s = 0; s < kSites; ++s) {
        auto& v = views_[s];
        if (!v.ctl) continue;
        const bool on = visible_ && s == shown_;
        if (on) v.ctl->put_Bounds(px_);
        v.ctl->put_IsVisible(on);  // a hidden view keeps playing
    }
}

void WebPlayer::pause() {
    for (auto& v : views_)
        if (v.wv && v.playing) v.wv->ExecuteScript(kPause, nullptr);
}

void WebPlayer::reload(Site site) {
    if (views_[site].wv) views_[site].wv->Navigate(url(site));
}

void WebPlayer::set_muted(bool muted) {
    if (muted == muted_) return;
    muted_ = muted;
    for (auto& v : views_) {
        ComPtr<ICoreWebView2_8> w8;
        if (v.wv && SUCCEEDED(v.wv.As(&w8))) w8->put_IsMuted(muted);
    }
}

void WebPlayer::make(Site site) {
    views_[site].requested = true;
    if (!env_) {
        if (env_requested_) return;  // the environment's callback makes the views asked for meanwhile
        env_requested_ = true;
        auto alive = alive_;
        const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, data_dir_.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([this, alive](HRESULT r, ICoreWebView2Environment* env) -> HRESULT {
                if (!*alive) return S_OK;
                if (FAILED(r) || !env) {
                    error_ = L"The web player couldn't start (WebView2 error " + std::to_wstring(unsigned(r)) + L").";
                    app_log("web player: environment failed " + std::to_string(unsigned(r)));
                } else {
                    env_ = env;
                    for (int s = 0; s < kSites; ++s)
                        if (views_[s].requested) make(Site(s));
                }
                if (on_changed) on_changed();
                return S_OK;
            }).Get());
        if (FAILED(hr)) {
            error_ = L"The web player needs the Microsoft Edge WebView2 Runtime. Install it from "
                     L"go.microsoft.com/fwlink/p/?LinkId=2124703 and open this page again.";
            app_log("web player: no WebView2 Runtime " + std::to_string(unsigned(hr)));
        }
        return;
    }
    auto alive = alive_;
    env_->CreateCoreWebView2Controller(
        parent_, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([this, alive, site](HRESULT r, ICoreWebView2Controller* c) -> HRESULT {
            if (!*alive) {
                if (c) c->Close();
                return S_OK;
            }
            if (FAILED(r) || !c) {
                error_ = L"The web player couldn't open " + std::wstring(name(site)) + L" (WebView2 error " + std::to_wstring(unsigned(r)) + L").";
                if (on_changed) on_changed();
                return S_OK;
            }
            auto& v = views_[site];
            v.ctl = c;
            v.ctl->get_CoreWebView2(&v.wv);
            ComPtr<ICoreWebView2_8> w8;
            if (SUCCEEDED(v.wv.As(&w8))) {
                w8->put_IsMuted(muted_);
                EventRegistrationToken t;
                w8->add_IsDocumentPlayingAudioChanged(Callback<ICoreWebView2IsDocumentPlayingAudioChangedEventHandler>(
                    [this, alive, site](ICoreWebView2* w, IUnknown*) -> HRESULT {
                        ComPtr<ICoreWebView2_8> x;
                        BOOL on = FALSE;
                        if (*alive && SUCCEEDED(ComPtr<ICoreWebView2>(w).As(&x))) x->get_IsDocumentPlayingAudio(&on);
                        if (!*alive) return S_OK;
                        views_[site].playing = on;
                        if (on) {  // the other site stops, and the app's player (on_playing)
                            auto& other = views_[site == spotify ? youtube : spotify];
                            if (other.wv && other.playing) other.wv->ExecuteScript(kPause, nullptr);
                            if (on_playing) on_playing(site);
                        }
                        if (on_changed) on_changed();
                        return S_OK;
                    }).Get(), &t);
            }
            v.wv->Navigate(url(site));
            place();
            if (on_changed) on_changed();
            return S_OK;
        }).Get());
}

}  // namespace wb
