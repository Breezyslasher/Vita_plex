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

#include <borealis/core/time.hpp>

#include <memory>
#include <thread>
#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <cmath>

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

// ───────────────────────────────────────────────────────────────────
// Server-select / connecting visual toolkit.
//
// The dialog is recreated from the HTML design reference with native
// borealis Box / Label trees: Plex gold marks brand / owned / active
// state, while the controller focus highlight stays borealis-native
// cyan so "where am I" never blends into "what's primary".
//
// The icons are hand-painted with nanovg rather than a web/icon font so
// nothing new has to be bundled. The shell glyph is Material Design
// Icons "server" (https://pictogrammers.com/library/mdi/icon/server/) —
// three stacked rack units with cut-out indicator LEDs, filled with the
// tint colour; the rest (chevron / plus / check / cross / ring) are
// simple stroked paths.
// ───────────────────────────────────────────────────────────────────
namespace {

// Design tokens (mirror the spec / Application::getTheme dark table).
const NVGcolor kWhite    = nvgRGB(255, 255, 255);
const NVGcolor kMuted    = nvgRGB(163, 163, 163);
const NVGcolor kDim      = nvgRGB(124, 124, 132);
const NVGcolor kDialogBg = nvgRGB(44, 44, 52);
const NVGcolor kCard     = nvgRGB(52, 52, 62);
const NVGcolor kRaised   = nvgRGB(67, 67, 79);
const NVGcolor kLine     = nvgRGB(67, 67, 74);
const NVGcolor kGold     = nvgRGB(229, 160, 13);
const NVGcolor kGoldDeep = nvgRGB(200, 135, 10);
const NVGcolor kGoldInk  = nvgRGB(36, 28, 8);
const NVGcolor kGreen    = nvgRGB(62, 207, 142);
const NVGcolor kGreenTxt = nvgRGB(95, 224, 166);
const NVGcolor kCyan     = nvgRGB(137, 241, 242);
const NVGcolor kCyanTxt  = nvgRGB(158, 240, 241);
const NVGcolor kRed      = nvgRGB(255, 86, 88);

// Copy of a colour with an explicit 0..1 alpha.
inline NVGcolor colA(NVGcolor c, float a) { c.a = a; return c; }

enum class Glyph { Server, Chevron, Plus, Check, Cross, Ring };

// Filled MDI "server": three rounded rack units, each with a 2x2 LED and a
// 1x2 tick cut out as holes (24x24 viewBox, mapped into [x,x+side]²).
void drawServerGlyph(NVGcontext* vg, float x, float y, float side, NVGcolor color) {
    const float s = side / 24.0f;
    auto PX = [&](float v) { return x + v * s; };
    auto PY = [&](float v) { return y + v * s; };
    nvgBeginPath(vg);
    for (int i = 0; i < 3; i++) {
        const float top = 1.0f + 8.0f * i;
        nvgRoundedRect(vg, PX(4), PY(top), 16.0f * s, 6.0f * s, 1.0f * s);
        const float ly = top + 2.0f;
        nvgRect(vg, PX(5), PY(ly), 2.0f * s, 2.0f * s);  // square LED
        nvgPathWinding(vg, NVG_HOLE);
        nvgRect(vg, PX(9), PY(ly), 1.0f * s, 2.0f * s);  // tick
        nvgPathWinding(vg, NVG_HOLE);
    }
    nvgFillColor(vg, color);
    nvgFill(vg);
}

// Simple stroked glyphs (chevron / plus / check / cross / ring).
void drawStrokeGlyph(NVGcontext* vg, Glyph g, float x, float y, float side,
                     NVGcolor color, float strokeW) {
    const float s = side / 24.0f;
    auto PX = [&](float v) { return x + v * s; };
    auto PY = [&](float v) { return y + v * s; };
    nvgStrokeColor(vg, color);
    nvgStrokeWidth(vg, strokeW * s);
    nvgLineCap(vg, NVG_ROUND);
    nvgLineJoin(vg, NVG_ROUND);
    nvgBeginPath(vg);
    switch (g) {
        case Glyph::Chevron:
            nvgMoveTo(vg, PX(9.5f), PY(6)); nvgLineTo(vg, PX(15.5f), PY(12)); nvgLineTo(vg, PX(9.5f), PY(18));
            break;
        case Glyph::Plus:
            nvgMoveTo(vg, PX(12), PY(5)); nvgLineTo(vg, PX(12), PY(19));
            nvgMoveTo(vg, PX(5), PY(12)); nvgLineTo(vg, PX(19), PY(12));
            break;
        case Glyph::Check:
            nvgMoveTo(vg, PX(5), PY(12.5f)); nvgLineTo(vg, PX(9.5f), PY(17)); nvgLineTo(vg, PX(19), PY(7));
            break;
        case Glyph::Cross:
            nvgMoveTo(vg, PX(6), PY(6)); nvgLineTo(vg, PX(18), PY(18));
            nvgMoveTo(vg, PX(18), PY(6)); nvgLineTo(vg, PX(6), PY(18));
            break;
        case Glyph::Ring:
            nvgCircle(vg, PX(12), PY(12), 8.0f * s);
            break;
        default: break;
    }
    nvgStroke(vg);
}

// A Box that paints a single glyph centered in its content box, tinted.
class GlyphView : public brls::Box {
public:
    GlyphView(Glyph g, NVGcolor color, float strokeW = 2.0f)
        : m_glyph(g), m_color(color), m_strokeW(strokeW) {}

