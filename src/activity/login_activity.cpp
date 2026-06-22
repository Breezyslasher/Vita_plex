/**
 * VitaPlex - Login Activity implementation
 */

#include "activity/login_activity.hpp"
#include "app/application.hpp"
#include "app/plex_client.hpp"
#include "app/downloads_manager.hpp"
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

// ─── Server selection ────────────────────────────────────────────────
// Replaces the plain dropdown with a card dialog: gold eyebrow, "We
// found N servers" headline, then a focusable card per server
// carrying its address + version + connection-type badges (Local /
// Remote / Relay) derived from PlexServer.connections — the same
// list connectToSelectedServer probes in parallel below.

namespace {

// Gold "eyebrow" row at the top of the dialog — gold MDI server
// glyph + caps label in the brand colour. The icon is the bundled
// resources/icons/server.png (rasterised from MDI /server with a gold
// fill at build time).
brls::Box* makeServerEyebrow() {
    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setMarginBottom(8);

    auto* icon = new brls::Image();
    icon->setWidth(18);
    icon->setHeight(18);
    icon->setScalingType(brls::ImageScalingType::FIT);
    icon->setImageFromRes("icons/server.png");
    icon->setMarginRight(10);
    row->addView(icon);

    auto* lbl = new brls::Label();
    lbl->setText("CHOOSE A SERVER");
    lbl->setFontSize(11);
    lbl->setTextColor(nvgRGB(229, 160, 13));
    row->addView(lbl);
    return row;
}

// Connection-type chip — small rounded badge with caps label.
brls::Box* makeConnBadge(const std::string& text, NVGcolor color) {
    auto* chip = new brls::Box();
    chip->setAxis(brls::Axis::ROW);
    chip->setAlignItems(brls::AlignItems::CENTER);
    chip->setCornerRadius(6);
    chip->setBackgroundColor(nvgRGBA(color.r * 255, color.g * 255, color.b * 255, 50));
    chip->setBorderColor(color);
    chip->setBorderThickness(1);
    chip->setPaddingLeft(7);
    chip->setPaddingRight(7);
    chip->setPaddingTop(2);
    chip->setPaddingBottom(2);
    chip->setMarginRight(6);
    auto* lbl = new brls::Label();
    lbl->setText(text);
    lbl->setFontSize(10);
    lbl->setTextColor(color);
    chip->addView(lbl);
    return chip;
}

} // namespace

