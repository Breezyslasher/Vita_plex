/**
 * VitaPlex - shell integration, Windows backend (see the header).
 *
 * Three pieces, in increasing order of how much Windows asks for first:
 *
 *   Taskbar progress   ITaskbarList3::SetProgressValue draws the bar straight
 *                      over the taskbar button. Pure runtime COM — nothing is
 *                      registered, nothing is left behind.
 *
 *   AppUserModelID     One call at startup. It is what the shell uses to tie a
 *                      running window to a launcher entry, so it fixes taskbar
 *                      grouping and pinning on its own — and it is the identity
 *                      a toast is sent as.
 *
 *   Toast              Windows will not show a toast from an unpackaged app
 *                      unless a Start Menu shortcut exists carrying the same
 *                      AppUserModelID. That is a real file in the user's
 *                      profile, so it is the one part of this that a portable
 *                      build would otherwise never write; see ensureShortcut.
 *
 * Where the toast headers are missing (older mingw-w64 ships no
 * windows.ui.notifications.h) VITAPLEX_HAVE_TOAST is undefined and notify()
 * falls back to flashing the taskbar button, which needs nothing but an HWND.
 */

#include "utils/shell_integration.hpp"

#if defined(_WIN32)

#include <borealis.hpp>

#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl.h>

#if defined(VITAPLEX_HAVE_TOAST)
#include <roapi.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.ui.notifications.h>
#include <windows.data.xml.dom.h>
#endif

#include <string>

namespace vitaplex {
namespace shell {

namespace {

using Microsoft::WRL::ComPtr;

// The identity everything here keys off. It must match between the process,
// the Start Menu shortcut and the toast notifier, or Windows silently drops
// the toast — "silently" being the whole difficulty of debugging this.
constexpr const wchar_t* kAumid = L"Breezyslasher.VitaPlex";
constexpr const wchar_t* kShortcutName = L"\\VitaPlex.lnk";

// Find this process's main top-level window. Same approach as the SMTC
// backend, and for the same reason: the taskbar calls want an HWND we own,
// and reaching into the window backend's headers from this TU is more
// coupling than the job needs.
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

HWND appHwnd() {
    g_appHwnd = nullptr;
    EnumWindows(enumWindowsProc, 0);
    return g_appHwnd;
}

// COM on this thread. Every entry point below is called from the UI thread via
// brls::sync, so one apartment covers all of them. Apartment-threaded because
// the shell objects want it; a second call returning RPC_E_CHANGED_MODE just
// means someone else got there first, which is fine.
bool ensureCom() {
    static bool s_done = false;
    if (!s_done) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        s_done = true;
    }
    return true;
}

ComPtr<ITaskbarList3> taskbar() {
    static ComPtr<ITaskbarList3> s_tb;
    static bool s_tried = false;
    if (!s_tried) {
        s_tried = true;
        ensureCom();
        ComPtr<ITaskbarList3> tb;
        if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&tb))) &&
            SUCCEEDED(tb->HrInit())) {
            s_tb = tb;
        } else {
            brls::Logger::debug("shell: no ITaskbarList3 — taskbar progress disabled");
        }
    }
    return s_tb;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::wstring exePath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring();
}

// %APPDATA%\Microsoft\Windows\Start Menu\Programs\VitaPlex.lnk
std::wstring shortcutPath() {
    wchar_t programs[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, SHGFP_TYPE_CURRENT, programs)))
        return std::wstring();
    return std::wstring(programs) + kShortcutName;
}

// Settings-controlled, Windows-only. On by default so toasts work out of the box.
bool g_shortcutAllowed = true;

