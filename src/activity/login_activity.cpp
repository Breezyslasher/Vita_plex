/**
 * VitaPlex - Login Activity implementation
 */

#include "activity/login_activity.hpp"
#include "app/application.hpp"
#include "app/plex_client.hpp"
#include "app/downloads_manager.hpp"
#include "view/progress_dialog.hpp"
#include "utils/async.hpp"
#include "utils/http_client.hpp"
#include "platform/platform.hpp"

#include <memory>
#include <thread>
#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <mutex>

namespace vitaplex {

// Minimal counting semaphore. std::counting_semaphore is C++20 but the
// project still targets C++17 for Vita / Switch toolchain compatibility,
// so roll the obvious mutex+condvar version inline here. Only used by
// the parallel probe loop below; no need for a header.
namespace {
class CountingSemaphore {
public:
    explicit CountingSemaphore(std::ptrdiff_t initial) : m_count(initial) {}
    void acquire() {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_cv.wait(lk, [&] { return m_count > 0; });
        --m_count;
    }
    void release() {
        std::lock_guard<std::mutex> lk(m_mutex);
        ++m_count;
        m_cv.notify_one();
    }
private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::ptrdiff_t m_count;
};
}  // namespace

LoginActivity::LoginActivity() {
    brls::Logger::debug("LoginActivity created");
}

brls::View* LoginActivity::createContentView() {
    return brls::View::createFromXMLResource("activity/login.xml");
}

void LoginActivity::onContentAvailable() {
    brls::Logger::debug("LoginActivity content available");

    // Pill background — brls XML doesn't accept rgba colours, so paint
    // the Plex-yellow-12%-alpha bg here. Same trick for the focus-able-
    // but-not-yet-built tile container is unnecessary because it's
    // empty until renderPinTiles() runs.
    if (quickPill) {
        quickPill->setBackgroundColor(nvgRGBA(229, 160, 13, 38));
    }

    // ── Server / Username / Password inputs ────────────────────────
    // Same IME handlers as before — only the surrounding presentation
    // changed (the Label now sits inside a rounded field-row Box in
    // the credentials sub-view). The text we set on the label switches
    // colour between muted (placeholder) and white (entered value).
    if (serverLabel) {
        std::string current = m_serverUrl.empty() ? std::string("Not set") : m_serverUrl;
        serverLabel->setText(current);
        serverLabel->setTextColor(m_serverUrl.empty() ? nvgRGB(163, 163, 163) : nvgRGB(255, 255, 255));
        serverLabel->registerClickAction([this](brls::View*) {
            brls::Application::getImeManager()->openForText([this](std::string text) {
                m_serverUrl = text;
                if (serverLabel) {
                    serverLabel->setText(text.empty() ? std::string("Not set") : text);
                    serverLabel->setTextColor(text.empty() ? nvgRGB(163, 163, 163) : nvgRGB(255, 255, 255));
                }
            }, "Enter Server URL", "http://your-server:32400", 256, m_serverUrl);
            return true;
        });
        serverLabel->addGestureRecognizer(new brls::TapGestureRecognizer(serverLabel));
    }

    if (usernameLabel) {
        std::string current = m_username.empty() ? std::string("Not set") : m_username;
        usernameLabel->setText(current);
        usernameLabel->setTextColor(m_username.empty() ? nvgRGB(163, 163, 163) : nvgRGB(255, 255, 255));
        usernameLabel->registerClickAction([this](brls::View*) {
            brls::Application::getImeManager()->openForText([this](std::string text) {
                m_username = text;
                if (usernameLabel) {
                    usernameLabel->setText(text.empty() ? std::string("Not set") : text);
                    usernameLabel->setTextColor(text.empty() ? nvgRGB(163, 163, 163) : nvgRGB(255, 255, 255));
                }
            }, "Enter Username", "", 128, m_username);
            return true;
        });
        usernameLabel->addGestureRecognizer(new brls::TapGestureRecognizer(usernameLabel));
    }

    if (passwordLabel) {
        passwordLabel->setText(m_password.empty() ? std::string("Not set") : std::string("********"));
        passwordLabel->setTextColor(m_password.empty() ? nvgRGB(163, 163, 163) : nvgRGB(255, 255, 255));
        passwordLabel->registerClickAction([this](brls::View*) {
            brls::Application::getImeManager()->openForPassword([this](std::string text) {
                m_password = text;
                if (passwordLabel) {
                    passwordLabel->setText(text.empty() ? std::string("Not set") : std::string("********"));
                    passwordLabel->setTextColor(text.empty() ? nvgRGB(163, 163, 163) : nvgRGB(255, 255, 255));
                }
            }, "Enter Password", "", 128, "");
            return true;
        });
        passwordLabel->addGestureRecognizer(new brls::TapGestureRecognizer(passwordLabel));
    }

    // ── Buttons ────────────────────────────────────────────────────
    // Sign-in button — fires the credentials path verbatim.
    if (loginButton) {
        loginButton->registerClickAction([this](brls::View*) {
            onLoginPressed();
            return true;
        });
    }

    // "Get a new code" — repurposed pin_button. Re-runs the PIN flow
    // so the user gets a fresh code without leaving the screen.
    if (pinButton) {
        pinButton->registerClickAction([this](brls::View*) {
            onPinLoginPressed();
            return true;
        });
    }

    if (offlineButton) {
        offlineButton->registerClickAction([this](brls::View*) {
            onOfflinePressed();
            return true;
        });
    }

    // "Use credentials" — swap the card to the credentials sub-view.
    if (useCredentialsButton) {
        useCredentialsButton->registerClickAction([this](brls::View*) {
            showCredentialsView();
            return true;
        });
    }

    // Back to PIN — return to the device-link view. Also wire B button
    // on the credentials view so D-pad users get the same shortcut.
    if (backToPinButton) {
        backToPinButton->registerClickAction([this](brls::View*) {
            showPinView();
            return true;
        });
    }
    if (credView) {
        credView->registerAction("Back to PIN", brls::ControllerButton::BUTTON_B,
            [this](brls::View*) {
                showPinView();
                return true;
            });
    }

    // Narrow / portrait phone: stack the secondary buttons vertically
    // and let the card take comfortable side margins. Threshold lines
    // up with the responsive break in main_activity / settings_tab.
    if (secondaryRow && brls::Application::contentWidth < 560) {
        secondaryRow->setAxis(brls::Axis::COLUMN);
        if (useCredentialsButton) {
            useCredentialsButton->setMarginRight(0);
            useCredentialsButton->setMarginBottom(10);
        }
    }

    // ── Initial state ──────────────────────────────────────────────
    if (statusLabel) statusLabel->setText("Requesting code…");
    if (expiryLabel) expiryLabel->setText("");
    showPinView();

    // Auto-start the PIN flow so the code is on screen the instant
    // the activity opens — the spec's whole "PIN-first" pivot.
    onPinLoginPressed();
}

// Build the digit-tile row from m_pinAuth.code. Each character lives
// in its own teal-bordered Box with a Label centred inside; codes
// shorter or longer than 4 chars lay out N tiles instead of clipping.
void LoginActivity::renderPinTiles() {
    if (!pinTilesBox) return;
    pinTilesBox->clearViews();

    const std::string& code = m_pinAuth.code;
    if (code.empty()) return;

    for (size_t i = 0; i < code.size(); i++) {
        auto* tile = new brls::Box();
        tile->setWidth(74);
        tile->setHeight(92);
        tile->setCornerRadius(14);
        tile->setBackgroundColor(nvgRGBA(229, 160, 13, 30));  // soft Plex-yellow glow
        tile->setBorderColor(nvgRGB(229, 160, 13));
        tile->setBorderThickness(1);
        tile->setJustifyContent(brls::JustifyContent::CENTER);
        tile->setAlignItems(brls::AlignItems::CENTER);
        if (i + 1 < code.size()) tile->setMarginRight(10);

        auto* digit = new brls::Label();
        std::string s(1, code[i]);
        digit->setText(s);
        digit->setFontSize(46);
        digit->setTextColor(nvgRGB(229, 160, 13));
        tile->addView(digit);

        pinTilesBox->addView(tile);
    }
}

void LoginActivity::showPinView() {
    if (pinView)  pinView->setVisibility(brls::Visibility::VISIBLE);
    if (credView) credView->setVisibility(brls::Visibility::GONE);
    // Default focus lands on "Use credentials" so the user can opt in
    // immediately on a controller. brls picks lastFocusedView on its
    // own when there's history.
    if (useCredentialsButton) {
        brls::Application::giveFocus(useCredentialsButton);
    }
}

void LoginActivity::showCredentialsView() {
    if (pinView)  pinView->setVisibility(brls::Visibility::GONE);
    if (credView) credView->setVisibility(brls::Visibility::VISIBLE);
    if (serverLabel) {
        brls::Application::giveFocus(serverLabel);
    }
}

// Drive an MM:SS countdown off the existing 2-second tick. The PIN
// is valid 5 minutes (m_pinCheckTimer > 150 means expired); each
// tick the remaining seconds = max(0, 300 - m_pinCheckTimer*2).
void LoginActivity::updateExpiryCountdown() {
    if (!expiryLabel) return;
    int remaining = 300 - m_pinCheckTimer * 2;
    if (remaining < 0) remaining = 0;
    int mins = remaining / 60;
    int secs = remaining % 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "Expires in %d:%02d", mins, secs);
    expiryLabel->setText(buf);
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
    dialog->addButton("Cancel", []() {});

