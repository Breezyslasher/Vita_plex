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
#include "app/plex_client.hpp"

namespace vitaplex {

class LoginActivity : public brls::Activity {
public:
    LoginActivity();

    brls::View* createContentView() override;

    void onContentAvailable() override;

private:
    void onLoginPressed();
    void onPinLoginPressed();
    void onOfflinePressed();
    void checkPinStatus();
    void showServerSelectionDialog(const std::vector<PlexServer>& servers);
    void connectToSelectedServer(const PlexServer& server);

    // Build one focusable server card row for showServerSelectionDialog.
    // Sub-line carries the URI, version, and the Local/Remote/Relay
    // badges derived from server.connections — the same list the
    // parallel probe in connectToSelectedServer will hit.
    brls::Box* buildServerCard(const PlexServer& server,
                               brls::Dialog* dialog);

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
    BRLS_BIND(brls::Label,  serverLabel,         "login/server_label");
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
};

} // namespace vitaplex