    void setColor(NVGcolor c) { m_color = c; }

    void draw(NVGcontext* vg, float x, float y, float w, float h,
              brls::Style style, brls::FrameContext* ctx) override {
        brls::Box::draw(vg, x, y, w, h, style, ctx);
        const float side = std::min(w, h);
        if (side <= 0) return;
        const float gx = x + (w - side) / 2.0f;
        const float gy = y + (h - side) / 2.0f;
        NVGcolor c = colA(m_color, m_color.a * this->getAlpha());
        if (m_glyph == Glyph::Server) drawServerGlyph(vg, gx, gy, side, c);
        else                          drawStrokeGlyph(vg, m_glyph, gx, gy, side, c, m_strokeW);
    }

private:
    Glyph m_glyph;
    NVGcolor m_color;
    float m_strokeW;
};

// Gold "spinning" ring (faint track + sweeping arc) with the server glyph
// in the middle — the connecting-state hero. Rotation is time-driven so it
// keeps moving every frame without an explicit animation object.
class SpinnerRing : public brls::Box {
public:
    void draw(NVGcontext* vg, float x, float y, float w, float h,
              brls::Style style, brls::FrameContext* ctx) override {
        brls::Box::draw(vg, x, y, w, h, style, ctx);
        const float a    = this->getAlpha();
        const float cx   = x + w / 2.0f, cy = y + h / 2.0f;
        const float diam = std::min(w, h);
        const float r    = diam / 2.0f - 5.0f;
        const float sw   = 5.0f * (diam / 88.0f);

        nvgBeginPath(vg); nvgCircle(vg, cx, cy, r);
        nvgStrokeColor(vg, colA(kGold, 0.18f * a)); nvgStrokeWidth(vg, sw); nvgStroke(vg);

        const double t  = brls::getCPUTimeUsec() / 1000000.0;
        const float  a0 = static_cast<float>(t * 2.0 * NVG_PI);  // ~1 rev / sec
        nvgBeginPath(vg);
        nvgArc(vg, cx, cy, r, a0, a0 + NVG_PI * 0.85f, NVG_CW);
        nvgStrokeColor(vg, colA(kGold, a)); nvgStrokeWidth(vg, sw);
        nvgLineCap(vg, NVG_ROUND); nvgStroke(vg);

        const float side = diam * 0.36f;
        drawServerGlyph(vg, cx - side / 2.0f, cy - side / 2.0f, side, colA(kGold, a));
    }
};

// One probe row's status pip. Its state is flipped from the UI thread as
// the real per-URI request progresses; busy spins, the rest are static.
class ProbeIcon : public brls::Box {
public:
    enum class State { Queued, Busy, Win, Fail };
    void setState(State s) { m_state = s; }

    void draw(NVGcontext* vg, float x, float y, float w, float h,
              brls::Style style, brls::FrameContext* ctx) override {
        brls::Box::draw(vg, x, y, w, h, style, ctx);
        const float a    = this->getAlpha();
        const float side = std::min(w, h);
        const float gx   = x + (w - side) / 2.0f;
        const float gy   = y + (h - side) / 2.0f;
        switch (m_state) {
            case State::Queued:
                drawStrokeGlyph(vg, Glyph::Ring, gx, gy, side, colA(kDim, a), 2.4f);
                break;
            case State::Win:
                drawStrokeGlyph(vg, Glyph::Check, gx, gy, side, colA(kGreen, a), 2.6f);
                break;
            case State::Fail:
                drawStrokeGlyph(vg, Glyph::Cross, gx, gy, side, colA(kDim, a), 2.4f);
                break;
            case State::Busy: {
                const float cx = x + w / 2.0f, cy = y + h / 2.0f;
                const float r  = side / 2.0f - 2.0f;
                const double t = brls::getCPUTimeUsec() / 1000000.0;
                const float a0 = static_cast<float>(t * 2.0 * NVG_PI);
                nvgBeginPath(vg);
                nvgArc(vg, cx, cy, r, a0, a0 + NVG_PI * 1.4f, NVG_CW);
                nvgStrokeColor(vg, colA(kGold, a));
                nvgStrokeWidth(vg, 2.4f * (side / 18.0f));
                nvgLineCap(vg, NVG_ROUND); nvgStroke(vg);
                break;
            }
        }
    }

private:
    State m_state = State::Queued;
};

// Rounded track + horizontal gold-gradient fill driven by setFraction().
class GradientBar : public brls::Box {
public:
    void setFraction(float f) { m_fraction = std::max(0.0f, std::min(1.0f, f)); }

