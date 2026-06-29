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

#include <windows.h>
#include <roapi.h>
#include <wrl.h>
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

// Find this process's main top-level window without depending on SDL headers
// (SDL's include dir isn't on this TU's search path under MinGW). SMTC's
// GetForWindow just needs any valid HWND we own; the visible, unowned top-level
// window is the app window.
HWND g_appHwnd = nullptr;
BOOL CALLBACK enumWindowsProc(HWND hwnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) &&
        GetWindow(hwnd, GW_OWNER) == nullptr) {
        g_appHwnd = hwnd;
        return FALSE;  // found it — stop enumerating
    }
    return TRUE;
}

HWND currentHwnd() {
    g_appHwnd = nullptr;
    EnumWindows(enumWindowsProc, 0);
    return g_appHwnd;
}

void onButton(WM::SystemMediaTransportControlsButton btn) {
    Transport t;
    switch (btn) {
        case WM::SystemMediaTransportControlsButton_Play:        t = Transport::Play;        break;
        case WM::SystemMediaTransportControlsButton_Pause:       t = Transport::Pause;       break;
        case WM::SystemMediaTransportControlsButton_Next:        t = Transport::Next;        break;
        case WM::SystemMediaTransportControlsButton_Previous:    t = Transport::Previous;    break;
        case WM::SystemMediaTransportControlsButton_Stop:        t = Transport::Stop;        break;
        case WM::SystemMediaTransportControlsButton_FastForward: t = Transport::FastForward; break;
        case WM::SystemMediaTransportControlsButton_Rewind:      t = Transport::Rewind;      break;
        default: return;
    }
    brls::sync([t]() { dispatchTransport(t); });
}

// WinRT delegate for the ButtonPressed event, hand-rolled as a plain COM object.
// MinGW's WRL ships neither wrl/event.h (Callback) nor a usable RuntimeClass/Make,
// so we implement IUnknown + ITypedEventHandler::Invoke directly. The delegate's
// vtable is just IUnknown + Invoke (it derives from IUnknown, not IInspectable).
typedef WF::ITypedEventHandler<WM::SystemMediaTransportControls*,
                               WM::SystemMediaTransportControlsButtonPressedEventArgs*> ButtonDelegate;

class ButtonHandler : public ButtonDelegate {
    LONG m_ref = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ButtonDelegate)) {
            *ppv = static_cast<ButtonDelegate*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = (ULONG)InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(
        WM::ISystemMediaTransportControls*,
        WM::ISystemMediaTransportControlsButtonPressedEventArgs* args) override {
        if (args) {
            WM::SystemMediaTransportControlsButton btn;
            if (SUCCEEDED(args->get_Button(&btn))) onButton(btn);
        }
        return S_OK;
    }
};

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
    ButtonHandler* handler = new ButtonHandler();   // ref = 1
    g_smtc->add_ButtonPressed(handler, &token);     // the session takes its own ref
    handler->Release();                             // drop ours; the session keeps it

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
    g_smtc->put_IsStopEnabled(true);
    g_smtc->put_IsFastForwardEnabled(true);
    g_smtc->put_IsRewindEnabled(true);
    g_smtc->put_IsNextEnabled(info.hasNext ? 1 : 0);
    g_smtc->put_IsPreviousEnabled(info.hasPrev ? 1 : 0);
    g_smtc->put_PlaybackStatus(info.playing ? WM::MediaPlaybackStatus_Playing
                                            : WM::MediaPlaybackStatus_Paused);
    if (g_music) {
        std::wstring t = widen(info.title), a = widen(info.artist);
        g_music->put_Title(HStringReference(t.c_str()).Get());
        g_music->put_Artist(HStringReference(a.c_str()).Get());
        // AlbumTitle lives on IMusicDisplayProperties2, which mingw-w64's
        // IMusicDisplayProperties doesn't expose; title + artist are what the
        // SMTC overlay shows anyway.
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
