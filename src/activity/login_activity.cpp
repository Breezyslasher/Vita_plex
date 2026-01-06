/**
 * VitaPlex - Login Activity implementation
 */

#include "activity/login_activity.hpp"
#include "app/application.hpp"
#include "app/plex_client.hpp"

namespace vitaplex {

LoginActivity::LoginActivity() {
    brls::Logger::debug("LoginActivity created");
}

brls::View* LoginActivity::createContentView() {
    return brls::View::createFromXMLResource("xml/activity/login.xml");
}

void LoginActivity::onContentAvailable() {
    brls::Logger::debug("LoginActivity content available");

    // Set initial values
    if (titleLabel) {
        titleLabel->setText("VitaPlex");
    }

    if (statusLabel) {
        statusLabel->setText("Enter your Plex server URL and credentials");
    }

    if (pinCodeLabel) {
        pinCodeLabel->setVisibility(brls::Visibility::GONE);
    }

    // Server URL input
    if (serverLabel) {
        serverLabel->setText("Server: " + (m_serverUrl.empty() ? "Not set" : m_serverUrl));
        serverLabel->registerClickAction([this](brls::View* view) {
            brls::Swkbd::openForText([this](std::string text) {
                m_serverUrl = text;
                serverLabel->setText("Server: " + text);
            }, "Enter Server URL", "http://your-server:32400", 256, m_serverUrl);
            return true;
        });
        serverLabel->addGestureRecognizer(new brls::TapGestureRecognizer(serverLabel));
    }

    // Username input
    if (usernameLabel) {
        usernameLabel->setText("Username: " + (m_username.empty() ? "Not set" : m_username));
        usernameLabel->registerClickAction([this](brls::View* view) {
            brls::Swkbd::openForText([this](std::string text) {
                m_username = text;
                usernameLabel->setText("Username: " + text);
            }, "Enter Username", "", 128, m_username);
            return true;
        });
        usernameLabel->addGestureRecognizer(new brls::TapGestureRecognizer(usernameLabel));
    }

    // Password input
    if (passwordLabel) {
        passwordLabel->setText("Password: " + (m_password.empty() ? "Not set" : "********"));
        passwordLabel->registerClickAction([this](brls::View* view) {
            brls::Swkbd::openForText([this](std::string text) {
                m_password = text;
                passwordLabel->setText("Password: ********");
            }, "Enter Password", "", 128, "", 0, "", 0);
            return true;
        });
        passwordLabel->addGestureRecognizer(new brls::TapGestureRecognizer(passwordLabel));
    }

    // Login button
    if (loginButton) {
        loginButton->setText("Login with Credentials");
        loginButton->registerClickAction([this](brls::View* view) {
            onLoginPressed();
            return true;
        });
    }

    // PIN login button
    if (pinButton) {
        pinButton->setText("Login with PIN (plex.tv/link)");
        pinButton->registerClickAction([this](brls::View* view) {
            onPinLoginPressed();
            return true;
        });
    }
}

void LoginActivity::onLoginPressed() {
    if (m_serverUrl.empty()) {
        if (statusLabel) statusLabel->setText("Please enter a server URL");
        return;
    }

    if (m_username.empty() || m_password.empty()) {
        if (statusLabel) statusLabel->setText("Please enter username and password");
        return;
    }

    if (statusLabel) statusLabel->setText("Logging in...");

    // Perform login
    PlexClient& client = PlexClient::getInstance();

    if (client.login(m_username, m_password)) {
        if (client.connectToServer(m_serverUrl)) {
            Application::getInstance().setUsername(m_username);
            Application::getInstance().saveSettings();

            if (statusLabel) statusLabel->setText("Login successful!");

            // Navigate to main activity
            brls::sync([this]() {
                Application::getInstance().pushMainActivity();
            });
        } else {
            if (statusLabel) statusLabel->setText("Failed to connect to server");
        }
    } else {
        if (statusLabel) statusLabel->setText("Login failed - check credentials");
    }
}

void LoginActivity::onPinLoginPressed() {
    if (m_serverUrl.empty()) {
        if (statusLabel) statusLabel->setText("Please enter a server URL first");
        return;
    }

    m_pinMode = true;

    PlexClient& client = PlexClient::getInstance();

    if (client.requestPin(m_pinAuth)) {
        if (pinCodeLabel) {
            pinCodeLabel->setVisibility(brls::Visibility::VISIBLE);
            pinCodeLabel->setText("PIN: " + m_pinAuth.code);
        }
        if (statusLabel) {
            statusLabel->setText("Go to plex.tv/link and enter the PIN above");
        }

        // Start checking PIN status
        m_pinCheckTimer = 0;
        brls::Application::createTimer(2000, [this]() {
            checkPinStatus();
            return !m_pinMode; // Stop timer when not in PIN mode
        });
    } else {
        if (statusLabel) statusLabel->setText("Failed to request PIN");
    }
}

void LoginActivity::checkPinStatus() {
    if (!m_pinMode) return;

    m_pinCheckTimer++;

    PlexClient& client = PlexClient::getInstance();

    if (client.checkPin(m_pinAuth)) {
        m_pinMode = false;

        if (client.connectToServer(m_serverUrl)) {
            Application::getInstance().saveSettings();

            if (statusLabel) statusLabel->setText("PIN authenticated!");

            brls::sync([this]() {
                Application::getInstance().pushMainActivity();
            });
        }
    } else if (m_pinAuth.expired || m_pinCheckTimer > 150) {
        // PIN expired (5 minutes)
        m_pinMode = false;
        if (statusLabel) statusLabel->setText("PIN expired - try again");
        if (pinCodeLabel) pinCodeLabel->setVisibility(brls::Visibility::GONE);
    }
}

} // namespace vitaplex
