/**
 * VitaPlex - shell integration, PS4 backend (see the header).
 *
 * sceKernelSendNotificationRequest writes to /dev/notification0, which
 * SceShellUI is listening on — that is the familiar popup in the top-right
 * corner, and it appears over a running application. It is in libkernel,
 * already linked, so this costs nothing to reach.
 *
 * There is no progress form. The PS4's own download progress lives in the
 * system download list, which a homebrew application has no way into, so
 * setProgress does nothing here rather than posting a fresh popup every
 * second — an unreadable notification area is worse than none.
 */

#include "utils/shell_integration.hpp"

#if defined(__PS4__)

#include <borealis.hpp>

#include <cstdio>
#include <cstring>
#include <string>

#if defined(VITAPLEX_HAVE_PS4_NOTIFY)
#include <orbis/libkernel.h>
#endif

namespace vitaplex {
namespace shell {

void init() {}

void notify(const std::string& summary, const std::string& body) {
#if defined(VITAPLEX_HAVE_PS4_NOTIFY)
    OrbisNotificationRequest req;
    std::memset(&req, 0, sizeof(req));
    req.type            = NotificationRequest;
    req.targetId        = -1;  // everyone on the console, not one user id
    req.useIconImageUri = 0;   // no iconUri set, so the system icon is used

    std::string text = summary;
    if (!body.empty()) {
        if (!text.empty()) text += "\n";
        text += body;
    }
    std::snprintf(req.message, sizeof(req.message), "%s", text.c_str());

    // Non-blocking: a notification is never worth stalling a download thread
    // for, and a failure here is not something the user needs told about.
    const int rc = sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
    if (rc < 0) brls::Logger::debug("shell: sceKernelSendNotificationRequest ({:#x})",
                                    (unsigned)rc);
#else
    (void)summary;
    (void)body;
#endif
}

// Nothing to draw on. See the file header.
void setProgress(double, const std::string&, const std::string&, bool) {}

}  // namespace shell
}  // namespace vitaplex

#endif  // __PS4__
