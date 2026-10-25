/**
 * VitaPlex - shell integration, PS Vita backend (see the header).
 *
 * SceNotificationUtil is the Vita's own notification system — the same popup
 * the system uses for a finished PS Store download, and the same entry in the
 * notification list afterwards.
 *
 * It has a progress form too. sceNotificationUtilProgressBegin/Update/Finish
 * drive the "BGDL-type" notification, which is what a background download
 * draws, so a VitaPlex download looks like any other download on the console.
 * The header's own note settles what would otherwise need a device to answer:
 * sceNotificationUtilBgAppInitialize "does not need to be called for normal
 * applications", so a plain VPK can drive these without being a registered
 * background app.
 *
 * What this is NOT is what Better Homebrew Browser does. BHBB ships a taiHEN
 * plugin built against Sony's own SDK that talks to SceLsdb, the system's
 * download-list database, which is how its downloads survive leaving the app.
 * That needs a proprietary toolchain and a kernel plugin; this needs neither,
 * and the cost is that downloads still stop when VitaPlex is suspended.
 *
 * Three shapes in the API are easy to get wrong and are load-bearing here:
 * sceNotificationUtilSendNotification wants a 0x410-BYTE buffer, not a
 * pointer to a short string, and the progress structs cap text at
 * SCE_NOTIFICATIONUTIL_TEXT_MAX (63) UTF-16 units followed by a separator
 * that must be zero. Both are handled by zero-initialising and truncating.
 * And the progress notification's event handler is the word at 0x4E8 of
 * ProgressBegin's parameters, which the header names unk_4EC and says can
 * be 0. It cannot (onProgressEvent).
 */

#include "utils/shell_integration.hpp"

#if defined(__vita__)

#include <borealis.hpp>

#include <cstdint>
#include <cstring>
#include <string>

#include "shell_companion.h"

#if defined(VITAPLEX_HAVE_PSV_NOTIFY)
#include <psp2/notificationutil.h>
#include <psp2/sysmodule.h>
#endif

