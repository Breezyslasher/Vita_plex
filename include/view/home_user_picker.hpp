/**
 * VitaPlex — Plex Home user picker UI (presentation only).
 *
 * Two screens, rebuilt with borealis trees + the all-Plex palette:
 *   1. A centered-focus profile row — the focused profile enlarges and goes
 *      fully opaque, neighbours shrink and dim. Cyan (borealis-native) focus
 *      ring on the avatar; gold marks the admin / active state.
 *   2. A PIN-entry overlay (dimmed picker behind) with four dots and an
 *      on-screen keypad.
 *
 * No brand/logo and no on-screen controller-hint bar on either screen.
 *
 * This file is presentation only: it takes the already-resolved user list and
 * a `trySwitch(user, pin) -> bool` callback (the verbatim switchHomeUser /
 * token-store logic lives in Application) plus an onComplete to run after the
 * picker resolves. It never talks to PlexClient directly.
 */

#pragma once

#include <borealis.hpp>
#include <functional>
#include <memory>
#include <atomic>
#include <string>
#include <vector>
#include <cctype>
#include <cmath>

#include "app/plex_client.hpp"   // HomeUser
#include "app/plex_palette.hpp"
#include "utils/image_loader.hpp"
#include "platform/platform.hpp"

namespace vitaplex {
namespace homepicker {

namespace pal = vitaplex::palette;

inline NVGcolor withA(NVGcolor c, float a) { c.a = a; return c; }

// ── helpers ─────────────────────────────────────────────────────────
inline std::string initialsOf(const std::string& name) {
    std::string s;
    bool boundary = true;
    for (char c : name) {
        if (c == ' ' || c == '-' || c == '_') { boundary = true; continue; }
        if (boundary && s.size() < 2) s += static_cast<char>(::toupper((unsigned char)c));
        boundary = false;
    }
    if (s.empty() && !name.empty()) s = std::string(1, static_cast<char>(::toupper((unsigned char)name[0])));
    if (s.empty()) s = "?";
    return s;
}

inline NVGcolor avatarBg(const std::string& name) {
    uint32_t h = 2166136261u;
    for (char c : name) { h ^= (unsigned char)c; h *= 16777619u; }
    static const NVGcolor wheel[] = {
        nvgRGB(0x4f, 0x6a, 0xd8), nvgRGB(0x8a, 0x5a, 0xd6), nvgRGB(0xcf, 0x5a, 0x8a),
        nvgRGB(0xcf, 0x84, 0x4f), nvgRGB(0x3e, 0xa0, 0x8a), nvgRGB(0x5a, 0x9a, 0xd6),
        nvgRGB(0xa0, 0x6a, 0x3e), nvgRGB(0x6a, 0x9a, 0x4f),
    };
    return wheel[h % (sizeof(wheel) / sizeof(wheel[0]))];
}

inline std::string subLabelFor(const HomeUser& u) {
    if (u.admin)  return "Owner";
    if (u.hasPin) return "PIN protected";
    return "";
}

// Small filled crown (drawn inside the gold admin badge).
inline void drawCrown(NVGcontext* vg, float x, float y, float s, NVGcolor color) {
    nvgBeginPath(vg);
    nvgMoveTo(vg, x,            y + s);
    nvgLineTo(vg, x,            y + s * 0.34f);
    nvgLineTo(vg, x + s * 0.27f, y + s * 0.62f);
    nvgLineTo(vg, x + s * 0.5f,  y + s * 0.12f);
    nvgLineTo(vg, x + s * 0.73f, y + s * 0.62f);
    nvgLineTo(vg, x + s,        y + s * 0.34f);
    nvgLineTo(vg, x + s,        y + s);
    nvgClosePath(vg);
    nvgFillColor(vg, color);
    nvgFill(vg);
}

// Small padlock (drawn inside the protected badge).
inline void drawLock(NVGcontext* vg, float x, float y, float s, NVGcolor color) {
    const float bw = s * 0.74f, bh = s * 0.52f;
    const float bx = x + (s - bw) * 0.5f, by = y + s - bh;
    nvgBeginPath(vg); nvgRoundedRect(vg, bx, by, bw, bh, s * 0.1f);
    nvgFillColor(vg, color); nvgFill(vg);
    nvgBeginPath(vg);
    nvgArc(vg, x + s * 0.5f, by, s * 0.23f, NVG_PI, 0.0f, NVG_CW);
    nvgStrokeColor(vg, color); nvgStrokeWidth(vg, s * 0.13f); nvgStroke(vg);
}

// Backspace (delete-into-box) glyph.
inline void drawBackspace(NVGcontext* vg, float x, float y, float s, NVGcolor color) {
    nvgStrokeColor(vg, color); nvgStrokeWidth(vg, s * 0.085f);
    nvgLineCap(vg, NVG_ROUND); nvgLineJoin(vg, NVG_ROUND);
    nvgBeginPath(vg);
    nvgMoveTo(vg, x + s * 0.36f, y + s * 0.2f);
    nvgLineTo(vg, x + s * 0.92f, y + s * 0.2f);
    nvgLineTo(vg, x + s * 0.92f, y + s * 0.8f);
    nvgLineTo(vg, x + s * 0.36f, y + s * 0.8f);
    nvgLineTo(vg, x + s * 0.08f, y + s * 0.5f);
    nvgClosePath(vg);
    nvgStroke(vg);
    nvgBeginPath(vg);
    nvgMoveTo(vg, x + s * 0.46f, y + s * 0.38f); nvgLineTo(vg, x + s * 0.74f, y + s * 0.62f);
    nvgMoveTo(vg, x + s * 0.74f, y + s * 0.38f); nvgLineTo(vg, x + s * 0.46f, y + s * 0.62f);
    nvgStroke(vg);
}

// ── Avatar ──────────────────────────────────────────────────────────
// Circular avatar: coloured-initials by default, thumb image on top once it
// loads. Optional admin crown / protected lock badges, drawn on the rim.
class Avatar : public brls::Box {
public:
    Avatar(const HomeUser& user, float radius, bool showBadges, bool focusable)
        : m_user(user), m_showBadges(showBadges) {
        m_alive = std::make_shared<std::atomic<bool>>(true);
        this->setJustifyContent(brls::JustifyContent::CENTER);
        this->setAlignItems(brls::AlignItems::CENTER);
        this->setBackgroundColor(avatarBg(user.title));
        if (focusable) this->setFocusable(true);

        m_initials = new brls::Label();
        m_initials->setText(initialsOf(user.title));
        m_initials->setTextColor(pal::text);
        this->addView(m_initials);

        m_image = new brls::Image();
        m_image->setScalingType(brls::ImageScalingType::FILL);
        m_image->setPositionType(brls::PositionType::ABSOLUTE);
        m_image->setPositionTop(0);
        m_image->setPositionLeft(0);
        m_image->setVisibility(brls::Visibility::GONE);
        this->addView(m_image);

        setRadius(radius);

        if (!user.thumb.empty()) {
            Avatar* self = this;
            auto alive = m_alive;
            ImageLoader::loadAsync(user.thumb,
                [self, alive](brls::Image* im) {
                    if (!alive || !*alive) return;
                    im->setVisibility(brls::Visibility::VISIBLE);
                    if (self->m_initials) self->m_initials->setVisibility(brls::Visibility::GONE);
                },
                m_image, m_alive);
        }
    }
    ~Avatar() override { if (m_alive) *m_alive = false; }