    dialog->registerAction("Back", brls::ControllerButton::BUTTON_B, [dialog](brls::View*) {
        dialog->dismiss();
        return true;
    });
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

        if (totalConnections == 0) {
            brls::sync([this, progressDialog]() {
                progressDialog->setStatus("No valid server connections");
                brls::delay(800, [progressDialog]() { progressDialog->dismiss(); });
            });
            return;
        }

        std::atomic<bool> found{false};
        std::atomic<int> completed{0};
        std::mutex winnerMutex;
        std::condition_variable winnerCv;
        std::string winnerUri;
        std::vector<std::thread> workers;
        workers.reserve(totalConnections);

        brls::sync([progressDialog]() {
            progressDialog->setStatus("Testing all server URLs in parallel...");
            progressDialog->setProgress(0.05f);
        });

        // Gate concurrent in-flight HTTP requests to what the platform's
        // network stack can sustain. On Switch (libnx) we observed 17
        // parallel HTTPS probes overrun the resolver: 10/17 came back
        // with "Couldn't resolve host name" within 30 ms while the rest
        // were queued behind a single-threaded NSD resolver. With the
        // semaphore set to platform::maxConcurrentNetworkRequests() (4
        // on Switch, 16 on desktop) the resolver / TLS handshake pool
        // stays healthy and every probe gets a real result instead of
        // a flooded-out error.
        const std::size_t maxInFlight =
            std::max<std::size_t>(1, platform::maxConcurrentNetworkRequests());
        CountingSemaphore sem(static_cast<std::ptrdiff_t>(maxInFlight));