namespace vitaplex {
namespace shell {

namespace {

#if defined(VITAPLEX_HAVE_PSV_NOTIFY)

bool g_ready        = false;
bool g_progressLive = false;
// The user can close the notification in SceShell while downloads go on, and
// what the updates do then is not known yet. Logging the first failure of
// each notification says so without one line a second.
bool g_updateFailLogged = false;

// SceShell's events for the progress notification. ProgressBegin keeps the
// word at kProgressHandlerOffset of its parameters and calls it, as
// void(int), with each event SceShell sends for the notification;
// ProgressFinish forgets it. vitasdk names that word unk_4EC ("can be set
// to 0") and the next one, at 0x4EC, eventHandler, but the module's code
// calls the one at 0x4E8 (DESIGN_NOTES, "Vita: notifications yes,
// background downloads no"). Left 0, up to build 2058, an event was a call
// to address 0: VitaPlex crashed in the background while the server
// prepared a download, after the user tapped the notification and closed it
// (log 677cc50f). That event came from inside
// ProgressUpdate, so on the UI thread, and with the module's lock held, so
// the handler must not call back into SceNotificationUtil. What the events
// mean is not known yet, so they are logged.
constexpr std::size_t kProgressHandlerOffset = 0x4E8;
// Begin sends SceShell 0x570 bytes of its parameters, not the 0x4F0 of
// vitasdk's struct (the method 0x7000B call in Begin's code, read from the
// build 2058 dump): the 0x80 after the struct are a third line, the question
// SceShell asks when the notification's cancel is pressed, with No and Yes
// under it. With only the struct, the question was the two characters of
// stack that followed it (builds 2063 to 2065).
struct ProgressBeginParam {
    SceNotificationUtilProgressInitParam p;
    SceWChar16 cancelText[SCE_NOTIFICATIONUTIL_TEXT_MAX];
    SceInt16 separator2;  // must be 0, as the others
};
static_assert(sizeof(ProgressBeginParam) == 0x570, "Begin sends 0x570 bytes");
static_assert(sizeof(void (*)(int)) == sizeof(SceInt32),
              "the handler is one word, before the one at 0x4EC");
static_assert(kProgressHandlerOffset + sizeof(SceInt32)
                  <= sizeof(SceNotificationUtilProgressInitParam),
              "the handler's word is inside ProgressBegin's parameters");

void onProgressEvent(int eventId) {
    brls::Logger::info("shell: progress notification event {}", eventId);
}

// UTF-8 to UTF-16, writing at most `max` code units and never splitting a
// surrogate pair. Nothing is terminated: every caller passes a zeroed struct
// field, and the separator that follows each of those arrays is required to
// be zero anyway, so the terminator is already there.
//
// Track titles are routinely longer than the 63 units the Vita allows, so
// truncation is the normal path rather than the error path.
std::size_t toUtf16(const std::string& in, SceWChar16* out, std::size_t max) {
    std::size_t o = 0;
    for (std::size_t i = 0; i < in.size() && o < max;) {
        const unsigned char c = (unsigned char)in[i];
        std::uint32_t cp;
        std::size_t   len;
        if      (c < 0x80)          { cp = c;        len = 1; }
        else if ((c & 0xE0) == 0xC0){ cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0){ cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0){ cp = c & 0x07; len = 4; }
        else { ++i; continue; }                   // stray continuation byte
        // A length that runs past the end is a truncated sequence. Emit the
        // replacement character and step one byte rather than stopping: a
        // single bad byte should not throw away the valid text after it, and
        // these strings are server metadata rather than anything we produced.
        if (i + len > in.size()) { out[o++] = 0xFFFD; ++i; continue; }
        for (std::size_t k = 1; k < len; ++k) {
            const unsigned char cc = (unsigned char)in[i + k];
            if ((cc & 0xC0) != 0x80) { cp = 0xFFFD; len = 1; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        i += len;
        if (cp >= 0x10000) {
            if (o + 2 > max) break;               // no room for the pair, stop clean
            cp -= 0x10000;
            out[o++] = (SceWChar16)(0xD800 + (cp >> 10));
            out[o++] = (SceWChar16)(0xDC00 + (cp & 0x3FF));
        } else {
            out[o++] = (SceWChar16)cp;
        }
    }
    return o;
}

// Summary and body are two lines everywhere else; the Vita's plain
// notification is a single string, so they are joined.
std::string joinLines(const std::string& summary, const std::string& body) {
    if (body.empty()) return summary;
    if (summary.empty()) return body;
    return summary + "\n" + body;
}

#endif  // VITAPLEX_HAVE_PSV_NOTIFY

}  // namespace

void init() {
#if defined(VITAPLEX_HAVE_PSV_NOTIFY)
    const int rc = sceSysmoduleLoadModule(SCE_SYSMODULE_NOTIFICATION_UTIL);
    if (rc < 0) {
        brls::Logger::warning("shell: SCE_SYSMODULE_NOTIFICATION_UTIL failed ({:#x}) "
                              "— no download notifications", (unsigned)rc);
        return;
    }
    g_ready = true;
#endif
}

void notify(const std::string& summary, const std::string& body) {
#if defined(VITAPLEX_HAVE_PSV_NOTIFY)
    if (!g_ready) return;
    // 0x410 bytes, as the API requires — not a pointer to a short string.
    SceWChar16 buf[0x410 / sizeof(SceWChar16)] = {};
    toUtf16(joinLines(summary, body), buf, (sizeof(buf) / sizeof(buf[0])) - 1);
    const int rc = sceNotificationUtilSendNotification(buf);
    if (rc < 0) brls::Logger::debug("shell: sceNotificationUtilSendNotification ({:#x})",
                                    (unsigned)rc);
#else
    (void)summary;
    (void)body;
#endif
}

// Where the progress notification showing is: none, VitaPlex's own, or the
// one the shell companion plugin shows for it. Chosen as one begins, and kept
// until it ends, so that a notification never moves half-way.
enum class Progress { None, Own, Shell };
Progress g_progressWhere = Progress::None;
bool g_shellFailLogged = false;

bool progressInShell() {
    return g_progressWhere == Progress::Shell;
}

void setProgress(double fraction, const std::string& title,
                 const std::string& detail, bool visible) {
    // The plugin's when it is in SceShell, VitaPlex having loaded it as the
    // downloads began (DownloadsManager::startDownloads): a notification is
    // its process's, so VitaPlex's own could be gone on with by nothing once
    // VitaPlex had closed, and SceShell had to begin a second one as it
    // carried the downloads on (builds 2063 to 2069). The plugin's is the one
    // notification throughout.
    if (g_progressWhere == Progress::None && visible && scIsOpen())
        g_progressWhere = Progress::Shell;
    if (g_progressWhere == Progress::Shell) {
        const uint32_t permille = fraction < 0.0 ? SC_PROGRESS_UNKNOWN
                                : fraction > 1.0 ? 1000u
                                                 : (uint32_t)(fraction * 1000.0);
        const int rc = visible ? scDownloadProgress(title.c_str(), detail.c_str(), permille)
                               : scDownloadProgressEnd(title.c_str(), detail.c_str());
        if (rc < 0 && !g_shellFailLogged) {
            g_shellFailLogged = true;
            brls::Logger::info("shell: the plugin's notification not reached ({:#x})",
                               (unsigned)rc);
        }
        if (!visible) {
            g_progressWhere = Progress::None;
            g_shellFailLogged = false;
        }
        return;
    }

#if defined(VITAPLEX_HAVE_PSV_NOTIFY)
    if (!g_ready) return;
    if (visible) g_progressWhere = Progress::Own;

    if (!visible) {
        g_progressWhere = Progress::None;
        if (!g_progressLive) return;
        g_progressLive = false;
        SceNotificationUtilProgressFinishParam p = {};
        toUtf16(title, p.notificationText, SCE_NOTIFICATIONUTIL_TEXT_MAX);
        toUtf16(detail, p.notificationSubText, SCE_NOTIFICATIONUTIL_TEXT_MAX);
        // path stays empty: it is where a finished download would point the
        // user, and there is nothing on the Vita to open a media file with.
        const int rc = sceNotificationUtilProgressFinish(&p);
        if (rc < 0) brls::Logger::info("shell: ProgressFinish ({:#x})", (unsigned)rc);
        return;
    }

    if (!g_progressLive) {
        ProgressBeginParam begin = {};
        SceNotificationUtilProgressInitParam& p = begin.p;
        toUtf16(title, p.notificationText, SCE_NOTIFICATIONUTIL_TEXT_MAX);
        toUtf16(detail, p.notificationSubText, SCE_NOTIFICATIONUTIL_TEXT_MAX);
        toUtf16("Pause downloads?", begin.cancelText, SCE_NOTIFICATIONUTIL_TEXT_MAX);
        // The handler goes in by offset, since the header's names for this
        // word and the next are what misled (onProgressEvent). The word it
        // calls eventHandler is passed to SceShell with the registration and
        // stays 0, as it was when events reached the handler.
        void (*handler)(int) = &onProgressEvent;
        std::memcpy(reinterpret_cast<unsigned char*>(&p) + kProgressHandlerOffset,
                    &handler, sizeof(handler));
        const int rc = sceNotificationUtilProgressBegin(&p);
        if (rc < 0) {
            brls::Logger::debug("shell: ProgressBegin ({:#x}) — no progress notification",
                                (unsigned)rc);
            return;
        }
        g_progressLive = true;
        g_updateFailLogged = false;
        return;  // Begin already carries the first text; nothing to update yet
    }

    SceNotificationUtilProgressUpdateParam p = {};
    toUtf16(title, p.notificationText, SCE_NOTIFICATIONUTIL_TEXT_MAX);
    toUtf16(detail, p.notificationSubText, SCE_NOTIFICATIONUTIL_TEXT_MAX);
    // A negative fraction means the size is not known yet. There is no
    // indeterminate state here, so it sits at zero and the text carries it.
    // In percent, as SceShell's bar runs to 100 (the plugin's notifyShow16).
    p.targetProgress = fraction < 0.0  ? 0.0f
                     : fraction > 1.0  ? 100.0f
                                       : (SceFloat)(fraction * 100.0);
    const int rc = sceNotificationUtilProgressUpdate(&p);
    if (rc < 0 && !g_updateFailLogged) {
        g_updateFailLogged = true;
        brls::Logger::info("shell: ProgressUpdate ({:#x}), first failure of this notification",
                           (unsigned)rc);
    }
#else
    (void)fraction;
    (void)title;
    (void)detail;
    (void)visible;
#endif
}

}  // namespace shell
}  // namespace vitaplex

#endif  // __vita__