brls::Box* LoginActivity::buildServerCard(const PlexServer& server,
                                         brls::Dialog* dialog) {
    auto* card = new brls::Box();
    card->setAxis(brls::Axis::ROW);
    card->setAlignItems(brls::AlignItems::CENTER);
    card->setBackgroundColor(nvgRGB(52, 52, 62));
    card->setBorderColor(nvgRGB(67, 67, 74));
    card->setBorderThickness(1);
    card->setCornerRadius(13);
    card->setPaddingLeft(14);
    card->setPaddingRight(14);
    card->setPaddingTop(12);
    card->setPaddingBottom(12);
    card->setMarginBottom(10);
    card->setFocusable(true);

    // Left icon tile.
    auto* tile = new brls::Box();
    tile->setWidth(40);
    tile->setHeight(40);
    tile->setCornerRadius(11);
    tile->setBackgroundColor(nvgRGB(67, 67, 79));
    tile->setJustifyContent(brls::JustifyContent::CENTER);
    tile->setAlignItems(brls::AlignItems::CENTER);
    tile->setMarginRight(12);
    auto* tileGlyph = new brls::Label();
    tileGlyph->setText("●");
    tileGlyph->setFontSize(14);
    tileGlyph->setTextColor(nvgRGB(163, 163, 163));
    tile->addView(tileGlyph);
    card->addView(tile);

    // Name + sub-line column.
    auto* col = new brls::Box();
    col->setAxis(brls::Axis::COLUMN);
    col->setAlignItems(brls::AlignItems::FLEX_START);
    col->setGrow(1.0f);

    auto* nameRow = new brls::Box();
    nameRow->setAxis(brls::Axis::ROW);
    nameRow->setAlignItems(brls::AlignItems::CENTER);
    auto* name = new brls::Label();
    name->setText(server.name.empty() ? std::string("(unnamed server)") : server.name);
    name->setFontSize(16);
    name->setTextColor(nvgRGB(255, 255, 255));
    nameRow->addView(name);
    col->addView(nameRow);

    // Sub-line: address (port) · version — version is parsed by
    // fetchServers from the resource JSON's productVersion field.
    auto* sub = new brls::Label();
    {
        std::string s = server.address;
        if (server.port > 0 && server.port != 32400) {
            s += ":" + std::to_string(server.port);
        }
        if (s.empty() && !server.connections.empty()) s = server.connections.front().uri;
        if (!server.version.empty()) s += "  ·  v" + server.version;
        sub->setText(s);
    }
    sub->setFontSize(12);
    sub->setTextColor(nvgRGB(163, 163, 163));
    sub->setMarginTop(2);
    col->addView(sub);

    // Connection-type badges from the actual server.connections list.
    int nLocal = 0, nRelay = 0, nRemote = 0;
    for (const auto& c : server.connections) {
        if (c.local)      nLocal++;
        else if (c.relay) nRelay++;
        else              nRemote++;
    }
    if (nLocal + nRelay + nRemote > 0) {
        auto* badges = new brls::Box();
        badges->setAxis(brls::Axis::ROW);
        badges->setMarginTop(6);
        if (nLocal > 0)  badges->addView(makeConnBadge("LOCAL",  nvgRGB(62, 207, 142)));
        if (nRemote > 0) badges->addView(makeConnBadge("REMOTE", nvgRGB(137, 241, 242)));
        if (nRelay > 0)  badges->addView(makeConnBadge("RELAY",  nvgRGB(163, 163, 163)));
        col->addView(badges);
    }

    card->addView(col);

    // Right chevron chip — neutral fill, focus glow lit by borealis.
    auto* chev = new brls::Box();
    chev->setWidth(30);
    chev->setHeight(30);
    chev->setCornerRadius(15);
    chev->setBackgroundColor(nvgRGB(67, 67, 79));
    chev->setJustifyContent(brls::JustifyContent::CENTER);
    chev->setAlignItems(brls::AlignItems::CENTER);
    chev->setMarginLeft(10);
    auto* chevLbl = new brls::Label();
    chevLbl->setText(">");
    chevLbl->setFontSize(14);
    chevLbl->setTextColor(nvgRGB(255, 255, 255));
    chev->addView(chevLbl);
    card->addView(chev);

    // A button = connect; reuse the existing parallel-probe path.
    PlexServer s = server;
    card->registerClickAction([this, s, dialog](brls::View*) {
        dialog->dismiss();
        connectToSelectedServer(s);
        return true;
    });
    card->addGestureRecognizer(new brls::TapGestureRecognizer(card));
    return card;
}

void LoginActivity::showServerSelectionDialog(const std::vector<PlexServer>& servers) {
    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setAlignItems(brls::AlignItems::STRETCH);
    // Card dialog — wider than the previous 560 so the long plex.direct
    // URIs + the version suffix on the sub-line have room without
    // shrinking the chevron column. Scales down on narrow viewports.
    int cardW = 720;
    if (brls::Application::contentWidth < 800) {
        cardW = std::max(360, static_cast<int>(brls::Application::contentWidth) - 40);
    }
    content->setWidth(cardW);
    content->setPaddingLeft(28);
    content->setPaddingRight(28);
    content->setPaddingTop(24);
    content->setPaddingBottom(20);
    content->setBackgroundColor(nvgRGB(44, 44, 52));
    content->setCornerRadius(20);

    content->addView(makeServerEyebrow());

    auto* title = new brls::Label();
    {
        std::string s = "We found " + std::to_string(servers.size()) +
                        (servers.size() == 1 ? " server" : " servers");
        title->setText(s);
    }
    title->setFontSize(22);
    title->setTextColor(nvgRGB(255, 255, 255));
    title->setMarginBottom(4);
    content->addView(title);

    auto* subtitle = new brls::Label();
    subtitle->setText("Pick one to connect — we'll test every address and use the fastest.");
    subtitle->setFontSize(13);
    subtitle->setTextColor(nvgRGB(163, 163, 163));
    subtitle->setMarginBottom(16);
    content->addView(subtitle);

    // Cards in a scroll frame so a large server list still fits. The
    // ~100px-per-card budget covers a name row + sub-line + a wrapped
    // version suffix without clipping the badges underneath.
    auto* scroll = new brls::ScrollingFrame();
    scroll->setHeight(std::min(440, (int)servers.size() * 100 + 8));
    scroll->setFocusable(false);
    auto* cardCol = new brls::Box();
    cardCol->setAxis(brls::Axis::COLUMN);
    cardCol->setAlignItems(brls::AlignItems::STRETCH);

    brls::Dialog* dialog = new brls::Dialog(content);

    for (const auto& server : servers) {
        cardCol->addView(buildServerCard(server, dialog));
    }
    scroll->setContentView(cardCol);
    content->addView(scroll);

    // Footer hint row — "A Connect · B Back".
    auto* footer = new brls::Box();
    footer->setAxis(brls::Axis::ROW);
    footer->setAlignItems(brls::AlignItems::CENTER);
    footer->setJustifyContent(brls::JustifyContent::FLEX_END);
    footer->setMarginTop(14);
    auto* hint = new brls::Label();
    hint->setText("A  Connect    B  Back");
    hint->setFontSize(12);
    hint->setTextColor(nvgRGB(124, 124, 132));
    footer->addView(hint);
    content->addView(footer);

    dialog->registerAction("Back", brls::ControllerButton::BUTTON_B,
        [dialog](brls::View*) {
            dialog->dismiss();
            return true;
        });
    dialog->open();
}