    void setRadius(float radius) {
        m_radius = radius;
        this->setWidth(radius * 2.0f);
        this->setHeight(radius * 2.0f);
        this->setCornerRadius(radius);
        this->setHighlightCornerRadius(radius);   // circular cyan focus ring
        if (m_image) {
            m_image->setWidth(radius * 2.0f);
            m_image->setHeight(radius * 2.0f);
            m_image->setCornerRadius(radius);
        }
        if (m_initials) m_initials->setFontSize(radius * 0.72f);
    }

    void setOnFocusChange(std::function<void(bool)> cb) { m_onFocusChange = std::move(cb); }
    void onFocusGained() override { brls::Box::onFocusGained(); if (m_onFocusChange) m_onFocusChange(true); }
    void onFocusLost()   override { brls::Box::onFocusLost();   if (m_onFocusChange) m_onFocusChange(false); }

    void draw(NVGcontext* vg, float x, float y, float w, float h,
              brls::Style st, brls::FrameContext* ctx) override {
        brls::Box::draw(vg, x, y, w, h, st, ctx);   // colour circle + initials + thumb
        if (!m_showBadges) return;
        const float r  = std::min(w, h) * 0.5f;
        const float cx = x + w * 0.5f, cy = y + h * 0.5f;
        const float a  = this->getAlpha();
        if (m_user.admin) {
            const float bx = cx + r * 0.70f, by = cy - r * 0.70f, br = r * 0.30f;
            nvgBeginPath(vg); nvgCircle(vg, bx, by, br + 2.0f); nvgFillColor(vg, withA(pal::bg, a)); nvgFill(vg);
            nvgBeginPath(vg); nvgCircle(vg, bx, by, br);        nvgFillColor(vg, withA(pal::gold, a)); nvgFill(vg);
            drawCrown(vg, bx - br * 0.58f, by - br * 0.5f, br * 1.16f, withA(pal::goldInk, a));
        }
        if (m_user.hasPin) {
            const float bx = cx + r * 0.70f, by = cy + r * 0.70f, br = r * 0.28f;
            nvgBeginPath(vg); nvgCircle(vg, bx, by, br + 2.0f); nvgFillColor(vg, withA(pal::bg, a)); nvgFill(vg);
            nvgBeginPath(vg); nvgCircle(vg, bx, by, br);        nvgFillColor(vg, withA(pal::panel, a)); nvgFill(vg);
            drawLock(vg, bx - br * 0.55f, by - br * 0.55f, br * 1.1f, withA(pal::gold, a));
        }
    }

private:
    HomeUser m_user;
    bool  m_showBadges;
    float m_radius = 52.0f;
    brls::Label* m_initials = nullptr;
    brls::Image* m_image    = nullptr;
    std::shared_ptr<std::atomic<bool>> m_alive;
    std::function<void(bool)> m_onFocusChange;
};

// ── ProfileTile ─────────────────────────────────────────────────────
// Fixed-width slot (stable row layout) with a focusable avatar that grows
// 104 -> 168 on focus, plus name + role sub-label.
class ProfileTile : public brls::Box {
public:
    ProfileTile(const HomeUser& user, std::function<void()> onSelect)
        : m_user(user) {
        this->setAxis(brls::Axis::COLUMN);
        this->setAlignItems(brls::AlignItems::CENTER);
        this->setJustifyContent(brls::JustifyContent::FLEX_START);
        this->setWidth(180.0f);
        this->setMarginLeft(17.0f);
        this->setMarginRight(17.0f);
        this->setAlpha(0.62f);

        auto* slot = new brls::Box();
        slot->setWidth(176.0f);
        slot->setHeight(176.0f);
        slot->setJustifyContent(brls::JustifyContent::CENTER);
        slot->setAlignItems(brls::AlignItems::CENTER);
        this->addView(slot);

        m_avatar = new Avatar(user, 52.0f, true, true);
        m_avatar->registerClickAction([onSelect](brls::View*) { if (onSelect) onSelect(); return true; });
        m_avatar->addGestureRecognizer(new brls::TapGestureRecognizer(m_avatar));
        m_avatar->setOnFocusChange([this](bool f) { applyFocus(f); });
        slot->addView(m_avatar);

        m_name = new brls::Label();
        m_name->setText(user.title);
        m_name->setFontSize(15.0f);
        m_name->setTextColor(pal::muted);
        m_name->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        m_name->setSingleLine(true);
        m_name->setMarginTop(12.0f);
        this->addView(m_name);

        m_sub = new brls::Label();
        m_sub->setText(subLabelFor(user));
        m_sub->setFontSize(13.0f);
        m_sub->setTextColor(pal::dim);
        m_sub->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        m_sub->setMarginTop(3.0f);
        m_sub->setVisibility(brls::Visibility::GONE);
        this->addView(m_sub);
    }