// Does this existing shortcut already carry our AppUserModelID? A shortcut
// without it is worse than none: Windows finds it, cannot attribute the app,
// and drops every toast without a word. One left by an installer, or made by
// hand from the exe, looks fine in the Start Menu and silently breaks
// notifications — so an existing file is inspected and repaired, never trusted.
bool shortcutHasAumid(const std::wstring& path, bool repair) {
    ensureCom();
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link))))
        return false;
    ComPtr<IPersistFile> file;
    if (FAILED(link.As(&file))) return false;
    if (FAILED(file->Load(path.c_str(), repair ? STGM_READWRITE : STGM_READ))) return false;

    ComPtr<IPropertyStore> store;
    if (FAILED(link.As(&store))) return false;

    PROPVARIANT pv;
    PropVariantInit(&pv);
    bool ok = false;
    if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR &&
        pv.pwszVal && wcscmp(pv.pwszVal, kAumid) == 0) {
        ok = true;
    }
    PropVariantClear(&pv);
    if (ok || !repair) return ok;

    // Present but unstamped (or stamped with something else) — put ours on it.
    PROPVARIANT set;
    if (FAILED(InitPropVariantFromString(kAumid, &set))) return false;
    HRESULT hr = store->SetValue(PKEY_AppUserModel_ID, set);
    PropVariantClear(&set);
    if (FAILED(hr) || FAILED(store->Commit())) return false;
    if (FAILED(file->Save(path.c_str(), TRUE))) return false;
    brls::Logger::info("shell: stamped the AppUserModelID onto an existing Start Menu shortcut");
    return true;
}

// The Start Menu shortcut a toast needs. Per-user, so no elevation; created if
// absent, repaired if present without our id.
bool ensureShortcut() {
    if (!g_shortcutAllowed) return false;
    const std::wstring path = shortcutPath();
    if (path.empty()) return false;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        return shortcutHasAumid(path, /*repair=*/true);

    const std::wstring exe = exePath();
    if (exe.empty()) return false;

    ensureCom();
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link))))
        return false;

    link->SetPath(exe.c_str());
    // Working directory matters: the app resolves its resources relative to it.
    std::wstring dir = exe;
    const size_t slash = dir.find_last_of(L'\\');
    if (slash != std::wstring::npos) {
        dir.resize(slash);
        link->SetWorkingDirectory(dir.c_str());
    }
    link->SetDescription(L"VitaPlex");

    ComPtr<IPropertyStore> store;
    if (FAILED(link.As(&store))) return false;
    PROPVARIANT pv;
    if (FAILED(InitPropVariantFromString(kAumid, &pv))) return false;
    // SetValue takes the variant by const reference, not by pointer.
    HRESULT hr = store->SetValue(PKEY_AppUserModel_ID, pv);
    PropVariantClear(&pv);
    if (FAILED(hr) || FAILED(store->Commit())) return false;

    ComPtr<IPersistFile> file;
    if (FAILED(link.As(&file))) return false;
    if (FAILED(file->Save(path.c_str(), TRUE))) return false;

    brls::Logger::info("shell: wrote the Start Menu shortcut toasts are keyed on");
    return true;
}

// Flash the taskbar button. The fallback when a toast cannot be shown — it
// needs nothing registered, and "something finished, look here" is most of
// what the notification was for.
void flashTaskbar() {
    HWND hwnd = appHwnd();
    if (!hwnd) return;
    // Only if we're not already the foreground window: flashing the window the user is looking at is pure noise.
    if (GetForegroundWindow() == hwnd) return;
    FLASHWINFO fi = {};
    fi.cbSize    = sizeof(fi);
    fi.hwnd      = hwnd;
    fi.dwFlags   = FLASHW_TRAY | FLASHW_TIMERNOFG;  // until the user looks at it
    fi.uCount    = 3;
    fi.dwTimeout = 0;
    FlashWindowEx(&fi);
}

#if defined(VITAPLEX_HAVE_TOAST)

namespace WUN = ABI::Windows::UI::Notifications;
namespace WDX = ABI::Windows::Data::Xml::Dom;
using Microsoft::WRL::Wrappers::HStringReference;

std::wstring xmlEscape(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    for (wchar_t c : in) {
        switch (c) {
            case L'&':  out += L"&amp;";  break;
            case L'<':  out += L"&lt;";   break;
            case L'>':  out += L"&gt;";   break;
            case L'"':  out += L"&quot;"; break;
            case L'\'': out += L"&apos;"; break;
            default:    out += c;         break;
        }
    }
    return out;
}