    void draw(NVGcontext* vg, float x, float y, float w, float h,
              brls::Style style, brls::FrameContext* ctx) override {
        brls::Box::draw(vg, x, y, w, h, style, ctx);
        const float a = this->getAlpha();
        const float r = h / 2.0f;
        nvgBeginPath(vg); nvgRoundedRect(vg, x, y, w, h, r);
        nvgFillColor(vg, colA(kWhite, 0.12f * a)); nvgFill(vg);

        const float fw = w * m_fraction;
        if (fw > 1.0f) {
            NVGpaint p = nvgLinearGradient(vg, x, y, x + w, y, colA(kGoldDeep, a), colA(kGold, a));
            nvgBeginPath(vg); nvgRoundedRect(vg, x, y, fw, h, r);
            nvgFillPaint(vg, p); nvgFill(vg);
        }
    }

private:
    float m_fraction = 0.0f;
};

// Focusable server card. Native cyan highlight is left to borealis; on
// focus the chevron chip flips to gold so active-vs-focus stay distinct.
class ServerCard : public brls::Box {
public:
    void bindChevron(brls::Box* chip, GlyphView* chevron) {
        m_chip = chip; m_chevron = chevron;
    }
    void onFocusGained() override {
        brls::Box::onFocusGained();
        if (m_chip)    m_chip->setBackgroundColor(kGold);
        if (m_chevron) m_chevron->setColor(kGoldInk);
    }
    void onFocusLost() override {
        brls::Box::onFocusLost();
        if (m_chip)    m_chip->setBackgroundColor(kRaised);
        if (m_chevron) m_chevron->setColor(kMuted);
    }

private:
    brls::Box* m_chip       = nullptr;
    GlyphView* m_chevron    = nullptr;
};

// Shared per-probe UI handle. The probe worker threads only ever touch
// these pointers through brls::sync (UI thread) and only while `alive`,
// which the UI thread clears the instant the connecting card is torn down
// — so a result landing after Cancel / success is a guarded no-op rather
// than a dangling write.
struct ConnectingUI {
    std::atomic<bool> alive { true };
    std::vector<ProbeIcon*>   icons;
    std::vector<brls::Label*> statuses;
    brls::Label* counter = nullptr;
    GradientBar* bar     = nullptr;
    int total            = 0;
};

// Truncate to a character budget with a trailing ellipsis (keeps long
// plex.direct hostnames from overflowing the card / probe row).
std::string ellipsize(const std::string& s, size_t budget) {
    if (s.size() <= budget) return s;
    if (budget <= 3) return s.substr(0, budget);
    return s.substr(0, budget - 3) + "...";
}

// Small rounded "Local / Remote / Relay" pill: coloured dot + label.
brls::Box* makeBadge(const std::string& text, NVGcolor textColor,
                     NVGcolor dotColor, NVGcolor bg) {
    auto* b = new brls::Box();
    b->setAxis(brls::Axis::ROW);
    b->setAlignItems(brls::AlignItems::CENTER);
    b->setHeight(21);
    b->setCornerRadius(6);
    b->setPaddingLeft(8);
    b->setPaddingRight(8);
    b->setBackgroundColor(bg);
    b->setMarginRight(6);
    b->setMarginTop(6);

    auto* dot = new brls::Box();
    dot->setWidth(6); dot->setHeight(6); dot->setCornerRadius(3);
    dot->setBackgroundColor(dotColor); dot->setMarginRight(6);
    b->addView(dot);

    auto* l = new brls::Label();
    l->setText(text); l->setFontSize(11); l->setTextColor(textColor);
    b->addView(l);
    return b;
}

}  // namespace

LoginActivity::LoginActivity() {
    m_alive = std::make_shared<std::atomic<bool>>(true);
    brls::Logger::debug("LoginActivity created");
}

LoginActivity::~LoginActivity() {
    // Disarm the process-lifetime orientation callback so a rotation that
    // fires after this activity is gone can't touch a freed `this`.
    if (m_alive) *m_alive = false;
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

    // Re-flow the server modal when the viewport orientation flips (a
    // portrait phone going near-full-width, badges/probes re-measuring).
    // Only the list state is rebuilt; a mid-probe rotation leaves the
    // connecting card alone so live results aren't reset. Guarded by the
    // shared alive flag — see the destructor.
    auto alive = m_alive;
    platform::onOrientationChanged([this, alive]() {
        if (!alive || !*alive) return;
        if (m_overlayMode == 1) showServerSelectionContent();
    });
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
    // Stash the resolved list owned-first (default focus + visual priority)
    // so the card state can be rebuilt verbatim on orientation change, and
    // float the redesigned picker.
    m_servers = servers;
    std::stable_sort(m_servers.begin(), m_servers.end(),
        [](const PlexServer& a, const PlexServer& b) { return a.owned && !b.owned; });
    showServerSelectionContent();
}