    Avatar* avatar() { return m_avatar; }

    void applyFocus(bool f) {
        m_avatar->setRadius(f ? 84.0f : 52.0f);
        m_avatar->invalidate();
        this->setAlpha(f ? 1.0f : 0.62f);
        m_name->setFontSize(f ? 22.0f : 15.0f);
        m_name->setTextColor(f ? pal::text : pal::muted);
        const bool hasSub = !subLabelFor(m_user).empty();
        m_sub->setVisibility(f && hasSub ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
        this->invalidate();
    }

private:
    HomeUser m_user;
    Avatar* m_avatar = nullptr;
    brls::Label* m_name = nullptr;
    brls::Label* m_sub  = nullptr;
};

// ── PIN dots ────────────────────────────────────────────────────────
class PinDots : public brls::Box {
public:
    PinDots() {
        this->setHeight(24.0f);
        this->setWidth(4.0f * 22.0f + 3.0f * 18.0f);
    }
    void setFilled(int n) { m_filled = n; }
    void flashError() { m_errorUntilUs = brls::getCPUTimeUsec() + 650000; }

    void draw(NVGcontext* vg, float x, float y, float w, float h,
              brls::Style st, brls::FrameContext* ctx) override {
        brls::Box::draw(vg, x, y, w, h, st, ctx);
        const double now = static_cast<double>(brls::getCPUTimeUsec());
        const bool err = now < m_errorUntilUs;
        const float r = 11.0f, gap = 18.0f;
        const float cy = y + h * 0.5f;
        for (int i = 0; i < 4; i++) {
            const float cx = x + r + i * (2.0f * r + gap);
            if (i < m_filled && !err) {
                nvgBeginPath(vg); nvgCircle(vg, cx, cy, r + 3.0f);
                nvgFillColor(vg, withA(pal::gold, 0.28f)); nvgFill(vg);        // glow
                nvgBeginPath(vg); nvgCircle(vg, cx, cy, r);
                nvgFillColor(vg, pal::gold); nvgFill(vg);
                nvgBeginPath(vg); nvgCircle(vg, cx, cy, r);
                nvgStrokeColor(vg, pal::gold); nvgStrokeWidth(vg, 2.0f); nvgStroke(vg);
            } else {
                nvgBeginPath(vg); nvgCircle(vg, cx, cy, r);
                nvgStrokeColor(vg, err ? pal::live : pal::surface3);
                nvgStrokeWidth(vg, 2.0f); nvgStroke(vg);
            }
        }
    }

private:
    int m_filled = 0;
    double m_errorUntilUs = 0.0;
};

// ── Keypad key ──────────────────────────────────────────────────────
class KeyButton : public brls::Box {
public:
    KeyButton(const std::string& digit, std::function<void()> onPress, bool backspace, float kw, float kh)
        : m_backspace(backspace) {
        this->setWidth(kw);
        this->setHeight(kh);
        this->setCornerRadius(16.0f);
        this->setJustifyContent(brls::JustifyContent::CENTER);
        this->setAlignItems(brls::AlignItems::CENTER);
        this->setBackgroundColor(pal::surface);
        this->setBorderColor(pal::line);
        this->setBorderThickness(1.0f);
        this->setFocusable(true);
        this->setHighlightCornerRadius(16.0f);
        this->registerClickAction([onPress](brls::View*) { if (onPress) onPress(); return true; });
        this->addGestureRecognizer(new brls::TapGestureRecognizer(this));
        if (!backspace) {
            auto* l = new brls::Label();
            l->setText(digit);
            l->setFontSize(28.0f);
            l->setTextColor(pal::text);
            this->addView(l);
        }
    }
    void draw(NVGcontext* vg, float x, float y, float w, float h,
              brls::Style st, brls::FrameContext* ctx) override {
        brls::Box::draw(vg, x, y, w, h, st, ctx);
        if (m_backspace) {
            const float s = std::min(w, h) * 0.42f;
            drawBackspace(vg, x + (w - s) * 0.5f, y + (h - s) * 0.5f, s, pal::muted);
        }
    }
private:
    bool m_backspace;
};

// ── PIN overlay ─────────────────────────────────────────────────────
// Translucent so the dimmed picker shows behind the scrim.
class PinOverlay : public brls::Box {
public:
    PinOverlay(const HomeUser& user,
               std::function<bool(const std::string&)> trySwitchPin,
               std::function<void()> onSuccess)
        : m_trySwitch(std::move(trySwitchPin)), m_onSuccess(std::move(onSuccess)) {
        const bool narrow = platform::isPortrait();
        const float kw = narrow ? 76.0f : 84.0f;
        const float kh = narrow ? 64.0f : 72.0f;

        this->setWidthPercentage(100.0f);
        this->setHeightPercentage(100.0f);
        this->setJustifyContent(brls::JustifyContent::CENTER);
        this->setAlignItems(brls::AlignItems::CENTER);
        this->setBackgroundColor(nvgRGBA(12, 10, 16, 189));   // scrim ~0.74

        auto* card = new brls::Box();
        card->setAxis(brls::Axis::COLUMN);
        card->setAlignItems(brls::AlignItems::CENTER);
        card->setBackgroundColor(pal::panel);
        card->setCornerRadius(20.0f);
        card->setBorderColor(pal::line);
        card->setBorderThickness(1.0f);
        card->setShadowType(brls::ShadowType::GENERIC);
        card->setPadding(28.0f, 34.0f, 30.0f, 34.0f);

        auto* av = new Avatar(user, 46.0f, false, false);
        av->setMarginBottom(16.0f);
        card->addView(av);

        auto* heading = new brls::Label();
        heading->setText("Enter " + user.title + "'s PIN");
        heading->setFontSize(23.0f);
        heading->setTextColor(pal::text);
        heading->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        card->addView(heading);

        auto* sub = new brls::Label();
        sub->setText("4-digit profile PIN");
        sub->setFontSize(13.5f);
        sub->setTextColor(pal::dim);
        sub->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        sub->setMarginTop(4.0f);
        sub->setMarginBottom(20.0f);
        card->addView(sub);

        m_dots = new PinDots();
        m_dots->setMarginBottom(24.0f);
        card->addView(m_dots);

        // Keypad: 1-9, [blank, 0, backspace]
        auto* grid = new brls::Box();
        grid->setAxis(brls::Axis::COLUMN);
        grid->setAlignItems(brls::AlignItems::CENTER);
        const char* rows[4][3] = {
            { "1", "2", "3" }, { "4", "5", "6" }, { "7", "8", "9" }, { "", "0", "\b" }
        };
        for (int r = 0; r < 4; r++) {
            auto* rowBox = new brls::Box();
            rowBox->setAxis(brls::Axis::ROW);
            rowBox->setMarginBottom(r < 3 ? 10.0f : 0.0f);
            for (int c = 0; c < 3; c++) {
                const std::string key = rows[r][c];
                brls::Box* cell;
                if (key.empty()) {
                    cell = new brls::Box();           // spacer
                    cell->setWidth(kw);
                    cell->setHeight(kh);
                } else if (key == "\b") {
                    cell = new KeyButton("", [this]() { backspace(); }, true, kw, kh);
                } else {
                    const char d = key[0];
                    cell = new KeyButton(key, [this, d]() { addDigit(d); }, false, kw, kh);
                    if (d == '5') m_defaultKey = cell;
                }
                cell->setMarginLeft(c == 0 ? 0.0f : 5.0f);
                cell->setMarginRight(c == 2 ? 0.0f : 5.0f);
                rowBox->addView(cell);
            }
            grid->addView(rowBox);
        }
        card->addView(grid);
        this->addView(card);

        this->registerAction("Back", brls::ControllerButton::BUTTON_B,
            [](brls::View*) { brls::Application::popActivity(); return true; });
    }

