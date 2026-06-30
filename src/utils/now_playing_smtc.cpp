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
#include <shobjidl.h>                    // SetCurrentProcessExplicitAppUserModelID, SHGetPropertyStoreForWindow
#include <roapi.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.media.h>
#include <systemmediatransportcontrolsinterop.h>
#if defined(VITAPLEX_SMTC_THUMB)
#include <windows.foundation.h>          // Windows.Foundation.Uri
#include <windows.storage.streams.h>     // RandomAccessStreamReference
#endif
#if defined(VITAPLEX_SMTC_WINID)
#include <propsys.h>                     // IPropertyStore (tag the window's AppUserModelID)
#endif

#include <string>

namespace vitaplex {
namespace nowplaying {

namespace detail {
void smtcUpdate(const Info& info);
void smtcClear();
void smtcInitAppIdentity();
}

namespace {

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
namespace WM = ABI::Windows::Media;
namespace WF = ABI::Windows::Foundation;

ComPtr<WM::ISystemMediaTransportControls> g_smtc;
#if defined(VITAPLEX_SMTC_MODES)
ComPtr<WM::ISystemMediaTransportControls2> g_smtc2;  // AutoRepeatMode + ShuffleEnabled (revision 2)
#endif
ComPtr<WM::ISystemMediaTransportControlsDisplayUpdater> g_updater;
ComPtr<WM::IMusicDisplayProperties> g_music;
bool g_init = false;     // SMTC wired up
bool g_failed = false;   // hard failure — stop retrying

constexpr const wchar_t* kAumid = L"VitaPlex";   // app identity for the media overlay

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

#if defined(VITAPLEX_SMTC_MODES)
// The overlay's repeat + shuffle toggles hand back an explicit requested state.
// Same hand-rolled-COM-delegate pattern as ButtonHandler (no WRL Callback on
// MinGW), one per event.
typedef WF::ITypedEventHandler<WM::SystemMediaTransportControls*,
        WM::AutoRepeatModeChangeRequestedEventArgs*> RepeatDelegate;
typedef WF::ITypedEventHandler<WM::SystemMediaTransportControls*,
        WM::ShuffleEnabledChangeRequestedEventArgs*> ShuffleDelegate;

class RepeatHandler : public RepeatDelegate {
    LONG m_ref = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(RepeatDelegate)) {
            *ppv = static_cast<RepeatDelegate*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = (ULONG)InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(
        WM::ISystemMediaTransportControls*,
        WM::IAutoRepeatModeChangeRequestedEventArgs* args) override {
        if (args) {
            WM::MediaPlaybackAutoRepeatMode m;
            if (SUCCEEDED(args->get_RequestedAutoRepeatMode(&m))) {
                RepeatMode rm = (m == WM::MediaPlaybackAutoRepeatMode_Track) ? RepeatMode::One
                              : (m == WM::MediaPlaybackAutoRepeatMode_List)  ? RepeatMode::All
                                                                             : RepeatMode::Off;
                brls::sync([rm]() { dispatchSetRepeat(rm); });
            }
        }
        return S_OK;
    }
};

class ShuffleHandler : public ShuffleDelegate {
    LONG m_ref = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ShuffleDelegate)) {
            *ppv = static_cast<ShuffleDelegate*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = (ULONG)InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(
        WM::ISystemMediaTransportControls*,
        WM::IShuffleEnabledChangeRequestedEventArgs* args) override {
        if (args) {
            boolean on = 0;
            if (SUCCEEDED(args->get_RequestedShuffleEnabled(&on)))
                brls::sync([on]() { dispatchSetShuffle(on != 0); });
        }
        return S_OK;
    }
};
#endif // VITAPLEX_SMTC_MODES

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

    detail::smtcInitAppIdentity();  // ensure identity is set (no-op if main() already did)

    HWND hwnd = currentHwnd();
    if (!hwnd) return false;   // window not up yet — retry on the next update()

#if defined(VITAPLEX_SMTC_WINID)
    // Belt-and-suspenders: tag the window itself with our AppUserModelID. The
    // early process-wide AUMID (set in main() before the window existed) is
    // normally what the overlay resolves, but stamping the window directly
    // covers cases where it was created with a different identity.
    {
        ComPtr<IPropertyStore> store;
        if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store))) && store) {
            // PKEY_AppUserModel_ID = {9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3}, pid 5
            PROPERTYKEY key = { {0x9F4C2855, 0x9F79, 0x4B39,
                                 {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5 };
            PROPVARIANT pv;
            pv.vt = VT_LPWSTR;
            pv.pwszVal = const_cast<PWSTR>(kAumid);  // SetValue copies the string
            store->SetValue(key, pv);
            store->Commit();
        }
    }
#endif

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

#if defined(VITAPLEX_SMTC_MODES)
    // Repeat + shuffle live on the revision-2 interface; subscribe to their
    // change-requested events so the overlay toggles drive the queue.
    if (SUCCEEDED(g_smtc.As(&g_smtc2)) && g_smtc2) {
        EventRegistrationToken rt = {}, st = {};
        RepeatHandler* rh = new RepeatHandler();
        g_smtc2->add_AutoRepeatModeChangeRequested(rh, &rt);
        rh->Release();
        ShuffleHandler* sh = new ShuffleHandler();
        g_smtc2->add_ShuffleEnabledChangeRequested(sh, &st);
        sh->Release();
    }
#endif

    g_init = true;
    brls::Logger::info("SMTC: Windows media controls active");
    return true;
}

