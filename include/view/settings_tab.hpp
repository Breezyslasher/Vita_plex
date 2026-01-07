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
    void createAccountSection();
    void createUISection();
    void createLayoutSection();
    void createPlaybackSection();
    void createTranscodeSection();
    void createAboutSection();

    void onLogout();
    void onThemeChanged(int index);
    void onQualityChanged(int index);
    void onSubtitleSizeChanged(int index);
    void onSeekIntervalChanged(int index);
    void onManageHiddenLibraries();

    brls::ScrollingFrame* m_scrollView = nullptr;
    brls::Box* m_contentBox = nullptr;

    // Account section
    brls::Label* m_userLabel = nullptr;
    brls::Label* m_serverLabel = nullptr;

    // UI section
    brls::SelectorCell* m_themeSelector = nullptr;
    brls::BooleanCell* m_clockToggle = nullptr;
    brls::BooleanCell* m_animationsToggle = nullptr;
    brls::BooleanCell* m_debugLogToggle = nullptr;

    // Layout section
    brls::BooleanCell* m_sidebarLibrariesToggle = nullptr;
    brls::BooleanCell* m_collapseSidebarToggle = nullptr;
    brls::DetailCell* m_hiddenLibrariesCell = nullptr;

    // Playback section
    brls::BooleanCell* m_autoPlayToggle = nullptr;
    brls::BooleanCell* m_resumeToggle = nullptr;
    brls::BooleanCell* m_subtitlesToggle = nullptr;
    brls::SelectorCell* m_subtitleSizeSelector = nullptr;
    brls::SelectorCell* m_seekIntervalSelector = nullptr;

    // Transcode section
    brls::SelectorCell* m_qualitySelector = nullptr;
    brls::BooleanCell* m_forceTranscodeToggle = nullptr;
    brls::BooleanCell* m_burnSubtitlesToggle = nullptr;
    brls::BooleanCell* m_directPlayToggle = nullptr;
};

} // namespace vitaplex
