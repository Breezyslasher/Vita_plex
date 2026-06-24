/**
 * VitaPlex - Settings Tab
 * Application settings and user info — master/detail layout.
 *
 * The tab is split into two panes: a vertical "rail" on the left that
 * lists the section categories, and a "detail" pane on the right that
 * holds the cells of the currently-selected section. Every existing
 * cell, change handler, and persistence path is preserved; only the
 * parent layout changes. Per-section builder methods now return their
 * own brls::Box instead of appending into one big m_contentBox, and
 * the detail pane swaps which one is visible based on the rail
 * selection.
 */

#pragma once

#include <borealis.hpp>
#include <vector>

namespace vitaplex {

class SettingsTab : public brls::Box {
public:
    SettingsTab();
    // Section boxes that aren't currently attached to m_detailContent
    // have no parent, so brls::Box::~Box's "delete every child" sweep
    // won't reach them. The destructor walks m_sectionBoxes and frees
    // the detached ones explicitly.
    ~SettingsTab() override;

private:
    // Section IDs. Order matches kSections in the cpp and the order in
    // which the rail rows are added. Keep them dense and contiguous —
    // showSection() indexes m_sectionBoxes / m_railRows by these.
    //
    // SEC_ABOUT is intentionally last and has no section box — the
    // rail renders it as a static, non-focusable version readout
    // instead of a clickable category. SEC_COUNT therefore counts
    // every entry including About (used to size m_railRows /
    // m_sectionBoxes); m_sectionBoxes[SEC_ABOUT] stays null.
    enum SectionId : int {
        SEC_ACCOUNT = 0,
        SEC_INTERFACE,
        SEC_LAYOUT,
        SEC_CONTENT,
        SEC_PLAYBACK,
        SEC_TRANSCODING,
        SEC_NETWORK,
        SEC_DOWNLOADS,
        SEC_MUSIC,
        SEC_LIVETV,
        SEC_ABOUT,
        SEC_COUNT
    };

    // Per-section builders. Each one now returns a freshly-built
    // brls::Box (COLUMN of cells) instead of appending into a shared
    // container — the constructor stitches all of them into
    // m_sectionBoxes and hides every one except the active section.
    brls::Box* createAccountSection();
    brls::Box* createUISection();
    brls::Box* createLayoutSection();
    brls::Box* createContentDisplaySection();
    brls::Box* createPlaybackSection();
    brls::Box* createTranscodeSection();
    brls::Box* createNetworkSection();
    brls::Box* createDownloadsSection();
    brls::Box* createMusicSection();
    brls::Box* createLiveTVSection();

    // Master/detail plumbing — see settings_tab.cpp for the layout.
    brls::Box*           makeSectionBox();
    brls::Box*           makeRailRow(const std::string& iconPath,
                                     const std::string& title,
                                     int sectionId);
    // Static, non-focusable row used for the About footer — shows the
    // version next to an icon and is skipped by focus navigation so
    // the user can't accidentally land on it.
    brls::Box*           makeRailInfoRow(const std::string& iconPath,
                                         const std::string& title);
    void                 showSection(int sectionId);
    // Refresh the left-bar / background / text colour on every rail row
    // so the visually-selected one matches m_activeSection. Called by
    // showSection and by any handler that programmatically changes it.
    void                 paintRailRowSelection();

    void onLogout();
    void onNetworkTest();
    // Connect / disconnect the persistent SyncLounge session that an active
    // player follows. See SyncLoungeSession.
    void onSyncLoungeConnect();
    // Open the party-members dialog (who's in the room; host can transfer host).
    void onSyncLoungeMembers();
    void onTestLocalPlayback();
    void onThemeChanged(int index);
    void onQualityChanged(int index);
    void onSubtitleSizeChanged(int index);
    void onSeekIntervalChanged(int index);
    void onControlsAutoHideChanged(int index);
    void onConnectionTimeoutChanged(int index);
    void onManageHiddenLibraries();
    void onManageSidebarOrder();
    void onManageDefaultDvrLibrary();
    void onSwitchUser();