#if defined(VITAPLEX_SMTC_THUMB)
namespace WSS = ABI::Windows::Storage::Streams;

// Point the overlay's artwork at the cover. Windows fetches it itself from the
// URI, so http(s) Plex thumbnails work directly; a local downloaded cover is
// handed over as a file:/// URI. Best-effort — any failure just leaves no art.
void setThumbnail(const std::string& artUrl) {
    if (artUrl.empty() || !g_updater) return;

    std::string u;
    if (artUrl.rfind("http://", 0) == 0 || artUrl.rfind("https://", 0) == 0 ||
        artUrl.rfind("file:", 0) == 0) {
        u = artUrl;
    } else {
        std::string p = artUrl;
        for (char& c : p) if (c == '\\') c = '/';
        u = "file:///" + p;
    }
    std::wstring wu = widen(u);

    ComPtr<WF::IUriRuntimeClassFactory> uriFactory;
    if (FAILED(RoGetActivationFactory(
            HStringReference(RuntimeClass_Windows_Foundation_Uri).Get(),
            IID_PPV_ARGS(&uriFactory))) || !uriFactory) return;
    ComPtr<WF::IUriRuntimeClass> uri;
    if (FAILED(uriFactory->CreateUri(HStringReference(wu.c_str()).Get(), &uri)) || !uri) return;

    ComPtr<WSS::IRandomAccessStreamReferenceStatics> rasrStatics;
    if (FAILED(RoGetActivationFactory(
            HStringReference(RuntimeClass_Windows_Storage_Streams_RandomAccessStreamReference).Get(),
            IID_PPV_ARGS(&rasrStatics))) || !rasrStatics) return;
    ComPtr<WSS::IRandomAccessStreamReference> streamRef;
    if (FAILED(rasrStatics->CreateFromUri(uri.Get(), &streamRef)) || !streamRef) return;

    g_updater->put_Thumbnail(streamRef.Get());
}
#endif

} // namespace

namespace detail {

// Give the overlay a real app name. An unpackaged Win32 app with no
// AppUserModelID shows up as "unknown app"; we register an explicit AUMID + its
// DisplayName under HKCU so Windows resolves "VitaPlex". Call this before any
// window is created (from main()) — the window inherits the process AUMID at
// creation time, which is what the media overlay reads. Idempotent + best-effort.
void smtcInitAppIdentity() {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
            L"Software\\Classes\\AppUserModelId\\VitaPlex",
            0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS && key) {
        RegSetValueExW(key, L"DisplayName", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(kAumid),
                       (DWORD)((wcslen(kAumid) + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
    SetCurrentProcessExplicitAppUserModelID(kAumid);
}

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
#if defined(VITAPLEX_SMTC_THUMB)
        // AlbumTitle lives on the IMusicDisplayProperties2 revision.
        ComPtr<WM::IMusicDisplayProperties2> music2;
        if (SUCCEEDED(g_music.As(&music2)) && music2) {
            std::wstring al = widen(info.album);
            music2->put_AlbumTitle(HStringReference(al.c_str()).Get());
        }
#endif
    }
#if defined(VITAPLEX_SMTC_MODES)
    // Reflect the queue's repeat/shuffle on the overlay toggles (music only).
    if (g_smtc2) {
        if (info.showRepeat) {
            WM::MediaPlaybackAutoRepeatMode m =
                info.repeat == RepeatMode::One ? WM::MediaPlaybackAutoRepeatMode_Track :
                info.repeat == RepeatMode::All ? WM::MediaPlaybackAutoRepeatMode_List  :
                                                 WM::MediaPlaybackAutoRepeatMode_None;
            g_smtc2->put_AutoRepeatMode(m);
        }
        if (info.showShuffle) g_smtc2->put_ShuffleEnabled(info.shuffle ? 1 : 0);
    }
#endif
#if defined(VITAPLEX_SMTC_THUMB)
    setThumbnail(info.artUrl);
#endif
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
