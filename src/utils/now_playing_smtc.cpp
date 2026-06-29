/**
 * VitaPlex - Windows System Media Transport Controls backend for the OS
 * "Now Playing" bridge.
 *
 * Desktop equivalent of the Android media notification / Linux MPRIS: publishes
 * the current track to Windows' System Media Transport Controls (the media
 * overlay you get with the volume flyout) and receives the multimedia-key /
 * overlay buttons so playback is controllable when VitaPlex isn't focused.
 *
 * Uses the WinRT ABI through WRL (ComPtr/Callback/HStringReference) rather than
 * C++/WinRT, so it builds under the MinGW toolchain. SMTC is bound to our window
 * (GetForWindow), so setup + button events live on the UI thread, where SDL
 * already pumps the message loop; transport actions are still bounced through
 * brls::sync for consistency with the other backends.
 *
 * Compiled only when VITAPLEX_SMTC is defined (CMake feature-probe succeeded).
 */

#if defined(VITAPLEX_SMTC)

#include "utils/now_playing.hpp"

#include <borealis.hpp>

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

#include <windows.h>
#include <roapi.h>
#include <wrl.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.media.h>
#include <systemmediatransportcontrolsinterop.h>

#include <string>

namespace vitaplex {
namespace nowplaying {

namespace detail {
void smtcUpdate(const Info& info);
void smtcClear();
}

namespace {

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
using Microsoft::WRL::Wrappers::HStringReference;
namespace WM = ABI::Windows::Media;
namespace WF = ABI::Windows::Foundation;

ComPtr<WM::ISystemMediaTransportControls> g_smtc;
ComPtr<WM::ISystemMediaTransportControlsDisplayUpdater> g_updater;
ComPtr<WM::IMusicDisplayProperties> g_music;
bool g_init = false;     // SMTC wired up
bool g_failed = false;   // hard failure — stop retrying

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

HWND currentHwnd() {
    SDL_Window* w = SDL_GL_GetCurrentWindow();
    if (!w) return nullptr;
    SDL_SysWMinfo wmi;
    SDL_VERSION(&wmi.version);
    if (SDL_GetWindowWMInfo(w, &wmi) != SDL_TRUE) return nullptr;
    return wmi.info.win.window;
}

void onButton(WM::SystemMediaTransportControlsButton btn) {
    Transport t;
    switch (btn) {
        case WM::SystemMediaTransportControlsButton_Play:     t = Transport::Play;     break;
        case WM::SystemMediaTransportControlsButton_Pause:    t = Transport::Pause;    break;
        case WM::SystemMediaTransportControlsButton_Next:     t = Transport::Next;     break;
        case WM::SystemMediaTransportControlsButton_Previous: t = Transport::Previous; break;
        case WM::SystemMediaTransportControlsButton_Stop:     t = Transport::Stop;     break;
        default: return;
    }
    brls::sync([t]() { dispatchTransport(t); });
}

bool ensureInit() {
    if (g_init) return true;
    if (g_failed) return false;

    static bool s_ro = false;
    if (!s_ro) {
        s_ro = true;
        // SDL already inits COM as STA on the main thread; tolerate S_FALSE /
        // RPC_E_CHANGED_MODE — we only need a usable apartment for the factory.
        RoInitialize(RO_INIT_SINGLETHREADED);
    }

    HWND hwnd = currentHwnd();
    if (!hwnd) return false;   // window not up yet — retry on the next update()

    ComPtr<ISystemMediaTransportControlsInterop> interop;
    HRESULT hr = RoGetActivationFactory(
        HStringReference(RuntimeClass_Windows_Media_SystemMediaTransportControls).Get(),
        IID_PPV_ARGS(&interop));
    if (FAILED(hr) || !interop) { g_failed = true; return false; }

    hr = interop->GetForWindow(hwnd, IID_PPV_ARGS(&g_smtc));
    if (FAILED(hr) || !g_smtc) { g_failed = true; return false; }

    g_smtc->put_IsEnabled(true);
    g_smtc->get_DisplayUpdater(&g_updater);
    if (g_updater) {
        g_updater->put_Type(WM::MediaPlaybackType_Music);
        g_updater->get_MusicProperties(&g_music);
    }

    EventRegistrationToken token = {};
    auto handler = Callback<WF::ITypedEventHandler<
        WM::SystemMediaTransportControls*, WM::SystemMediaTransportControlsButtonPressedEventArgs*>>(
        [](WM::ISystemMediaTransportControls*,
           WM::ISystemMediaTransportControlsButtonPressedEventArgs* args) -> HRESULT {
            if (args) {
                WM::SystemMediaTransportControlsButton btn;
                if (SUCCEEDED(args->get_Button(&btn))) onButton(btn);
            }
            return S_OK;
        });
    if (handler) g_smtc->add_ButtonPressed(handler.Get(), &token);

    g_init = true;
    brls::Logger::info("SMTC: Windows media controls active");
    return true;
}

} // namespace

namespace detail {

void smtcUpdate(const Info& info) {
    if (!ensureInit()) return;

    g_smtc->put_IsPlayEnabled(true);
    g_smtc->put_IsPauseEnabled(true);
    g_smtc->put_IsNextEnabled(info.hasNext ? 1 : 0);
    g_smtc->put_IsPreviousEnabled(info.hasPrev ? 1 : 0);
    g_smtc->put_PlaybackStatus(info.playing ? WM::MediaPlaybackStatus_Playing
                                            : WM::MediaPlaybackStatus_Paused);
    if (g_music) {
        std::wstring t = widen(info.title), a = widen(info.artist), al = widen(info.album);
        g_music->put_Title(HStringReference(t.c_str()).Get());
        g_music->put_Artist(HStringReference(a.c_str()).Get());
        g_music->put_AlbumTitle(HStringReference(al.c_str()).Get());
    }
    if (g_updater) g_updater->Update();
}

void smtcClear() {
    if (!g_init || !g_smtc) return;
    g_smtc->put_PlaybackStatus(WM::MediaPlaybackStatus_Stopped);
    if (g_updater) {
        g_updater->ClearAll();
        g_updater->Update();
    }
}

} // namespace detail
} // namespace nowplaying
} // namespace vitaplex

#endif // VITAPLEX_SMTC