// ─── Connecting state ────────────────────────────────────────────────
// Live probe-list dialog driven by the same parallel-200-wins network
// flow as before. Each candidate URI gets its own row; workers update
// its state via brls::sync as the probe transitions queued →
// connecting → 200 OK / Failed. The fastest 200 marks the winner row
// green; the rest dim out with their actual result.

namespace {

// Per-row UI handles. We hand the worker thread a shared_ptr<ConnectingUI>
// that owns vectors of these; brls keeps the actual views alive for the
// lifetime of the dialog (parents own their children).
struct ProbeRow {
    brls::Box*   dot       = nullptr; // state indicator (colour changes)
    brls::Label* uriLabel  = nullptr; // mono-style URI text
    brls::Label* statusLbl = nullptr; // "Connecting…", "200 OK", "Failed", …
};

struct ConnectingUI {
    brls::Dialog* dialog       = nullptr;
    brls::Label*  counterLbl   = nullptr;
    brls::Box*    progressFill = nullptr;
    int           progressTrackW = 360;
    std::vector<ProbeRow> rows;
    // Flipped by the cancel handler so post-async sync()'s know not to
    // touch the now-dismissed dialog.
    std::atomic<bool> dismissed{false};
};

void setRowState(ProbeRow& r, const std::string& status, NVGcolor color) {
    if (r.dot)       r.dot->setBackgroundColor(color);
    if (r.statusLbl) {
        r.statusLbl->setText(status);
        r.statusLbl->setTextColor(color);
    }
}

} // namespace

