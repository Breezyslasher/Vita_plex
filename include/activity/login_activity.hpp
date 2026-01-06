/**
 * VitaPlex - Login Activity
 * Handles user authentication via credentials or PIN
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
    void checkPinStatus();
    void showServerSelectionDialog(const std::vector<PlexServer>& servers);
    void connectToSelectedServer(const PlexServer& server);

    BRLS_BIND(brls::Label, titleLabel, "login/title");
    BRLS_BIND(brls::Box, inputContainer, "login/input_container");
    BRLS_BIND(brls::Label, serverLabel, "login/server_label");
    BRLS_BIND(brls::Label, usernameLabel, "login/username_label");
    BRLS_BIND(brls::Label, passwordLabel, "login/password_label");
    BRLS_BIND(brls::Button, loginButton, "login/login_button");
    BRLS_BIND(brls::Button, pinButton, "login/pin_button");
    BRLS_BIND(brls::Label, statusLabel, "login/status");
    BRLS_BIND(brls::Label, pinCodeLabel, "login/pin_code");

    std::string m_serverUrl;
    std::string m_username;
    std::string m_password;
    PinAuth m_pinAuth;
    bool m_pinMode = false;
    int m_pinCheckTimer = 0;
    brls::RepeatingTimer m_pinTimer;
};

} // namespace vitaplex