void LoginActivity::showServerSelectionContent() {
    const float vw      = brls::Application::contentWidth;
    const bool  narrow  = platform::isPortrait() || vw < 600;
    const float dialogW = narrow ? std::max(320.0f, vw - 32.0f) : 560.0f;

    // ── Card shell ──────────────────────────────────────────────────
    auto* card = new brls::Box();
    card->setAxis(brls::Axis::COLUMN);
    card->setWidth(dialogW);
    card->setMaxWidthPercentage(100.0f);
    card->setBackgroundColor(kDialogBg);
    card->setCornerRadius(20);
    card->setBorderColor(kLine);
    card->setBorderThickness(1);
    card->setShadowType(brls::ShadowType::GENERIC);

    // ── Header: gold eyebrow + title + subtitle ─────────────────────
    auto* head = new brls::Box();
    head->setAxis(brls::Axis::COLUMN);
    head->setPadding(24, 26, 18, 26);
    head->setLineBottom(1);
    head->setLineColor(kLine);

    auto* eyebrow = new brls::Box();
    eyebrow->setAxis(brls::Axis::ROW);
    eyebrow->setAlignItems(brls::AlignItems::CENTER);
    eyebrow->setMarginBottom(9);
    auto* eg = new GlyphView(Glyph::Server, kGold);
    eg->setWidth(15); eg->setHeight(15); eg->setMarginRight(8);
    eyebrow->addView(eg);
    auto* el = new brls::Label();
    el->setText("CHOOSE A SERVER");
    el->setFontSize(12); el->setTextColor(kGold);
    eyebrow->addView(el);
    head->addView(eyebrow);

    auto* title = new brls::Label();
    title->setText("We found " + std::to_string(m_servers.size()) + " servers");
    title->setFontSize(23); title->setTextColor(kWhite);
    head->addView(title);

    auto* sub = new brls::Label();
    sub->setText("Pick one to connect — we'll test every address and use the fastest.");
    sub->setFontSize(14); sub->setTextColor(kMuted); sub->setMarginTop(4);
    head->addView(sub);
    card->addView(head);

    // ── Body: one focusable card per server ─────────────────────────
    auto* body = new brls::Box();
    body->setAxis(brls::Axis::COLUMN);
    body->setPadding(16, 16, 16, 16);
    brls::View* firstCard = nullptr;
    for (const auto& s : m_servers) {
        PlexServer sv = s;  // capture by value for the activation lambda
        auto* c = buildServerCard(sv, [this, sv]() {
            // Defer: connectToSelectedServer swaps the modal content, which
            // frees this very card while its A / tap action is dispatching.
            m_returnToList = true;
            brls::sync([this, sv]() { connectToSelectedServer(sv); });
        });
        if (!firstCard) firstCard = c;
        body->addView(c);
    }
    card->addView(body);

    // ── Footer: "Enter address manually" ────────────────────────────
    auto* foot = new brls::Box();
    foot->setAxis(brls::Axis::ROW);
    foot->setAlignItems(brls::AlignItems::CENTER);
    foot->setPadding(14, 20, 14, 20);
    foot->setLineTop(1);
    foot->setLineColor(kLine);
    // Right-align the manual affordance on wide layouts (left on narrow).
    if (!narrow) {
        auto* spacer = new brls::Box();
        spacer->setGrow(1.0f);
        foot->addView(spacer);
    }

    auto* add = new brls::Box();
    add->setAxis(brls::Axis::ROW);
    add->setAlignItems(brls::AlignItems::CENTER);
    add->setPadding(6, 8, 6, 8);
    add->setCornerRadius(8);
    add->setFocusable(true);
    add->setHighlightCornerRadius(8);
    auto* pg = new GlyphView(Glyph::Plus, kGold, 2.2f);
    pg->setWidth(15); pg->setHeight(15); pg->setMarginRight(6);
    add->addView(pg);
    auto* al = new brls::Label();
    al->setText("Enter address manually");
    al->setFontSize(13); al->setTextColor(kGold);
    add->addView(al);
    add->registerClickAction([this](brls::View*) { onEnterAddressManually(); return true; });
    add->addGestureRecognizer(new brls::TapGestureRecognizer(add));
    foot->addView(add);
    card->addView(foot);

    // B closes the modal and returns to the login screen. Defer the pop —
    // we're inside this card's own B action and popActivity frees it.
    card->registerAction("Back", brls::ControllerButton::BUTTON_B,
        [this](brls::View*) {
            brls::sync([this]() { dismissDialog(); });
            return true;
        });

    m_overlayMode = 1;
    presentDialogCard(card, firstCard);
}