    // Layout panes
    brls::Box*           m_railContainer = nullptr;  // left column wrapper
    brls::ScrollingFrame* m_railScroll   = nullptr;
    brls::Box*           m_railBox       = nullptr;  // holds the rail rows
    brls::Box*           m_detailContainer = nullptr;
    brls::Box*           m_detailHeader  = nullptr;
    brls::Label*         m_detailTitle   = nullptr;
    brls::Label*         m_detailSubtitle = nullptr;
    brls::ScrollingFrame* m_detailScroll = nullptr;
    brls::Box*           m_detailContent = nullptr;  // holds the active section's Box

    std::vector<brls::Box*> m_railRows;       // one per section, indexed by SectionId
    std::vector<brls::Box*> m_sectionBoxes;   // one per section, indexed by SectionId
    // Which section box is currently parented to m_detailContent. brls
    // Box::removeView with free=false doesn't reset the view's parent
    // pointer, so getParent() can't be used to tell whether we already
    // attached a particular section. Track it explicitly here.
    brls::Box*              m_attachedSection = nullptr;
    int                     m_activeSection = SEC_ACCOUNT;

    // Account section
    brls::Label* m_userLabel = nullptr;
    brls::Label* m_serverLabel = nullptr;
    brls::BooleanCell* m_autoLoginToggle = nullptr;
    brls::DetailCell*  m_switchUserCell  = nullptr;

    // UI section
    brls::SelectorCell* m_themeSelector = nullptr;
    brls::BooleanCell* m_debugLogToggle = nullptr;

    // Layout section
    brls::BooleanCell* m_sidebarLibrariesToggle = nullptr;
    brls::BooleanCell* m_collapseSidebarToggle = nullptr;
    brls::DetailCell* m_hiddenLibrariesCell = nullptr;
    brls::DetailCell* m_sidebarOrderCell = nullptr;

    // Content display section
    brls::BooleanCell* m_collectionsToggle = nullptr;
    brls::BooleanCell* m_playlistsToggle = nullptr;
    brls::BooleanCell* m_genresToggle = nullptr;
    brls::BooleanCell* m_hideTitlesToggle = nullptr;
    brls::BooleanCell* m_skipSingleSeasonToggle = nullptr;

    // Playback section
    brls::BooleanCell* m_autoPlayToggle = nullptr;
    brls::BooleanCell* m_resumeToggle = nullptr;
    brls::BooleanCell* m_subtitlesToggle = nullptr;
    brls::SelectorCell* m_subtitleSizeSelector = nullptr;
    brls::SelectorCell* m_seekIntervalSelector = nullptr;
    brls::SelectorCell* m_controlsAutoHideSelector = nullptr;
    brls::BooleanCell* m_autoSkipIntroToggle = nullptr;
    brls::BooleanCell* m_autoSkipCreditsToggle = nullptr;

    // Transcode section
    brls::SelectorCell* m_qualitySelector = nullptr;
    brls::BooleanCell* m_forceTranscodeToggle = nullptr;
    brls::BooleanCell* m_directPlayToggle = nullptr;

    // Network section (split out of Transcoding so it gets its own rail
    // row; the cell + handler are unchanged).
    brls::SelectorCell* m_connectionTimeoutSelector = nullptr;
    // HTTP response cache controls live in Network too — keeps all the
    // server-traffic settings in one place.
    brls::SelectorCell* m_cacheLifetimeSelector = nullptr;
    brls::DetailCell*   m_clearCacheCell        = nullptr;
    // Persistent SyncLounge session connect/disconnect cell (shows status).
    brls::DetailCell*   m_syncLoungeSyncCell    = nullptr;

    // Downloads section
    brls::BooleanCell* m_deleteAfterWatchToggle = nullptr;
    brls::DetailCell* m_clearDownloadsCell = nullptr;

    // Music section (split out of Playback)
    brls::SelectorCell* m_trackActionSelector = nullptr;
    brls::BooleanCell* m_backgroundMusicToggle = nullptr;

    // Live TV section
    brls::DetailCell*   m_defaultDvrLibraryCell = nullptr;
    brls::SelectorCell* m_dvrStartOffsetSelector = nullptr;
    brls::SelectorCell* m_dvrEndOffsetSelector   = nullptr;
    brls::BooleanCell*  m_dvrRecordPartialsToggle = nullptr;
    brls::SelectorCell* m_dvrMinQualitySelector  = nullptr;
    brls::SelectorCell* m_liveTvGuideHoursSelector = nullptr;
};

} // namespace vitaplex