// Returns false if anything at all went wrong, so the caller can fall back rather than leave the user with nothing.
bool showToast(const std::wstring& summary, const std::wstring& body) {
    if (!ensureShortcut()) {
        brls::Logger::warning("shell: no usable Start Menu shortcut — Windows will not "
                              "show a toast without one; falling back to the taskbar flash");
        return false;
    }

    ComPtr<WUN::IToastNotificationManagerStatics> mgr;
    if (FAILED(RoGetActivationFactory(
            HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(),
            IID_PPV_ARGS(&mgr))))
        return false;

    // ToastGeneric with two text lines: title, then the detail.
    const std::wstring xml =
        L"<toast><visual><binding template=\"ToastGeneric\"><text>" +
        xmlEscape(summary) + L"</text><text>" + xmlEscape(body) +
        L"</text></binding></visual></toast>";

    ComPtr<WDX::IXmlDocument> doc;
    if (FAILED(RoActivateInstance(
            HStringReference(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument).Get(),
            (IInspectable**)doc.GetAddressOf())))
        return false;
    ComPtr<WDX::IXmlDocumentIO> io;
    if (FAILED(doc.As(&io))) return false;
    if (FAILED(io->LoadXml(HStringReference(xml.c_str()).Get()))) return false;

    ComPtr<WUN::IToastNotificationFactory> factory;
    if (FAILED(RoGetActivationFactory(
            HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotification).Get(),
            IID_PPV_ARGS(&factory))))
        return false;
    ComPtr<WUN::IToastNotification> toast;
    if (FAILED(factory->CreateToastNotification(doc.Get(), &toast))) return false;

    // Notifier is per-AppUserModelID, and the id must match the shortcut's.
    ComPtr<WUN::IToastNotifier> notifier;
    if (FAILED(mgr->CreateToastNotifierWithId(HStringReference(kAumid).Get(), &notifier)))
        return false;

    const HRESULT hr = notifier->Show(toast.Get());
    if (FAILED(hr)) {
        // The usual causes are notifications switched off for this app, Focus
        // Assist, or an AppUserModelID the shell cannot resolve to the shortcut.
        brls::Logger::warning("shell: toast Show failed ({:#x}) — falling back to the "
                              "taskbar flash", (unsigned)hr);
        return false;
    }
    brls::Logger::debug("shell: toast shown");
    return true;
}

#if defined(VITAPLEX_HAVE_TOAST_PROGRESS)

// A progress toast is Windows' answer to replaces_id, and it works the other
// way round: the toast is posted once with a <progress> element whose fields
// are data bindings, and later values are pushed with Update() against the
// toast's tag. Nothing is re-posted, so there is only ever one popup and no id
// to lose track of.
constexpr const wchar_t* kProgressTag = L"vitaplex-download";
UINT32 g_progressSeq = 0;
bool   g_progressLive = false;

ComPtr<WUN::IToastNotifier> progressNotifier() {
    static ComPtr<WUN::IToastNotifier> s_notifier;
    if (s_notifier) return s_notifier;
    ComPtr<WUN::IToastNotificationManagerStatics> mgr;
    if (FAILED(RoGetActivationFactory(
            HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(),
            IID_PPV_ARGS(&mgr))))
        return nullptr;
    mgr->CreateToastNotifierWithId(HStringReference(kAumid).Get(), &s_notifier);
    return s_notifier;
}

