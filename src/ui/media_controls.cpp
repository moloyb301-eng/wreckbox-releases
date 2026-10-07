#include "ui/media_controls.h"

#include <shcore.h>
#include <shlwapi.h>
#include <systemmediatransportcontrolsinterop.h>
#include <windows.media.h>
#include <windows.storage.streams.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>

namespace wb::ui {

using namespace ABI::Windows::Media;
using namespace ABI::Windows::Storage::Streams;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;

struct MediaControls::Impl {
    ComPtr<ISystemMediaTransportControls> smtc;
    EventRegistrationToken token{};
};

MediaControls::MediaControls() = default;

MediaControls::~MediaControls() { shutdown(); }

// Must run while the window still exists: removing the handler from a transport-controls object whose window is gone
// crashed the app on exit.
void MediaControls::shutdown() {
    if (impl_ && impl_->smtc) {
        impl_->smtc->put_IsEnabled(false);
        impl_->smtc->remove_ButtonPressed(impl_->token);
    }
    delete impl_;
    impl_ = nullptr;
}

bool MediaControls::init(HWND hwnd) {
    ComPtr<ISystemMediaTransportControlsInterop> interop;
    if (FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Media_SystemMediaTransportControls).Get(), IID_PPV_ARGS(&interop))))
        return false;
    auto impl = new Impl;
    if (FAILED(interop->GetForWindow(hwnd, IID_PPV_ARGS(&impl->smtc)))) {
        delete impl;
        return false;
    }
    impl_ = impl;
    auto& s = impl_->smtc;
    s->put_IsEnabled(false);  // until something plays
    s->put_IsPlayEnabled(true);
    s->put_IsPauseEnabled(true);
    s->put_IsStopEnabled(true);
    // Presses arrive on a Windows thread: hand them to the window as the media-key commands it already knows.
    auto on_press = Callback<ABI::Windows::Foundation::ITypedEventHandler<SystemMediaTransportControls*, SystemMediaTransportControlsButtonPressedEventArgs*>>(
        [hwnd](ISystemMediaTransportControls*, ISystemMediaTransportControlsButtonPressedEventArgs* args) {
            SystemMediaTransportControlsButton b{};
            args->get_Button(&b);
            int cmd = 0;
            switch (b) {
                case SystemMediaTransportControlsButton_Play: cmd = APPCOMMAND_MEDIA_PLAY; break;
                case SystemMediaTransportControlsButton_Pause: cmd = APPCOMMAND_MEDIA_PAUSE; break;
                case SystemMediaTransportControlsButton_Stop: cmd = APPCOMMAND_MEDIA_STOP; break;
                case SystemMediaTransportControlsButton_Next: cmd = APPCOMMAND_MEDIA_NEXTTRACK; break;
                case SystemMediaTransportControlsButton_Previous: cmd = APPCOMMAND_MEDIA_PREVIOUSTRACK; break;
                default: return S_OK;
            }
            PostMessageW(hwnd, WM_APP_MEDIA, WPARAM(cmd), 0);
            return S_OK;
        });
    s->add_ButtonPressed(on_press.Get(), &impl_->token);
    return true;
}

void MediaControls::update(const State& st) {
    if (!impl_ || st == last_) return;
    const bool track_changed = st.title != last_.title || st.artist != last_.artist || st.cover != last_.cover;
    last_ = st;
    auto& s = impl_->smtc;
    s->put_IsEnabled(st.active);
    s->put_IsNextEnabled(st.can_next);
    s->put_IsPreviousEnabled(st.can_previous);
    s->put_PlaybackStatus(st.playing ? MediaPlaybackStatus_Playing : st.active ? MediaPlaybackStatus_Paused : MediaPlaybackStatus_Stopped);
    if (!track_changed) return;
    ComPtr<ISystemMediaTransportControlsDisplayUpdater> du;
    if (FAILED(s->get_DisplayUpdater(&du))) return;
    du->ClearAll();
    du->put_Type(MediaPlaybackType_Music);
    ComPtr<IMusicDisplayProperties> music;
    if (SUCCEEDED(du->get_MusicProperties(&music))) {
        music->put_Title(HStringReference(st.title.c_str()).Get());
        music->put_Artist(HStringReference(st.artist.c_str()).Get());
    }
    // The cover: the image file wrapped as a WinRT stream (no async file APIs needed).
    ComPtr<IStream> file;
    ComPtr<IRandomAccessStream> stream;
    ComPtr<IRandomAccessStreamReferenceStatics> refs;
    ComPtr<IRandomAccessStreamReference> ref;
    if (st.cover && SUCCEEDED(SHCreateStreamOnFileEx(st.cover->c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &file)) &&
        SUCCEEDED(CreateRandomAccessStreamOverStream(file.Get(), BSOS_DEFAULT, IID_PPV_ARGS(&stream))) &&
        SUCCEEDED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Storage_Streams_RandomAccessStreamReference).Get(), IID_PPV_ARGS(&refs))) &&
        SUCCEEDED(refs->CreateFromStream(stream.Get(), &ref)))
        du->put_Thumbnail(ref.Get());
    du->Update();
}

}  // namespace wb::ui
