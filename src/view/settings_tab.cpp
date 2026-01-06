/**
 * VitaPlex - Settings Tab implementation
 */

#include "view/settings_tab.hpp"
#include "app/application.hpp"
#include "app/plex_client.hpp"

namespace vitaplex {

SettingsTab::SettingsTab() {
    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setPadding(20);
    this->setGrow(1.0f);

    // Title
    m_titleLabel = new brls::Label();
    m_titleLabel->setText("Settings");
    m_titleLabel->setFontSize(28);
    m_titleLabel->setMarginBottom(30);
    this->addView(m_titleLabel);

    // User info section
    auto* userSection = new brls::Label();
    userSection->setText("Account");
    userSection->setFontSize(22);
    userSection->setMarginBottom(10);
    this->addView(userSection);

    Application& app = Application::getInstance();

    m_userLabel = new brls::Label();
    m_userLabel->setText("User: " + app.getUsername());
    m_userLabel->setFontSize(18);
    m_userLabel->setMarginBottom(10);
    this->addView(m_userLabel);

    m_serverLabel = new brls::Label();
    m_serverLabel->setText("Server: " + app.getServerUrl());
    m_serverLabel->setFontSize(18);
    m_serverLabel->setMarginBottom(30);
    this->addView(m_serverLabel);

    // About section
    auto* aboutSection = new brls::Label();
    aboutSection->setText("About");
    aboutSection->setFontSize(22);
    aboutSection->setMarginBottom(10);
    this->addView(aboutSection);

    m_versionLabel = new brls::Label();
    m_versionLabel->setText("VitaPlex v" VITA_PLEX_VERSION);
    m_versionLabel->setFontSize(18);
    m_versionLabel->setMarginBottom(10);
    this->addView(m_versionLabel);

    auto* creditLabel = new brls::Label();
    creditLabel->setText("Plex Client for PlayStation Vita");
    creditLabel->setFontSize(16);
    creditLabel->setMarginBottom(10);
    this->addView(creditLabel);

    auto* borealisLabel = new brls::Label();
    borealisLabel->setText("UI powered by Borealis");
    borealisLabel->setFontSize(16);
    borealisLabel->setMarginBottom(30);
    this->addView(borealisLabel);

    // Logout button
    m_logoutButton = new brls::Button();
    m_logoutButton->setText("Logout");
    m_logoutButton->setWidth(200);
    m_logoutButton->registerClickAction([this](brls::View* view) {
        onLogout();
        return true;
    });
    this->addView(m_logoutButton);
}

void SettingsTab::onLogout() {
    brls::Dialog* dialog = new brls::Dialog("Are you sure you want to logout?");

    dialog->addButton("Cancel", [dialog]() {
        dialog->close();
    });

    dialog->addButton("Logout", [dialog, this]() {
        dialog->close();

        // Clear credentials
        PlexClient::getInstance().logout();
        Application::getInstance().setAuthToken("");
        Application::getInstance().setServerUrl("");
        Application::getInstance().setUsername("");
        Application::getInstance().saveSettings();

        // Go back to login
        Application::getInstance().pushLoginActivity();
    });

    dialog->open();
}

} // namespace vitaplex