brls::Box* LoginActivity::buildServerCard(const PlexServer& server,
                                          std::function<void()> onActivate) {
    const bool owned  = server.owned;
    const bool narrow = platform::isPortrait() || brls::Application::contentWidth < 600;

    auto* card = new ServerCard();
    card->setAxis(brls::Axis::ROW);
    card->setAlignItems(brls::AlignItems::CENTER);
    card->setPadding(14, 15, 14, 15);
    card->setCornerRadius(13);
    card->setBackgroundColor(kCard);
    card->setBorderColor(kLine);
    card->setBorderThickness(1);
    card->setMarginBottom(9);
    card->setFocusable(true);                 // borealis paints the cyan ring
    card->setHighlightCornerRadius(13);
    card->registerClickAction([onActivate](brls::View*) { onActivate(); return true; });
    card->addGestureRecognizer(new brls::TapGestureRecognizer(card));

    // Left icon tile — gold-tinted for owned servers, neutral for shared.
    auto* tile = new brls::Box();
    tile->setWidth(44); tile->setHeight(44);
    tile->setCornerRadius(11);
    tile->setJustifyContent(brls::JustifyContent::CENTER);
    tile->setAlignItems(brls::AlignItems::CENTER);
    tile->setMarginRight(14);
    tile->setBackgroundColor(owned ? colA(kGold, 0.16f) : kRaised);
    auto* tileGlyph = new GlyphView(Glyph::Server, owned ? kGold : kMuted);
    tileGlyph->setWidth(23); tileGlyph->setHeight(23);
    tile->addView(tileGlyph);
    card->addView(tile);

    // Info column.
    auto* info = new brls::Box();
    info->setAxis(brls::Axis::COLUMN);
    info->setGrow(1.0f);
    info->setShrink(1.0f);

    // Name row + OWNED chip (owned) / "Shared by X" (shared).
    auto* nameRow = new brls::Box();
    nameRow->setAxis(brls::Axis::ROW);
    nameRow->setAlignItems(brls::AlignItems::CENTER);
    auto* name = new brls::Label();
    name->setText(server.name);
    name->setFontSize(16);
    name->setTextColor(kWhite);
    name->setSingleLine(true);
    nameRow->addView(name);
    if (owned) {
        auto* chip = new brls::Box();
        chip->setAxis(brls::Axis::ROW);
        chip->setAlignItems(brls::AlignItems::CENTER);
        chip->setHeight(19);
        chip->setCornerRadius(5);
        chip->setPaddingLeft(7); chip->setPaddingRight(7);
        chip->setMarginLeft(9);
        chip->setBackgroundColor(colA(kGold, 0.18f));
        auto* cl = new brls::Label();
        cl->setText("OWNED"); cl->setFontSize(11); cl->setTextColor(kGold);
        chip->addView(cl);
        nameRow->addView(chip);
    } else if (!server.sourceTitle.empty()) {
        auto* shared = new brls::Label();
        shared->setText("Shared by " + server.sourceTitle);
        shared->setFontSize(12); shared->setTextColor(kMuted);
        shared->setMarginLeft(9); shared->setSingleLine(true);
        nameRow->addView(shared);
    }
    info->addView(nameRow);

    // Sub-line: host (mono-ish, dim, truncated) · version.
    std::string host;
    for (const auto& c : server.connections) {
        if (!c.local && !c.relay) { host = c.uri; break; }  // prefer the remote uri
    }
    if (host.empty() && !server.connections.empty()) host = server.connections[0].uri;
    if (host.empty()) host = server.address;

    auto* subRow = new brls::Box();
    subRow->setAxis(brls::Axis::ROW);
    subRow->setAlignItems(brls::AlignItems::CENTER);
    subRow->setMarginTop(3);
    auto* hostL = new brls::Label();
    hostL->setText(ellipsize(host, narrow ? 26 : 42));
    hostL->setFontSize(12.5f);
    hostL->setTextColor(kDim);
    hostL->setSingleLine(true);
    subRow->addView(hostL);
    if (!server.version.empty()) {
        auto* dot = new brls::Box();
        dot->setWidth(3); dot->setHeight(3); dot->setCornerRadius(2);
        dot->setBackgroundColor(kDim);
        dot->setMarginLeft(9); dot->setMarginRight(9);
        subRow->addView(dot);
        auto* ver = new brls::Label();
        ver->setText("v" + server.version);
        ver->setFontSize(12.5f); ver->setTextColor(kDim);
        ver->setSingleLine(true);
        subRow->addView(ver);
    }
    info->addView(subRow);

    // Connection-type badges, derived from the exact connection list the
    // parallel probe will hit (local / relay / otherwise remote).
    bool hasLocal = false, hasRemote = false, hasRelay = false;
    for (const auto& c : server.connections) {
        if (c.local)      hasLocal  = true;
        else if (c.relay) hasRelay  = true;
        else              hasRemote = true;
    }
    auto* badges = new brls::Box();
    badges->setAxis(brls::Axis::ROW);
    badges->setAlignItems(brls::AlignItems::CENTER);
    if (hasLocal)  badges->addView(makeBadge("Local",  kGreenTxt, kGreen, colA(kGreen, 0.15f)));
    if (hasRemote) badges->addView(makeBadge("Remote", kCyanTxt,  kCyan,  colA(kCyan,  0.13f)));
    if (hasRelay)  badges->addView(makeBadge("Relay",  nvgRGB(188, 188, 196), kDim, colA(kMuted, 0.14f)));
    info->addView(badges);
    card->addView(info);

    // Right chevron chip — fills gold on focus (see ServerCard).
    auto* chip = new brls::Box();
    chip->setWidth(30); chip->setHeight(30);
    chip->setCornerRadius(15);
    chip->setJustifyContent(brls::JustifyContent::CENTER);
    chip->setAlignItems(brls::AlignItems::CENTER);
    chip->setMarginLeft(10);
    chip->setBackgroundColor(kRaised);
    auto* chevron = new GlyphView(Glyph::Chevron, kMuted, 2.2f);
    chevron->setWidth(16); chevron->setHeight(16);
    chip->addView(chevron);
    card->addView(chip);
    card->bindChevron(chip, chevron);

    return card;
}

