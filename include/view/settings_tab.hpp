/**
 * VitaPlex - Settings Tab
 * Application settings and user info
 */

#pragma once

#include <borealis.hpp>

namespace vitaplex {

class SettingsTab : public brls::Box {
public:
    SettingsTab();

private:
    void onLogout();

    brls::Label* m_titleLabel = nullptr;
    brls::Label* m_userLabel = nullptr;
    brls::Label* m_serverLabel = nullptr;
    brls::Label* m_versionLabel = nullptr;
    brls::Button* m_logoutButton = nullptr;
};

} // namespace vitaplex
