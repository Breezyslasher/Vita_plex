/**
 * VitaPlex - Login Activity
 *
 * PIN-first login. On open the device-link flow auto-starts and the
 * 4-digit code is shown as a row of teal tiles; the username/password
 * path lives behind a "Use credentials" sub-view so TV/console users
 * don't have to type via the IME unless they want to. Every existing
 * auth handler (onLoginPressed, onPinLoginPressed, checkPinStatus,
 * onOfflinePressed, the IME callbacks) is kept verbatim — the
 * redesign only changes layout, default focus, and visual styling.
 */

#pragma once

#include <borealis.hpp>
#include <borealis/core/timer.hpp>
#include <functional>
#include <memory>
#include <atomic>
#include <vector>
#include "app/plex_client.hpp"

namespace vitaplex {

class LoginActivity : public brls::Activity {
public:
    LoginActivity();
    ~LoginActivity() override;

    brls::View* createContentView() override;

    void onContentAvailable() override;

private:
    void onLoginPressed();
    void onPinLoginPressed();
    void onOfflinePressed();
    void checkPinStatus();
    void showServerSelectionDialog(const std::vector<PlexServer>& servers);
    void connectToSelectedServer(const PlexServer& server);

    // ── Server-selection / connecting modal ────────────────────────
    // The redesigned server picker and the "connecting" probe view are
    // two states of ONE modal card floated over a dimmed scrim. Both
    // reuse the existing server-resolution + parallel-probe logic in
    // connectToSelectedServer verbatim — only the presentation changes.

    // (Re)build the card-list state from m_servers and float it.
    void showServerSelectionContent();

    // Build one focusable server card for the list. Sub-line carries the
    // host + the Local/Remote/Relay badges derived from server.connections
    // — the same list the parallel probe in connectToSelectedServer hits.
    // onActivate runs on A / tap (swap to the connecting state + probe).
    brls::Box* buildServerCard(const PlexServer& server,
                               std::function<void()> onActivate);

    // Footer "Enter address manually" — opens the IME for a URL and runs
    // it through the same single-candidate parallel probe.
    void onEnterAddressManually();

    // Float `card` centered on a dim scrim (pushing the scrim activity the
    // first time, swapping its child afterwards) and focus `focusView`.
    void presentDialogCard(brls::View* card, brls::View* focusView);
    // Pop the scrim activity if present.
    void dismissDialog();

    // Build a row of digit tiles inside login/pin_tiles from
    // m_pinAuth.code (one tile per character). Called on each
    // successful onPinLoginPressed.
    void renderPinTiles();
    // Swap the card between the PIN view and the credentials sub-view.
    void showPinView();
    void showCredentialsView();
    // Update the "Expires in MM:SS" countdown label from m_pinCheckTimer
    // (driven by the existing 2-second poll). Called from checkPinStatus.
    void updateExpiryCountdown();

    // Existing IDs — preserved so onContentAvailable can still find them.
    // (The Server URL field was removed; server addresses are entered via
    // the picker's "Enter address manually" affordance instead.)
    BRLS_BIND(brls::Label,  usernameLabel,       "login/username_label");
    BRLS_BIND(brls::Label,  passwordLabel,       "login/password_label");
    BRLS_BIND(brls::Button, loginButton,         "login/login_button");
    BRLS_BIND(brls::Button, pinButton,           "login/pin_button");
    BRLS_BIND(brls::Button, offlineButton,       "login/offline_button");
    BRLS_BIND(brls::Label,  statusLabel,         "login/status");
    BRLS_BIND(brls::Label,  pinCodeLabel,        "login/pin_code");

    // New layout pieces.
    BRLS_BIND(brls::Box,    cardBox,             "login/card");
    BRLS_BIND(brls::Box,    pinView,             "login/pin_view");
    BRLS_BIND(brls::Box,    credView,            "login/cred_view");
    BRLS_BIND(brls::Box,    pinTilesBox,         "login/pin_tiles");
    BRLS_BIND(brls::Box,    statusDot,           "login/status_dot");
    BRLS_BIND(brls::Label,  expiryLabel,         "login/expiry");
    BRLS_BIND(brls::Box,    quickPill,           "login/quick_pill");
    BRLS_BIND(brls::Box,    getNewCodeRow,       "login/get_new_code_row");
    BRLS_BIND(brls::Box,    secondaryRow,        "login/secondary_row");
    BRLS_BIND(brls::Button, useCredentialsButton,"login/use_credentials");
    BRLS_BIND(brls::Button, backToPinButton,     "login/back_to_pin");

    std::string m_serverUrl;
    std::string m_username;
    std::string m_password;
    PinAuth m_pinAuth;
    bool m_pinMode = false;
    int m_pinCheckTimer = 0;
    brls::RepeatingTimer m_pinTimer;

    // Modal bookkeeping. m_dialogScrim is the full-screen translucent
    // root pushed as an Activity; null when the modal is closed. The two
    // states (list / connecting) swap as its single child. m_servers is
    // the resolved, owned-first list so the list can be rebuilt on
    // orientation change; m_returnToList tells the connecting state's
    // Cancel/Back whether there's a list to go back to (true) or whether
    // it should close the modal entirely (single-server / manual entry).
    brls::Box* m_dialogScrim = nullptr;
    std::vector<PlexServer> m_servers;
    bool m_returnToList = false;
    int  m_overlayMode = 0;  // 0 none, 1 list, 2 connecting (orientation rebuild)

    // Process-lifetime orientation callback captures this guard instead of
    // a bare `this`; cleared in the destructor so a late rotation after the
    // activity is gone is a no-op rather than a use-after-free.
    std::shared_ptr<std::atomic<bool>> m_alive;
};

} // namespace vitaplex