        for (size_t i = 0; i < totalConnections; i++) {
            const auto conn = server.connections[i];
            workers.emplace_back([&, conn, i]() {
                if (*cancelled) {
                    completed.fetch_add(1);
                    return;
                }

                // Hold a permit for the duration of the HTTP request.
                // Workers wait here once maxInFlight are already running;
                // released when the request finishes (success or error).
                sem.acquire();

                // Recheck the cancel flag and the "already found a
                // winner" flag — we may have queued behind a long line
                // and the answer arrived while we were waiting.
                if (*cancelled || found.load()) {
                    sem.release();
                    completed.fetch_add(1);
                    return;
                }

                int timeout = conn.local ? 8 : (conn.relay ? 16 : 12);
                HttpClient http;
                HttpRequest req;
                req.method = "GET";
                req.timeout = timeout;
                req.headers["Accept"] = "application/json";
                req.url = conn.uri + "/?X-Plex-Token=" + HttpClient::urlEncode(client.getAuthToken());

                brls::Logger::debug("Parallel probe {}/{} {}", i + 1, totalConnections, conn.uri);
                HttpResponse resp = http.request(req);
                sem.release();

                if (resp.statusCode == 200 && !found.exchange(true)) {
                    {
                        std::lock_guard<std::mutex> lk(winnerMutex);
                        winnerUri = conn.uri;
                    }
                    winnerCv.notify_one();
                }

                int doneNow = completed.fetch_add(1) + 1;
                brls::sync([progressDialog, doneNow, totalConnections]() {
                    float p = 0.1f + (0.7f * (float)doneNow / (float)totalConnections);
                    progressDialog->setProgress(std::min(0.85f, p));
                    progressDialog->setAttempt(doneNow, totalConnections);
                });
                winnerCv.notify_one();
            });
        }