void LoginActivity::onEnterAddressManually() {
    // Reuse the existing single-URL probe path. If we're standing on the
    // server list, Cancel from the connecting state returns there; if not,
    // it closes the modal.
    const bool fromList = (m_dialogScrim != nullptr);
    brls::Application::getImeManager()->openForText(
        [this, fromList](std::string url) {
            if (url.empty()) return;
            PlexServer manual;
            manual.name  = "Manual server";
            manual.owned = false;
            ServerConnection c;
            c.uri = url; c.local = false; c.relay = false;
            manual.connections.push_back(c);
            m_returnToList = fromList;
            connectToSelectedServer(manual);
        },
        "Enter Server URL", "http://your-server:32400", 256, m_serverUrl);
}

void LoginActivity::presentDialogCard(brls::View* card, brls::View* focusView) {
    if (!m_dialogScrim) {
        // First show: build the dim scrim and push it as a modal activity.
        m_dialogScrim = new brls::Box();
        m_dialogScrim->setWidthPercentage(100.0f);
        m_dialogScrim->setHeightPercentage(100.0f);
        m_dialogScrim->setJustifyContent(brls::JustifyContent::CENTER);
        m_dialogScrim->setAlignItems(brls::AlignItems::CENTER);
        m_dialogScrim->setBackgroundColor(nvgRGBA(10, 11, 14, 200));
        m_dialogScrim->addView(card);
        brls::Application::pushActivity(new brls::Activity(m_dialogScrim));
    } else {
        // Subsequent states swap the scrim's single child (frees the old one).
        m_dialogScrim->clearViews();
        m_dialogScrim->addView(card);
    }
    if (focusView) brls::Application::giveFocus(focusView);
}

void LoginActivity::dismissDialog() {
    if (m_dialogScrim) {
        m_dialogScrim = nullptr;   // null first so a stray callback is inert
        m_overlayMode = 0;
        brls::Application::popActivity();
    }
}