    bool isTranslucent() override { return true; }
    brls::View* defaultKey() { return m_defaultKey; }

    void addDigit(char d) {
        if (m_pin.size() >= 4) return;
        m_pin.push_back(d);
        m_dots->setFilled(static_cast<int>(m_pin.size()));
        if (m_pin.size() == 4) submit();
    }
    void backspace() {
        if (m_pin.empty()) return;
        m_pin.pop_back();
        m_dots->setFilled(static_cast<int>(m_pin.size()));
    }

private:
    void submit() {
        if (m_trySwitch && m_trySwitch(m_pin)) {
            if (m_onSuccess) m_onSuccess();
        } else {
            m_dots->flashError();
            m_pin.clear();
            m_dots->setFilled(0);
        }
    }

    std::string m_pin;
    PinDots* m_dots = nullptr;
    brls::View* m_defaultKey = nullptr;
    std::function<bool(const std::string&)> m_trySwitch;
    std::function<void()> m_onSuccess;
};

// ── Entry point ─────────────────────────────────────────────────────
// Builds + pushes the picker. `trySwitch(user, pin)` performs the real
// switchHomeUser/token-store (returns success); `onComplete` runs after the
// picker resolves (switch or cancel).
inline void show(const std::vector<HomeUser>& users, int selectedIndex,
                 std::function<bool(const HomeUser&, const std::string&)> trySwitch,
                 std::function<void()> onComplete) {
    auto* root = new brls::Box();
    root->setAxis(brls::Axis::COLUMN);
    root->setJustifyContent(brls::JustifyContent::CENTER);
    root->setAlignItems(brls::AlignItems::CENTER);
    root->setWidthPercentage(100.0f);
    root->setHeightPercentage(100.0f);
    root->setBackgroundColor(pal::bg);

    auto* title = new brls::Label();
    title->setText("Choose your profile");
    title->setFontSize(30.0f);
    title->setTextColor(pal::text);
    title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    title->setMarginBottom(42.0f);
    root->addView(title);

    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setJustifyContent(brls::JustifyContent::CENTER);
    row->setAlignItems(brls::AlignItems::CENTER);

    brls::View* defaultFocus = nullptr;
    for (size_t i = 0; i < users.size(); i++) {
        HomeUser user = users[i];
        auto* tile = new ProfileTile(user, [user, trySwitch, onComplete]() {
            if (user.hasPin) {
                auto* overlay = new PinOverlay(
                    user,
                    [trySwitch, user](const std::string& pin) { return trySwitch(user, pin); },
                    [onComplete]() {
                        // pop the PIN overlay, then the picker, then proceed
                        brls::Application::popActivity(brls::TransitionAnimation::NONE, [onComplete]() {
                            brls::Application::popActivity(brls::TransitionAnimation::NONE, [onComplete]() {
                                if (onComplete) onComplete();
                            });
                        });
                    });
                brls::Application::pushActivity(new brls::Activity(overlay));
                if (overlay->defaultKey()) brls::Application::giveFocus(overlay->defaultKey());
            } else {
                if (trySwitch(user, "")) {
                    brls::Application::popActivity(brls::TransitionAnimation::NONE, [onComplete]() {
                        if (onComplete) onComplete();
                    });
                } else {
                    auto* d = new brls::Dialog("Failed to switch to " + user.title);
                    d->addButton("OK", []() {});
                    d->open();
                }
            }
        });
        row->addView(tile);

        if (user.admin) defaultFocus = tile->avatar();
        if (!defaultFocus && static_cast<int>(i) == selectedIndex) defaultFocus = tile->avatar();
    }
    if (!defaultFocus && !row->getChildren().empty()) {
        // first tile -> its avatar (tile is a column: slot -> avatar)
        if (auto* firstTile = dynamic_cast<ProfileTile*>(row->getChildren()[0]))
            defaultFocus = firstTile->avatar();
    }
    root->addView(row);

    auto* helper = new brls::Label();
    helper->setText("←  browse profiles  →");
    helper->setFontSize(13.0f);
    helper->setTextColor(pal::dim);
    helper->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    helper->setMarginTop(38.0f);
    root->addView(helper);

    root->registerAction("Back", brls::ControllerButton::BUTTON_B,
        [onComplete](brls::View*) {
            brls::Application::popActivity(brls::TransitionAnimation::FADE, [onComplete]() {
                if (onComplete) onComplete();
            });
            return true;
        });

    brls::Application::pushActivity(new brls::Activity(root));
    if (defaultFocus) brls::Application::giveFocus(defaultFocus);
}

}  // namespace homepicker
}  // namespace vitaplex