        {
            std::unique_lock<std::mutex> lk(winnerMutex);
            winnerCv.wait(lk, [&]() {
                return found.load() || completed.load() >= (int)totalConnections || *cancelled;
            });
        }

        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }

        if (!*cancelled && found.load()) {
            std::string uri;
            {
                std::lock_guard<std::mutex> lk(winnerMutex);
                uri = winnerUri;
            }
            brls::Logger::info("Fastest server URL selected: {}", uri);
            if (client.connectToServer(uri, 15)) {
                brls::sync([this, progressDialog, server]() {
                    progressDialog->setStatus("Connected!");
                    progressDialog->setProgress(1.0f);
                    Application::getInstance().saveSettings();
                    if (statusLabel) statusLabel->setText("Connected to " + server.name);
                    brls::delay(500, [progressDialog]() {
                        progressDialog->dismiss();
                        Application::getInstance().pushMainActivity();
                        Application::getInstance().showHomeUserPicker(nullptr);
                    });
                });
                return;
            }
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

void LoginActivity::onOfflinePressed() {
    // Initialize downloads manager to load saved downloads
    DownloadsManager::getInstance().init();

    auto downloads = DownloadsManager::getInstance().getDownloads();
    bool hasDownloads = false;
    for (const auto& d : downloads) {
        if (d.state == DownloadState::COMPLETED) {
            hasDownloads = true;
            break;
        }
    }

    if (!hasDownloads) {
        if (statusLabel) statusLabel->setText("No downloaded content available for offline use");
        return;
    }

    if (statusLabel) statusLabel->setText("Entering offline mode...");

    // Set offline flag so main activity only shows relevant tabs
    Application::getInstance().setOfflineMode(true);

    // Push main activity in offline mode - downloads tab will show available content
    Application::getInstance().pushMainActivity();
}

void LoginActivity::onLoginPressed() {
    if (m_username.empty() || m_password.empty()) {
        if (statusLabel) statusLabel->setText("Please enter username and password");
        return;
    }

    if (statusLabel) statusLabel->setText("Logging in...");

    // Perform login
    PlexClient& client = PlexClient::getInstance();

    // Move the password into a local and immediately best-effort-wipe the
    // member so it doesn't linger on the heap for the rest of the activity's
    // lifetime. `std::string` doesn't give us a guarantee against leftover
    // copies in the SSO/allocator, but scrubbing what we control means a
    // core dump shortly after login no longer trivially recovers the creds.
    std::string password = std::move(m_password);
    {
        // Overwrite the moved-from buffer (if any) before its destructor
        // runs. volatile prevents the compiler from optimizing the writes
        // away once we stop reading the string.
        volatile char* p = const_cast<volatile char*>(m_password.data());
        for (size_t i = 0; i < m_password.size(); i++) p[i] = 0;
        m_password.clear();
    }

    bool loginOk = client.login(m_username, password);
    // Scrub the local copy as well — we no longer need it after login.
    {
        volatile char* p = const_cast<volatile char*>(password.data());
        for (size_t i = 0; i < password.size(); i++) p[i] = 0;
        password.clear();
    }

    if (loginOk) {
        // Capture the master account token now, before any /switch can
        // overwrite m_authToken with a per-user one. This is what
        // fetchHomeUsers / switchHomeUser need.
        Application::getInstance().setMasterAuthToken(
            Application::getInstance().getAuthToken());
        Application::getInstance().setCurrentHomeUserUuid("");
        Application::getInstance().setCurrentHomeUserTitle("");
        Application::getInstance().setUsername(m_username);

        // If server URL provided, use it; otherwise auto-detect
        if (!m_serverUrl.empty()) {
            if (statusLabel) statusLabel->setText("Connecting to server...");
            if (client.connectToServer(m_serverUrl)) {
                Application::getInstance().saveSettings();
                if (statusLabel) statusLabel->setText("Login successful!");
                brls::sync([]() {
                    Application::getInstance().pushMainActivity();
                    Application::getInstance().showHomeUserPicker(nullptr);
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
        // Render the digits as tiles in the new layout. pinCodeLabel
        // is the vestigial hidden Label kept for binding compatibility;
        // keep it updated for any callers that still read it.
        if (pinCodeLabel) pinCodeLabel->setText(m_pinAuth.code);
        renderPinTiles();

        if (statusLabel) statusLabel->setText("Waiting for confirmation…");
        if (statusDot)   statusDot->setBackgroundColor(nvgRGB(229, 160, 13));
        if (getNewCodeRow) getNewCodeRow->setVisibility(brls::Visibility::GONE);

        // Start checking PIN status using RepeatingTimer
        m_pinCheckTimer = 0;
        updateExpiryCountdown();
        m_pinTimer.setCallback([this]() {
            checkPinStatus();
        });
        m_pinTimer.start(2000); // Check every 2 seconds
    } else {
        if (statusLabel) statusLabel->setText("Failed to request PIN");
        if (statusDot)   statusDot->setBackgroundColor(nvgRGB(255, 86, 88));
        if (expiryLabel) expiryLabel->setText("");
        if (getNewCodeRow) getNewCodeRow->setVisibility(brls::Visibility::VISIBLE);
    }
}

void LoginActivity::checkPinStatus() {
    if (!m_pinMode) {
        m_pinTimer.stop();
        return;
    }

    m_pinCheckTimer++;
    updateExpiryCountdown();

    PlexClient& client = PlexClient::getInstance();

    if (client.checkPin(m_pinAuth)) {
        m_pinMode = false;
        m_pinTimer.stop();
        if (pinCodeLabel) pinCodeLabel->setVisibility(brls::Visibility::GONE);

        // Master token is whatever PIN auth just landed in m_authToken;
        // stash it so the home-user picker can list siblings later.
        Application::getInstance().setMasterAuthToken(
            Application::getInstance().getAuthToken());
        Application::getInstance().setCurrentHomeUserUuid("");
        Application::getInstance().setCurrentHomeUserTitle("");

        if (statusLabel) statusLabel->setText("PIN authenticated! Finding servers...");
        if (expiryLabel) expiryLabel->setText("");

        // If server URL provided, use it; otherwise auto-detect
        if (!m_serverUrl.empty()) {
            if (client.connectToServer(m_serverUrl)) {
                Application::getInstance().saveSettings();
                if (statusLabel) statusLabel->setText("Connected!");
                brls::sync([]() {
                    Application::getInstance().pushMainActivity();
                    Application::getInstance().showHomeUserPicker(nullptr);
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
        // PIN expired (5 minutes). Status dot turns red, countdown
        // disappears, "Get a new code" button reveals so the user can
        // retry without leaving the screen.
        m_pinMode = false;
        m_pinTimer.stop();
        if (statusLabel) statusLabel->setText("Code expired");
        if (statusDot)   statusDot->setBackgroundColor(nvgRGB(255, 86, 88));
        if (expiryLabel) expiryLabel->setText("");
        if (getNewCodeRow) {
            getNewCodeRow->setVisibility(brls::Visibility::VISIBLE);
            if (pinButton) brls::Application::giveFocus(pinButton);
        }
        // Clear the displayed tiles too — they're no longer valid.
        if (pinTilesBox) pinTilesBox->clearViews();
    }
}

} // namespace vitaplex
