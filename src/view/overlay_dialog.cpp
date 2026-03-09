/**
 * VitaPlex - Semi-Transparent Overlay Dialog implementation
 */

#include "view/overlay_dialog.hpp"

namespace vitaplex {

OverlayDialog::OverlayDialog(const std::string& title, const std::string& message, uint8_t backdropAlpha)
    : m_backdropAlpha(backdropAlpha) {
    // Full-screen backdrop with configurable transparency
    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::CENTER);
    this->setAlignItems(brls::AlignItems::CENTER);
    this->setBackgroundColor(nvgRGBA(0, 0, 0, backdropAlpha));
    this->setWidth(brls::View::AUTO);
    this->setHeight(brls::View::AUTO);

    // Content container
    m_container = new brls::Box();
    m_container->setAxis(brls::Axis::COLUMN);
    m_container->setAlignItems(brls::AlignItems::STRETCH);
    m_container->setBackgroundColor(nvgRGBA(40, 40, 40, m_contentAlpha));
    m_container->setCornerRadius(12);
    m_container->setPadding(25);
    m_container->setWidth(420);

    // Title
    m_titleLabel = new brls::Label();
    m_titleLabel->setText(title);
    m_titleLabel->setFontSize(22);
    m_titleLabel->setMarginBottom(12);
    m_container->addView(m_titleLabel);

    // Content area (holds either message or custom content)
    m_contentArea = new brls::Box();
    m_contentArea->setAxis(brls::Axis::COLUMN);
    m_contentArea->setMarginBottom(20);
    m_container->addView(m_contentArea);

    // Default message label
    m_messageLabel = new brls::Label();
    m_messageLabel->setText(message);
    m_messageLabel->setFontSize(16);
    m_contentArea->addView(m_messageLabel);

    // Button row
    m_buttonBox = new brls::Box();
    m_buttonBox->setAxis(brls::Axis::ROW);
    m_buttonBox->setJustifyContent(brls::JustifyContent::FLEX_END);
    m_buttonBox->setGrow(0);
    m_container->addView(m_buttonBox);

    this->addView(m_container);

    // Handle back button (Circle) to dismiss instead of triggering app exit
    this->registerAction("Back", brls::ControllerButton::BUTTON_B, [this](brls::View* view) {
        dismiss();
        return true;
    });
}

OverlayDialog::~OverlayDialog() {
    m_dismissed = true;
}

void OverlayDialog::setContentAlpha(uint8_t alpha) {
    m_contentAlpha = alpha;
    if (m_container) {
        m_container->setBackgroundColor(nvgRGBA(40, 40, 40, alpha));
    }
}

void OverlayDialog::addButton(const std::string& text, std::function<void()> callback) {
    auto* btn = new brls::Button();
    btn->setText(text);
    btn->setMarginLeft(10);
    btn->registerClickAction([callback](brls::View* view) {
        if (callback) callback();
        return true;
    });
    m_buttonBox->addView(btn);
}

void OverlayDialog::setCustomContent(brls::Box* content) {
    if (m_contentArea) {
        m_contentArea->clearViews();
        m_contentArea->addView(content);
    }
}

void OverlayDialog::show() {
    brls::Application::pushActivity(new brls::Activity(this));
}

void OverlayDialog::dismiss() {
    if (!m_dismissed) {
        m_dismissed = true;
        brls::Application::popActivity();
    }
}

} // namespace vitaplex
