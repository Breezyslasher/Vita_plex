/**
 * VitaPlex - Login Activity implementation
 */

#include "activity/login_activity.hpp"
#include "app/application.hpp"
#include "app/plex_client.hpp"
#include "view/progress_dialog.hpp"
#include "utils/async.hpp"

#include <memory>

namespace vitaplex {

LoginActivity::LoginActivity() {
    brls::Logger::debug("LoginActivity created");
}

brls::View* LoginActivity::createContentView() {
    return brls::View::createFromXMLResource("activity/login.xml");
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
        serverLabel->setText(std::string("Server: ") + (m_serverUrl.empty() ? "Not set" : m_serverUrl));
        serverLabel->registerClickAction([this](brls::View* view) {
            brls::Application::getImeManager()->openForText([this](std::string text) {
                m_serverUrl = text;
                serverLabel->setText(std::string("Server: ") + text);
            }, "Enter Server URL", "http://your-server:32400", 256, m_serverUrl);
            return true;
        });
        serverLabel->addGestureRecognizer(new brls::TapGestureRecognizer(serverLabel));
    }

    // Username input
    if (usernameLabel) {
        usernameLabel->setText(std::string("Username: ") + (m_username.empty() ? "Not set" : m_username));
        usernameLabel->registerClickAction([this](brls::View* view) {
            brls::Application::getImeManager()->openForText([this](std::string text) {
                m_username = text;
                usernameLabel->setText(std::string("Username: ") + text);
            }, "Enter Username", "", 128, m_username);
            return true;
        });
        usernameLabel->addGestureRecognizer(new brls::TapGestureRecognizer(usernameLabel));
    }