// Fill a NotificationData with the three bound fields plus a sequence number.
// The sequence number is what lets the platform discard an update that arrives
// out of order; it must increase, and 0 means "always apply".
bool buildData(ComPtr<WUN::INotificationData>& data, const std::wstring& status,
               double fraction, const std::wstring& valueText) {
    ComPtr<IInspectable> insp;
    if (FAILED(RoActivateInstance(
            HStringReference(RuntimeClass_Windows_UI_Notifications_NotificationData).Get(), &insp)))
        return false;
    if (FAILED(insp.As(&data))) return false;

    ComPtr<ABI::Windows::Foundation::Collections::IMap<HSTRING, HSTRING>> values;
    if (FAILED(data->get_Values(&values))) return false;

    boolean replaced = false;
    wchar_t num[32];
    // A negative fraction means the size is unknown; "indeterminate" is the
    // value the progress element defines for exactly that.
    if (fraction < 0.0) wcscpy_s(num, L"indeterminate");
    else                swprintf_s(num, L"%.3f", fraction > 1.0 ? 1.0 : fraction);

    values->Insert(HStringReference(L"progressValue").Get(),
                   HStringReference(num).Get(), &replaced);
    values->Insert(HStringReference(L"progressValueString").Get(),
                   HStringReference(valueText.c_str()).Get(), &replaced);
    values->Insert(HStringReference(L"progressStatus").Get(),
                   HStringReference(status.c_str()).Get(), &replaced);
    data->put_SequenceNumber(++g_progressSeq);
    return true;
}

// Post the one progress toast. Its <progress> fields are bindings, so every
// later change goes through updateProgressToast rather than a new toast.
bool showProgressToast(const std::wstring& title, const std::wstring& status,
                       double fraction, const std::wstring& valueText) {
    if (!ensureShortcut()) return false;
    ComPtr<WUN::IToastNotifier> notifier = progressNotifier();
    if (!notifier) return false;

    const std::wstring xml =
        L"<toast><visual><binding template=\"ToastGeneric\"><text>" + xmlEscape(title) +
        L"</text><progress value=\"{progressValue}\" "
        L"valueStringOverride=\"{progressValueString}\" status=\"{progressStatus}\"/>"
        L"</binding></visual></toast>";

    ComPtr<WDX::IXmlDocument> doc;
    if (FAILED(RoActivateInstance(
            HStringReference(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument).Get(),
            (IInspectable**)doc.GetAddressOf())))
        return false;
    ComPtr<WDX::IXmlDocumentIO> io;
    if (FAILED(doc.As(&io)) ||
        FAILED(io->LoadXml(HStringReference(xml.c_str()).Get())))
        return false;

    ComPtr<WUN::IToastNotificationFactory> factory;
    if (FAILED(RoGetActivationFactory(
            HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotification).Get(),
            IID_PPV_ARGS(&factory))))
        return false;
    ComPtr<WUN::IToastNotification> toast;
    if (FAILED(factory->CreateToastNotification(doc.Get(), &toast))) return false;

    // The tag is how Update() finds this toast later.
    ComPtr<WUN::IToastNotification2> toast2;
    if (FAILED(toast.As(&toast2))) return false;
    if (FAILED(toast2->put_Tag(HStringReference(kProgressTag).Get()))) return false;

    ComPtr<WUN::INotificationData> data;
    if (!buildData(data, status, fraction, valueText)) return false;
    if (FAILED(toast2->put_Data(data.Get()))) return false;

    if (FAILED(notifier->Show(toast.Get()))) return false;
    g_progressLive = true;
    return true;
}

// Push new values into the toast already on screen. Returns false when the
// platform says the toast is gone — dismissed, or never shown — so the caller
// can stop rather than keep pushing into nothing.
bool updateProgressToast(const std::wstring& status, double fraction,
                         const std::wstring& valueText) {
    ComPtr<WUN::IToastNotifier> notifier = progressNotifier();
    if (!notifier) return false;
    ComPtr<WUN::IToastNotifier2> notifier2;
    if (FAILED(notifier.As(&notifier2))) return false;

    ComPtr<WUN::INotificationData> data;
    if (!buildData(data, status, fraction, valueText)) return false;

    WUN::NotificationUpdateResult result = WUN::NotificationUpdateResult_Failed;
    if (FAILED(notifier2->UpdateWithTag(data.Get(),
                                        HStringReference(kProgressTag).Get(), &result)))
        return false;
    // NotificationNotFound is the user having dismissed it. Re-posting would be
    // the Linux "closing it reopened it" bug in Windows dress, so treat it as
    // final for this run.
    return result == WUN::NotificationUpdateResult_Succeeded;
}