void LoginActivity::connectToSelectedServer(const PlexServer& server) {
    PlexClient& client = PlexClient::getInstance();
    const size_t totalConnections = server.connections.size();
    auto cancelled = std::make_shared<bool>(false);

    // ── Build the connecting dialog ────────────────────────────────
    auto ui = std::make_shared<ConnectingUI>();

    int cardW = 560;
    if (brls::Application::contentWidth < 640) {
        cardW = static_cast<int>(brls::Application::contentWidth) - 40;
    }
    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setAlignItems(brls::AlignItems::STRETCH);
    content->setWidth(cardW);
    content->setPaddingLeft(28);
    content->setPaddingRight(28);
    content->setPaddingTop(24);
    content->setPaddingBottom(20);
    content->setBackgroundColor(nvgRGB(44, 44, 52));
    content->setCornerRadius(20);

    // Gold spinner + heading row. brls::ProgressSpinner is the
    // canonical "we're working" ring; setting its size keeps it
    // proportional to the heading.
    auto* topRow = new brls::Box();
    topRow->setAxis(brls::Axis::ROW);
    topRow->setAlignItems(brls::AlignItems::CENTER);
    topRow->setMarginBottom(8);
    auto* spinner = new brls::ProgressSpinner();
    spinner->setWidth(28);
    spinner->setHeight(28);
    spinner->setMarginRight(12);
    topRow->addView(spinner);
    auto* headCol = new brls::Box();
    headCol->setAxis(brls::Axis::COLUMN);
    headCol->setGrow(1.0f);
    auto* heading = new brls::Label();
    heading->setText("Connecting to " + (server.name.empty() ? std::string("server") : server.name));
    heading->setFontSize(20);
    heading->setTextColor(nvgRGB(255, 255, 255));
    headCol->addView(heading);
    auto* subline = new brls::Label();
    subline->setText("Testing " + std::to_string(totalConnections) +
                     " address" + (totalConnections == 1 ? "" : "es") +
                     " in parallel — fastest wins.");
    subline->setFontSize(13);
    subline->setTextColor(nvgRGB(163, 163, 163));
    subline->setMarginTop(2);
    headCol->addView(subline);
    topRow->addView(headCol);
    content->addView(topRow);

    // Progress bar + counter.
    auto* meterRow = new brls::Box();
    meterRow->setAxis(brls::Axis::ROW);
    meterRow->setAlignItems(brls::AlignItems::CENTER);
    meterRow->setMarginTop(16);
    meterRow->setMarginBottom(12);

    // Pin the track to a known width so the absolute-positioned fill's
    // width math (progressFill->setWidth(p * trackW)) tracks the visible
    // bar exactly — flex grow would force us to query the realised
    // width at draw time.
    const int progressTrackW = std::max(160, cardW - 28 * 2 - 12 - 150);
    auto* track = new brls::Box();
    track->setWidth(progressTrackW);
    track->setHeight(6);
    track->setCornerRadius(3);
    track->setBackgroundColor(nvgRGB(67, 67, 79));
    track->setMarginRight(12);

    auto* fill = new brls::Box();
    fill->setPositionType(brls::PositionType::ABSOLUTE);
    fill->setPositionLeft(0);
    fill->setPositionTop(0);
    fill->setWidth(0);
    fill->setHeight(6);
    fill->setCornerRadius(3);
    fill->setBackgroundColor(nvgRGB(229, 160, 13));
    track->addView(fill);
    meterRow->addView(track);

    auto* counter = new brls::Label();
    counter->setText("0 of " + std::to_string(totalConnections) + " checked");
    counter->setFontSize(12);
    counter->setTextColor(nvgRGB(163, 163, 163));
    counter->setWidth(150);
    meterRow->addView(counter);

    content->addView(meterRow);

    // Probe list. One row per connection URI; status updates per row
    // come from the async workers below via brls::sync.
    auto* listBox = new brls::Box();
    listBox->setAxis(brls::Axis::COLUMN);
    listBox->setAlignItems(brls::AlignItems::STRETCH);
    listBox->setMarginBottom(14);

    ui->rows.reserve(totalConnections);
    for (size_t i = 0; i < totalConnections; i++) {
        const auto& conn = server.connections[i];
        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setPaddingTop(7);
        row->setPaddingBottom(7);
        row->setPaddingLeft(10);
        row->setPaddingRight(10);
        row->setMarginBottom(4);
        row->setCornerRadius(8);
        row->setBackgroundColor(nvgRGB(52, 52, 62));

        // State indicator.
        auto* dot = new brls::Box();
        dot->setWidth(10);
        dot->setHeight(10);
        dot->setCornerRadius(5);
        dot->setBackgroundColor(nvgRGB(124, 124, 132));   // queued = dim grey
        dot->setMarginRight(10);
        row->addView(dot);

        // Connection-type tag for context (Local / Remote / Relay).
        std::string typeTag = conn.local ? "LOCAL " : (conn.relay ? "RELAY " : "REMOTE ");
        auto* uriLbl = new brls::Label();
        uriLbl->setText(typeTag + conn.uri);
        uriLbl->setFontSize(12);
        uriLbl->setTextColor(nvgRGB(255, 255, 255));
        uriLbl->setGrow(1.0f);
        row->addView(uriLbl);

        // Per-row status text.
        auto* status = new brls::Label();
        status->setText("Queued");
        status->setFontSize(12);
        status->setTextColor(nvgRGB(124, 124, 132));
        row->addView(status);

        listBox->addView(row);
        ui->rows.push_back({ dot, uriLbl, status });
    }
    content->addView(listBox);

    // Cancel button — focusable, cyan focus ring is borealis-native.
    auto* footer = new brls::Box();
    footer->setAxis(brls::Axis::ROW);
    footer->setJustifyContent(brls::JustifyContent::FLEX_END);

    auto* cancelBtn = new brls::Button();
    cancelBtn->setText("Cancel");
    cancelBtn->setWidth(140);
    footer->addView(cancelBtn);
    content->addView(footer);

    brls::Dialog* dialog = new brls::Dialog(content);
    ui->dialog         = dialog;
    ui->counterLbl     = counter;
    ui->progressFill   = fill;
    ui->progressTrackW = progressTrackW;

    cancelBtn->registerClickAction([dialog, cancelled, ui](brls::View*) {
        *cancelled = true;
        ui->dismissed.store(true);
        dialog->dismiss();
        return true;
    });
    dialog->registerAction("Cancel", brls::ControllerButton::BUTTON_B,
        [dialog, cancelled, ui](brls::View*) {
            *cancelled = true;
            ui->dismissed.store(true);
            dialog->dismiss();
            return true;
        });
    dialog->open();

    if (totalConnections == 0) {
        if (heading) heading->setText("No valid server connections");
        brls::delay(800, [ui]() { if (ui && ui->dialog) ui->dialog->dismiss(); });
        return;
    }

    // ── Async parallel probe (unchanged network logic) ─────────────
    asyncRun([this, server, ui, totalConnections, cancelled]() {
        PlexClient& client = PlexClient::getInstance();

        std::atomic<bool> found{false};
        std::atomic<int> completed{0};
        std::atomic<int> winnerIdx{-1};
        std::mutex winnerMutex;
        std::condition_variable winnerCv;
        std::string winnerUri;
        std::vector<std::thread> workers;
        workers.reserve(totalConnections);

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
                if (*cancelled) { completed.fetch_add(1); return; }

                sem.acquire();
                if (*cancelled || found.load()) {
                    sem.release();
                    completed.fetch_add(1);
                    return;
                }

                // Flip the row to "connecting" — gold dot + gold text.
                brls::sync([ui, i]() {
                    if (ui->dismissed.load()) return;
                    if (i < ui->rows.size())
                        setRowState(ui->rows[i], "Connecting…", nvgRGB(229, 160, 13));
                });

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

                bool is200 = (resp.statusCode == 200);
                bool weAreWinner = false;
                if (is200 && !found.exchange(true)) {
                    {
                        std::lock_guard<std::mutex> lk(winnerMutex);
                        winnerUri = conn.uri;
                    }
                    winnerIdx.store(static_cast<int>(i));
                    weAreWinner = true;
                    winnerCv.notify_one();
                }

                // Update this row's terminal state.
                int statusCode = resp.statusCode;
                brls::sync([ui, i, is200, weAreWinner, statusCode]() {
                    if (ui->dismissed.load()) return;
                    if (i >= ui->rows.size()) return;
                    if (weAreWinner) {
                        setRowState(ui->rows[i], "200 OK ✓", nvgRGB(62, 207, 142));
                    } else if (is200) {
                        // Reached but a faster URL already won.
                        setRowState(ui->rows[i], "Slower (200 OK)", nvgRGB(124, 124, 132));
                    } else if (statusCode == 0) {
                        setRowState(ui->rows[i], "Timeout", nvgRGB(255, 86, 88));
                    } else {
                        setRowState(ui->rows[i], "HTTP " + std::to_string(statusCode),
                                    nvgRGB(255, 86, 88));
                    }
                });

                int doneNow = completed.fetch_add(1) + 1;
                brls::sync([ui, doneNow, totalConnections]() {
                    if (ui->dismissed.load()) return;
                    float p = (float)doneNow / (float)totalConnections;
                    if (ui->counterLbl) {
                        ui->counterLbl->setText(std::to_string(doneNow) + " of " +
                                                std::to_string(totalConnections) +
                                                " checked");
                    }
                    if (ui->progressFill) {
                        ui->progressFill->setWidth(p * (float)ui->progressTrackW);
                    }
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
                brls::sync([this, ui, server]() {
                    if (!ui->dismissed.load() && ui->dialog) {
                        // Snap progress to 100% for the brief beat
                        // before pushing main.
                        if (ui->progressFill)
                            ui->progressFill->setWidth(ui->progressTrackW);
                    }
                    Application::getInstance().saveSettings();
                    if (statusLabel) statusLabel->setText("Connected to " + server.name);
                    brls::delay(500, [ui]() {
                        ui->dismissed.store(true);
                        if (ui->dialog) ui->dialog->dismiss();
                        Application::getInstance().pushMainActivity();
                        Application::getInstance().showHomeUserPicker(nullptr);
                    });
                });
                return;
            }
        }

        // All connections failed (or user cancelled).
        if (!*cancelled) {
            brls::sync([this, ui, server, totalConnections]() {
                if (statusLabel) statusLabel->setText("Failed to connect to " + server.name);
                brls::Logger::error("All {} connections failed for {}", totalConnections, server.name);
                brls::delay(1800, [ui]() {
                    ui->dismissed.store(true);
                    if (ui->dialog) ui->dialog->dismiss();
                });
            });
        }
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