    // Password input
    if (passwordLabel) {
        passwordLabel->setText(std::string("Password: ") + (m_password.empty() ? "Not set" : "********"));
        passwordLabel->registerClickAction([this](brls::View* view) {
            brls::Application::getImeManager()->openForPassword([this](std::string text) {
                m_password = text;
                passwordLabel->setText("Password: ********");
            }, "Enter Password", "", 128, "");
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

void LoginActivity::showServerSelectionDialog(const std::vector<PlexServer>& servers) {
    // Create dialog with server list
    auto* dialog = new brls::Dialog("Select Server");

    auto* list = new brls::Box();
    list->setAxis(brls::Axis::COLUMN);
    list->setPadding(20);

    for (size_t i = 0; i < servers.size(); i++) {
        auto* btn = new brls::Button();
        btn->setText(servers[i].name);
        btn->setMarginBottom(10);

        // Capture server by value
        PlexServer server = servers[i];
        btn->registerClickAction([this, server, dialog](brls::View* view) {
            dialog->dismiss();
            connectToSelectedServer(server);
            return true;
        });

        list->addView(btn);
    }

    dialog->addView(list);
    dialog->addButton("Cancel", [dialog]() { dialog->dismiss(); });

    brls::Application::pushActivity(new brls::Activity(dialog));
}

void LoginActivity::connectToSelectedServer(const PlexServer& server) {
    PlexClient& client = PlexClient::getInstance();

    // Show progress dialog
    auto* progressDialog = new ProgressDialog("Connecting");
    progressDialog->setStatus("Connecting to " + server.name + "...");
    progressDialog->show();

    // Track if connection was cancelled - use shared_ptr so it persists across async operation
    auto cancelled = std::make_shared<bool>(false);
    progressDialog->setCancelCallback([cancelled]() {
        *cancelled = true;
    });

    size_t totalConnections = server.connections.size();

    // Run connection attempts asynchronously
    asyncRun([this, server, progressDialog, totalConnections, cancelled]() {
        PlexClient& client = PlexClient::getInstance();

        for (size_t i = 0; i < totalConnections && !*cancelled; i++) {
            const auto& conn = server.connections[i];
            std::string connType = conn.local ? "local" : (conn.relay ? "relay" : "remote");

            brls::sync([progressDialog, i, totalConnections, server, connType]() {
                progressDialog->setAttempt(i + 1, totalConnections);
                progressDialog->setStatus("Trying " + connType + " connection...");
                progressDialog->setProgress(static_cast<float>(i) / totalConnections);
            });

            brls::Logger::info("Trying connection {}/{}: {} ({})",
                              i + 1, totalConnections, conn.uri, connType);

            if (client.connectToServer(conn.uri)) {
                // Success!
                brls::sync([this, progressDialog, server]() {
                    progressDialog->setStatus("Connected!");
                    progressDialog->setProgress(1.0f);

                    Application::getInstance().saveSettings();
                    if (statusLabel) statusLabel->setText("Connected to " + server.name);

                    // Delay to show success, then proceed
                    brls::delay(500, [this, progressDialog]() {
                        progressDialog->dismiss();
                        Application::getInstance().pushMainActivity();
                    });
                });
                return;
            }

            brls::Logger::info("Connection {} failed, trying next...", i + 1);
        }

        // All connections failed
        brls::sync([this, progressDialog, server, totalConnections]() {
            progressDialog->setStatus("All " + std::to_string(totalConnections) + " connection attempts failed");
            progressDialog->setProgress(1.0f);

            if (statusLabel) statusLabel->setText("Failed to connect to " + server.name);
            brls::Logger::error("All {} connections failed for {}", totalConnections, server.name);

            // Delay then dismiss
            brls::delay(2000, [progressDialog]() {
                progressDialog->dismiss();
            });
        });
    });
}

void LoginActivity::onLoginPressed() {
    if (m_username.empty() || m_password.empty()) {
        if (statusLabel) statusLabel->setText("Please enter username and password");
        return;
    }

    if (statusLabel) statusLabel->setText("Logging in...");

    // Perform login
    PlexClient& client = PlexClient::getInstance();

    if (client.login(m_username, m_password)) {
        Application::getInstance().setUsername(m_username);

        // If server URL provided, use it; otherwise auto-detect
        if (!m_serverUrl.empty()) {
            if (statusLabel) statusLabel->setText("Connecting to server...");
            if (client.connectToServer(m_serverUrl)) {
                Application::getInstance().saveSettings();
                if (statusLabel) statusLabel->setText("Login successful!");
                brls::sync([this]() {
                    Application::getInstance().pushMainActivity();
                });
            } else {
                if (statusLabel) statusLabel->setText("Failed to connect to server");
            }
        } else {
            // Auto-detect servers
            if (statusLabel) statusLabel->setText("Finding your servers...");
            std::vector<PlexServer> servers;
            if (client.fetchServers(servers) && !servers.empty()) {
                if (servers.size() == 1) {
                    // Only one server, connect directly
                    connectToSelectedServer(servers[0]);
                } else {
                    // Multiple servers, show selection dialog
                    if (statusLabel) statusLabel->setText("Select a server:");
                    showServerSelectionDialog(servers);
                }
            } else {
                if (statusLabel) statusLabel->setText("No servers found - enter URL manually");
            }
        }
    } else {
        if (statusLabel) statusLabel->setText("Login failed - check credentials");
    }
}

void LoginActivity::onPinLoginPressed() {
    m_pinMode = true;

    PlexClient& client = PlexClient::getInstance();

    if (client.requestPin(m_pinAuth)) {
        if (pinCodeLabel) {
            pinCodeLabel->setVisibility(brls::Visibility::VISIBLE);
            pinCodeLabel->setText(std::string("PIN: ") + m_pinAuth.code);
        }
        if (statusLabel) {
            statusLabel->setText("Go to plex.tv/link and enter the PIN above");
        }

        // Start checking PIN status using RepeatingTimer
        m_pinCheckTimer = 0;
        m_pinTimer.setCallback([this]() {
            checkPinStatus();
        });
        m_pinTimer.start(2000); // Check every 2 seconds
    } else {
        if (statusLabel) statusLabel->setText("Failed to request PIN");
    }
}

void LoginActivity::checkPinStatus() {
    if (!m_pinMode) {
        m_pinTimer.stop();
        return;
    }

    m_pinCheckTimer++;

    PlexClient& client = PlexClient::getInstance();

    if (client.checkPin(m_pinAuth)) {
        m_pinMode = false;
        m_pinTimer.stop();
        if (pinCodeLabel) pinCodeLabel->setVisibility(brls::Visibility::GONE);

        if (statusLabel) statusLabel->setText("PIN authenticated! Finding servers...");

        // If server URL provided, use it; otherwise auto-detect
        if (!m_serverUrl.empty()) {
            if (client.connectToServer(m_serverUrl)) {
                Application::getInstance().saveSettings();
                if (statusLabel) statusLabel->setText("Connected!");
                brls::sync([this]() {
                    Application::getInstance().pushMainActivity();
                });
            } else {
                if (statusLabel) statusLabel->setText("Failed to connect to server");
            }
        } else {
            // Auto-detect servers
            std::vector<PlexServer> servers;
            if (client.fetchServers(servers) && !servers.empty()) {
                if (servers.size() == 1) {
                    // Only one server, connect directly
                    connectToSelectedServer(servers[0]);
                } else {
                    // Multiple servers, show selection dialog
                    if (statusLabel) statusLabel->setText("Select a server:");
                    showServerSelectionDialog(servers);
                }
            } else {
                if (statusLabel) statusLabel->setText("No servers found - enter URL manually");
            }
        }
    } else if (m_pinAuth.expired || m_pinCheckTimer > 150) {
        // PIN expired (5 minutes)
        m_pinMode = false;
        m_pinTimer.stop();
        if (statusLabel) statusLabel->setText("PIN expired - try again");
        if (pinCodeLabel) pinCodeLabel->setVisibility(brls::Visibility::GONE);
    }
}

} // namespace vitaplex