void LoginActivity::connectToSelectedServer(const PlexServer& server) {
    const size_t totalConnections = server.connections.size();

    // Shared UI handle the probe workers update through brls::sync, plus the
    // cancel flag the existing probe loop already honours.
    auto ui        = std::make_shared<ConnectingUI>();
    ui->total      = static_cast<int>(totalConnections);
    auto cancelled = std::make_shared<std::atomic<bool>>(false);

    // ── Connecting card (same shell, swapped into the modal) ────────
    const float vw      = brls::Application::contentWidth;
    const bool  narrow  = platform::isPortrait() || vw < 600;
    const float dialogW = narrow ? std::max(320.0f, vw - 32.0f) : 520.0f;
    const float innerW  = std::max(220.0f, dialogW - 60.0f);

    auto* card = new brls::Box();
    card->setAxis(brls::Axis::COLUMN);
    card->setWidth(dialogW);
    card->setMaxWidthPercentage(100.0f);
    card->setAlignItems(brls::AlignItems::CENTER);
    card->setBackgroundColor(kDialogBg);
    card->setCornerRadius(20);
    card->setBorderColor(kLine);
    card->setBorderThickness(1);
    card->setShadowType(brls::ShadowType::GENERIC);
    card->setPadding(30, 30, 26, 30);

    auto* ring = new SpinnerRing();
    ring->setWidth(88); ring->setHeight(88); ring->setMarginBottom(20);
    card->addView(ring);

    auto* heading = new brls::Label();
    heading->setText("Connecting to " + server.name);
    heading->setFontSize(21); heading->setTextColor(kWhite);
    heading->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    card->addView(heading);

    auto* where = new brls::Label();
    where->setText("Testing " + std::to_string(totalConnections) +
                   (totalConnections == 1 ? " address" : " addresses") +
                   " in parallel — fastest wins");
    where->setFontSize(14); where->setTextColor(kMuted); where->setMarginTop(6);
    where->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    card->addView(where);

    auto* bar = new GradientBar();
    bar->setWidth(innerW); bar->setHeight(7);
    bar->setMarginTop(22); bar->setMarginBottom(10);
    ui->bar = bar;
    card->addView(bar);

    auto* counter = new brls::Label();
    counter->setText("0 of " + std::to_string(totalConnections) + " checked");
    counter->setFontSize(12.5f); counter->setTextColor(kDim);
    counter->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    ui->counter = counter;
    card->addView(counter);

    // Probe list — one row per candidate URI, wired to its real request.
    auto* probes = new brls::Box();
    probes->setAxis(brls::Axis::COLUMN);
    probes->setWidth(innerW);
    for (size_t i = 0; i < totalConnections; i++) {
        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setPadding(8, 11, 8, 11);
        row->setCornerRadius(9);
        row->setBackgroundColor(kCard);
        row->setMarginBottom(6);
        // Focusable so the list is navigable on a controller — Up/Down moves
        // between rows and the ScrollingFrame scrolls to keep focus visible.
        // Native cyan highlight; no action (rows are informational).
        row->setFocusable(true);
        row->setHighlightCornerRadius(9);

        auto* pi = new ProbeIcon();
        pi->setWidth(18); pi->setHeight(18); pi->setMarginRight(10);
        ui->icons.push_back(pi);
        row->addView(pi);

        auto* url = new brls::Label();
        url->setText(ellipsize(server.connections[i].uri, narrow ? 30 : 44));
        url->setFontSize(11.5f); url->setTextColor(kMuted);
        url->setSingleLine(true); url->setGrow(1.0f); url->setShrink(1.0f);
        row->addView(url);

        auto* st = new brls::Label();
        st->setText("queued"); st->setFontSize(11); st->setTextColor(kDim);
        st->setMarginLeft(8);
        ui->statuses.push_back(st);
        row->addView(st);

        probes->addView(row);
    }

    // A server can advertise many connection URIs (Plex commonly exposes
    // 10+ across local / plex.direct / relay), which would push the dialog
    // off-screen. Wrap the list in a vertical ScrollingFrame capped to a
    // fraction of the viewport: short lists fit exactly, long ones scroll.
    // The rows are focusable, so Up/Down navigates the list and the frame
    // scrolls to keep the focused row on screen.
    const float rowStride  = 40.0f;
    const float maxVisible = std::max(3.0f,
        std::floor(brls::Application::contentHeight * 0.42f / rowStride));
    const float probesH    = std::min(static_cast<float>(totalConnections), maxVisible) * rowStride;

    auto* scroll = new brls::ScrollingFrame();
    scroll->setWidth(innerW);
    scroll->setHeight(probesH);
    scroll->setMarginTop(18);
    scroll->setContentView(probes);
    card->addView(scroll);

    // Cancel — focusable Box so it gets the native cyan ring.
    auto* cancel = new brls::Box();
    cancel->setAxis(brls::Axis::ROW);
    cancel->setJustifyContent(brls::JustifyContent::CENTER);
    cancel->setAlignItems(brls::AlignItems::CENTER);
    cancel->setHeight(42);
    cancel->setPaddingLeft(26); cancel->setPaddingRight(26);
    cancel->setCornerRadius(10);
    cancel->setBackgroundColor(kRaised);
    cancel->setBorderColor(kLine); cancel->setBorderThickness(1);
    cancel->setMarginTop(22);
    cancel->setFocusable(true);
    cancel->setHighlightCornerRadius(10);
    auto* cancelL = new brls::Label();
    cancelL->setText("Cancel"); cancelL->setFontSize(14); cancelL->setTextColor(kWhite);
    cancel->addView(cancelL);

    auto cancelHandler = [this, ui, cancelled]() {
        if (!ui->alive) return;   // already settled by success / a prior cancel
        // Flip the atomics now so in-flight workers stop touching the rows,
        // but defer the actual teardown: we're running inside the Cancel
        // view's own click / B action, and clearViews()/popActivity() free
        // it immediately — swap on the next sync pass instead.
        ui->alive  = false;
        *cancelled = true;
        const bool toList = m_returnToList;
        brls::sync([this, toList]() {
            if (toList) showServerSelectionContent();
            else        dismissDialog();
        });
    };
    cancel->registerClickAction([cancelHandler](brls::View*) { cancelHandler(); return true; });
    cancel->addGestureRecognizer(new brls::TapGestureRecognizer(cancel));
    card->addView(cancel);
    card->registerAction("Cancel", brls::ControllerButton::BUTTON_B,
        [cancelHandler](brls::View*) { cancelHandler(); return true; });

    m_overlayMode = 2;
    presentDialogCard(card, cancel);

    // Push one probe row's active/terminal state from a worker thread.
    auto setProbe = [ui](size_t i, ProbeIcon::State s, std::string text, NVGcolor color) {
        brls::sync([ui, i, s, text, color]() {
            if (!ui->alive) return;
            if (i < ui->icons.size()    && ui->icons[i])    ui->icons[i]->setState(s);
            if (i < ui->statuses.size() && ui->statuses[i]) {
                ui->statuses[i]->setText(text);
                ui->statuses[i]->setTextColor(color);
            }
        });
    };

    // ── Parallel probe (semantics unchanged) wired to the live rows ──
    asyncRun([this, server, ui, cancelled, totalConnections, setProbe]() {
        PlexClient& client = PlexClient::getInstance();

        if (totalConnections == 0) {
            brls::sync([this, ui]() {
                if (!ui->alive) return;
                if (ui->counter) ui->counter->setText("No valid server connections");
                brls::delay(1600, [this, ui]() {
                    if (!ui->alive) return;
                    ui->alive = false;
                    if (m_returnToList) showServerSelectionContent();
                    else                dismissDialog();
                });
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
                    if (!found.load())
                        setProbe(i, ProbeIcon::State::Fail, "skipped", kDim);
                    completed.fetch_add(1);
                    return;
                }

                setProbe(i, ProbeIcon::State::Busy, "connecting", kGold);

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

                bool won = false;
                if (resp.statusCode == 200 && !found.exchange(true)) {
                    {
                        std::lock_guard<std::mutex> lk(winnerMutex);
                        winnerUri = conn.uri;
                    }
                    won = true;
                    winnerCv.notify_one();
                }

                // Reflect the real per-URI result on its row.
                if (won) {
                    setProbe(i, ProbeIcon::State::Win, "200 OK", kGreen);
                } else if (resp.statusCode == 200) {
                    setProbe(i, ProbeIcon::State::Fail, "slower", kDim);
                } else if (resp.statusCode > 0) {
                    setProbe(i, ProbeIcon::State::Fail, "HTTP " + std::to_string(resp.statusCode), kRed);
                } else {
                    setProbe(i, ProbeIcon::State::Fail, "timeout", kRed);
                }

                int doneNow = completed.fetch_add(1) + 1;
                brls::sync([ui, doneNow, totalConnections]() {
                    if (!ui->alive) return;
                    if (ui->bar) ui->bar->setFraction((float)doneNow / (float)totalConnections);
                    if (ui->counter)
                        ui->counter->setText(std::to_string(doneNow) + " of " +
                                             std::to_string(totalConnections) + " checked");
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
                    if (!ui->alive) return;
                    ui->alive = false;   // settle: Cancel becomes inert
                    if (ui->bar) ui->bar->setFraction(1.0f);
                    Application::getInstance().saveSettings();
                    if (statusLabel) statusLabel->setText("Connected to " + server.name);
                    brls::delay(500, [this]() {
                        dismissDialog();
                        Application::getInstance().pushMainActivity();
                        Application::getInstance().showHomeUserPicker(nullptr);
                    });
                });
                return;
            }
        }

        // No winner (or the winner's full connect failed): surface it, then
        // fall back to the list (or close) so the user can retry.
        brls::sync([this, ui, server, totalConnections]() {
            if (!ui->alive) return;
            if (ui->bar) ui->bar->setFraction(1.0f);
            if (ui->counter) ui->counter->setText("Couldn't reach " + server.name);
            if (statusLabel) statusLabel->setText("Failed to connect to " + server.name);
            brls::Logger::error("All {} connections failed for {}", totalConnections, server.name);
            brls::delay(2200, [this, ui]() {
                if (!ui->alive) return;
                ui->alive = false;
                if (m_returnToList) showServerSelectionContent();
                else                dismissDialog();
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
                    // Only one server, connect directly. No list to fall
                    // back to, so the connecting state's Cancel closes the
                    // modal rather than returning to a picker.
                    m_returnToList = false;
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
                    // Only one server, connect directly. No list to fall
                    // back to, so the connecting state's Cancel closes the
                    // modal rather than returning to a picker.
                    m_returnToList = false;
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
