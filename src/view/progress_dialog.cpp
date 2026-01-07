/**
 * VitaPlex - Progress Dialog implementation
 */

#include "view/progress_dialog.hpp"

namespace vitaplex {

ProgressDialog::ProgressDialog(const std::string& title) {
    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::CENTER);
    this->setAlignItems(brls::AlignItems::CENTER);
    this->setBackgroundColor(nvgRGBA(0, 0, 0, 200));
    this->setWidth(brls::View::AUTO);
    this->setHeight(brls::View::AUTO);

    // Container box
    auto* container = new brls::Box();
    container->setAxis(brls::Axis::COLUMN);
    container->setAlignItems(brls::AlignItems::CENTER);
    container->setBackgroundColor(nvgRGBA(40, 40, 40, 255));
    container->setCornerRadius(10);
    container->setPadding(30);
    container->setWidth(400);

    // Title
    m_titleLabel = new brls::Label();
    m_titleLabel->setText(title);
    m_titleLabel->setFontSize(22);
    m_titleLabel->setMarginBottom(20);
    container->addView(m_titleLabel);

    // Status
    m_statusLabel = new brls::Label();
    m_statusLabel->setText("Initializing...");
    m_statusLabel->setFontSize(16);
    m_statusLabel->setMarginBottom(10);
    container->addView(m_statusLabel);

    // Attempt label
    m_attemptLabel = new brls::Label();
    m_attemptLabel->setText("");
    m_attemptLabel->setFontSize(14);
    m_attemptLabel->setMarginBottom(15);
    m_attemptLabel->setVisibility(brls::Visibility::GONE);
    container->addView(m_attemptLabel);

    // Progress bar background
    auto* progressContainer = new brls::Box();
    progressContainer->setWidth(340);
    progressContainer->setHeight(20);
    progressContainer->setMarginBottom(15);

    m_progressBg = new brls::Rectangle();
    m_progressBg->setWidth(340);
    m_progressBg->setHeight(20);
    m_progressBg->setColor(nvgRGBA(60, 60, 60, 255));
    m_progressBg->setCornerRadius(5);
    progressContainer->addView(m_progressBg);

    // Progress bar foreground
    m_progressBar = new brls::Rectangle();
    m_progressBar->setWidth(0);
    m_progressBar->setHeight(20);
    m_progressBar->setColor(nvgRGBA(229, 160, 13, 255));  // Plex orange
    m_progressBar->setCornerRadius(5);
    m_progressBar->setPositionType(brls::PositionType::ABSOLUTE);
    m_progressBar->setPositionLeft(0);
    m_progressBar->setPositionTop(0);
    progressContainer->addView(m_progressBar);

    container->addView(progressContainer);

    // Progress percentage label
    m_progressLabel = new brls::Label();
    m_progressLabel->setText("0%");
    m_progressLabel->setFontSize(14);
    m_progressLabel->setMarginBottom(20);
    m_progressLabel->setVisibility(brls::Visibility::GONE);
    container->addView(m_progressLabel);

    // Cancel button
    m_cancelButton = new brls::Button();
    m_cancelButton->setText("Cancel");
    m_cancelButton->setWidth(150);
    m_cancelButton->registerClickAction([this](brls::View* view) {
        if (m_cancelCallback) {
            m_cancelCallback();
        }
        dismiss();
        return true;
    });
    container->addView(m_cancelButton);

    this->addView(container);
}

ProgressDialog::~ProgressDialog() {
    m_dismissed = true;
}

void ProgressDialog::setStatus(const std::string& status) {
    if (m_statusLabel && !m_dismissed) {
        m_statusLabel->setText(status);
    }
}

void ProgressDialog::setProgress(float progress) {
    if (m_dismissed) return;

    if (progress < 0) progress = 0;
    if (progress > 1) progress = 1;

    if (m_progressBar) {
        m_progressBar->setWidth(340 * progress);
    }

    if (m_progressLabel) {
        m_progressLabel->setVisibility(brls::Visibility::VISIBLE);
        int percent = static_cast<int>(progress * 100);
        m_progressLabel->setText(std::to_string(percent) + "%");
    }
}

void ProgressDialog::setAttempt(int current, int total) {
    if (m_attemptLabel && !m_dismissed) {
        m_attemptLabel->setVisibility(brls::Visibility::VISIBLE);
        m_attemptLabel->setText("Attempt " + std::to_string(current) + " of " + std::to_string(total));
    }
}

void ProgressDialog::show() {
    // Push as an overlay activity
    brls::Application::pushActivity(new brls::Activity(this));
}

void ProgressDialog::dismiss() {
    if (!m_dismissed) {
        m_dismissed = true;
        brls::Application::popActivity();
    }
}

void ProgressDialog::setCancelCallback(std::function<void()> callback) {
    m_cancelCallback = callback;
}

ProgressDialog* ProgressDialog::showConnecting(const std::string& serverName) {
    auto* dialog = new ProgressDialog("Connecting to Server");
    dialog->setStatus("Connecting to " + serverName + "...");
    dialog->show();
    return dialog;
}

ProgressDialog* ProgressDialog::showDownloading(const std::string& title) {
    auto* dialog = new ProgressDialog("Downloading");
    dialog->setStatus(title);
    dialog->setProgress(0);
    dialog->show();
    return dialog;
}

} // namespace vitaplex