void hideProgressToast() {
    if (!g_progressLive) return;
    g_progressLive = false;
    ComPtr<WUN::IToastNotificationManagerStatics2> mgr2;
    if (FAILED(RoGetActivationFactory(
            HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(),
            IID_PPV_ARGS(&mgr2))))
        return;
    ComPtr<WUN::IToastNotificationHistory> history;
    if (FAILED(mgr2->get_History(&history))) return;
    history->RemoveGroupedTagWithId(HStringReference(kProgressTag).Get(),
                                    HStringReference(L"").Get(),
                                    HStringReference(kAumid).Get());
}

#endif // VITAPLEX_HAVE_TOAST_PROGRESS
#endif // VITAPLEX_HAVE_TOAST

} // namespace

void init() {
    ensureCom();
    // Before any window exists. Without it the shell treats the process as
    // having no identity: the taskbar button will not group or pin reliably,
    // and a toast has nothing to be sent as.
    HRESULT hr = SetCurrentProcessExplicitAppUserModelID(kAumid);
    if (FAILED(hr))
        brls::Logger::debug("shell: SetCurrentProcessExplicitAppUserModelID failed ({:#x})",
                            (unsigned)hr);
}

void setShortcutAllowed(bool allowed) {
    if (g_shortcutAllowed == allowed) return;
    g_shortcutAllowed = allowed;
    if (allowed) {
        ensureShortcut();
        return;
    }
    // Turned off: take the shortcut away rather than leaving one behind that the setting says should not exist.
    const std::wstring path = shortcutPath();
    if (!path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (DeleteFileW(path.c_str()))
            brls::Logger::info("shell: removed the Start Menu shortcut");
    }
}

void notify(const std::string& summary, const std::string& body) {
#if defined(VITAPLEX_HAVE_TOAST)
    if (showToast(widen(summary), widen(body))) return;
    // Fell through: no shortcut could be written, or the notifier refused.
#endif
    flashTaskbar();
}

// The taskbar bar is a fraction and a state; there is no text on it, so the
// title and detail are unused here.
void setProgress(double fraction, const std::string& title,
                 const std::string& detail, bool visible) {
    // The taskbar button first. This is the native idiom for a download on
    // Windows — it is what browsers use — and unlike the toast below it needs
    // nothing registered and is always available.
    if (ComPtr<ITaskbarList3> tb = taskbar()) {
        if (HWND hwnd = appHwnd()) {
            if (!visible) {
                tb->SetProgressState(hwnd, TBPF_NOPROGRESS);
            } else if (fraction < 0.0) {
                // Size unknown — the marquee, rather than a 0% that reads as stalled.
                tb->SetProgressState(hwnd, TBPF_INDETERMINATE);
            } else {
                const double f = fraction > 1.0 ? 1.0 : fraction;
                tb->SetProgressState(hwnd, TBPF_NORMAL);
                tb->SetProgressValue(hwnd, (ULONGLONG)(f * 1000.0), 1000ULL);
            }
        }
    }

#if defined(VITAPLEX_HAVE_TOAST_PROGRESS)
    // And the text, which the taskbar bar has nowhere to put. One toast, posted
    // once and then updated in place through its tag — Windows' equivalent of
    // replaces_id, and it cannot drift out of sync because nothing is re-posted.
    static bool s_giveUp = false;
    if (!visible) {
        hideProgressToast();
        s_giveUp = false;                    // a new run gets a fresh try
        return;
    }
    if (s_giveUp) return;
    const std::wstring wTitle  = widen(title.empty() ? "Downloading" : title);
    const std::wstring wDetail = widen(detail);
    if (!g_progressLive) {
        if (!showProgressToast(wTitle, wDetail, fraction, wDetail)) s_giveUp = true;
    } else if (!updateProgressToast(wDetail, fraction, wDetail)) {
        // Dismissed, or gone. Stop pushing; the taskbar bar carries on.
        g_progressLive = false;
        s_giveUp       = true;
    }
#else
    (void)title;
    (void)detail;
#endif
}

} // namespace shell
} // namespace vitaplex

#endif // _WIN32
