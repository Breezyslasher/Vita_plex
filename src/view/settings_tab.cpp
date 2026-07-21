/**
 * VitaPlex - Settings Tab implementation (master/detail layout).
 *
 * The tab is a ROW box containing a fixed-width rail of section names
 * on the left and a growing detail pane on the right. Each
 * createXSection() builder now returns its own brls::Box of cells;
 * SettingsTab keeps them all alive but only the active section's box
 * is VISIBLE in the detail content holder. All change handlers,
 * persistence, and cell types are preserved verbatim — only the
 * parent layout and per-section parenthood changed.
 */

#include "view/settings_tab.hpp"
#include "app/application.hpp"
#include "app/plex_client.hpp"
#include "app/plex_palette.hpp"
#include "app/downloads_manager.hpp"
#include "app/synclounge_session.hpp"
#include "view/media_detail_view.hpp"
#include "activity/player_activity.hpp"
#include "utils/http_client.hpp"
#include "utils/http_cache.hpp"
#include "platform/platform.hpp"
#include "platform/paths.hpp"
#include <set>
#include <chrono>
#include <fstream>
#include <memory>

#ifdef __vita__
#include <psp2/net/netctl.h>
#include <psp2/net/net.h>
#elif defined(_WIN32)
// Windows IP helper API for adapter enumeration + DNS info.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
// windows.h drags in <wingdi.h> which #defines ABSOLUTE (and a few
// other generic names) for the old GDI line-draw API. That collides
// head-on with brls::PositionType::ABSOLUTE used elsewhere in this
// file. Drop the GDI macros before the borealis headers see them.
#ifdef ABSOLUTE
#  undef ABSOLUTE
#endif
#ifdef RELATIVE
#  undef RELATIVE
#endif
#elif defined(__SWITCH__)
// libnx exposes the in-system network manager (nifm) for IP/profile
// queries. ifaddrs/resolv.conf don't exist on the Switch toolchain,
// so it gets its own branch.
#include <switch.h>
#else
// POSIX path (Linux / macOS / Android / iOS / tvOS / PS4). Uses the
// classic "connect a UDP socket to a public address and ask the
// kernel which local IP it chose" trick to get the active outbound
// IPv4 — works on every POSIX flavour without needing getifaddrs
// (which only landed in Android NDK at API 24). /etc/resolv.conf
// gives the resolver list where it exists. SSID/signal stay blank
// because there's no portable way to read them without nl80211 /
// CoreWLAN / NetworkManager bindings.
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace vitaplex {

// ─── design tokens ────────────────────────────────────────────────────
// Pulled from the redesign spec — kept inline (rather than in a header)
// because this is the only file that paints with them.
namespace tok {
    static inline NVGcolor bg()        { return vitaplex::palette::bg; }
    static inline NVGcolor railBg()    { return vitaplex::palette::panel; }
    static inline NVGcolor raised()    { return vitaplex::palette::surface2; }
    static inline NVGcolor hairline()  { return vitaplex::palette::line; }
    static inline NVGcolor text()      { return vitaplex::palette::text; }
    static inline NVGcolor muted()     { return vitaplex::palette::muted; }
    static inline NVGcolor dim()       { return vitaplex::palette::dim; }
    // Plex brand yellow (#E5A00D) used as the app-wide accent.
    static inline NVGcolor accent()    { return vitaplex::palette::gold; }
    static inline NVGcolor chipBg()    { return vitaplex::palette::goldTint(0.15f); }  // ~15% alpha
}

// Per-section metadata. The rail rows + detail header pull from this
// table — keep its order in sync with the SectionId enum in the header.
struct SectionMeta {
    const char* name;      // rail label + detail title
    const char* icon;      // file under BRLS_RESOURCES "/icons/"
    const char* subtitle;  // one-liner shown under the detail title
};

// Sections that use a dedicated MDI glyph use the matching PNG under
// resources/icons/ (rasterised from the MDI SVG at build time). The
// rest fall back to the closest existing in-tree asset.
//
// SEC_ABOUT is special — it never opens a sub-page, so its name field
// here is a placeholder; the rail builds it as a static info row that
// shows VITA_PLEX_DISPLAY_VERSION instead of "About".
static const SectionMeta kSections[] = {
    /* SEC_ACCOUNT     */ { "Account",         "account.png",
                            "Sign-in, switch user, and auto-login." },
    /* SEC_INTERFACE   */ { "Interface",       "theme-light-dark.png",
                            "Logging, diagnostics, and hidden libraries." },
    /* SEC_CONTENT     */ { "Content Display", "show.png",
                            "What appears in grids and library hubs." },
    /* SEC_PLAYBACK    */ { "Playback",        "play.png",
                            "Resume, seek behaviour, subtitles, intro/credits." },
    /* SEC_TRANSCODING */ { "Transcoding",     "options.png",
                            "Direct play, quality preset, and forced transcoding." },
    /* SEC_NETWORK     */ { "Network",         "web.png",
                            "Connection timeout for slow links." },
    /* SEC_DOWNLOADS   */ { "Downloads",       "download.png",
                            "Storage, cleanup, and delete-after-watch." },
    /* SEC_MUSIC       */ { "Music",           "music.png",
                            "Background playback and default track action." },
    /* SEC_LIVETV      */ { "Live TV & DVR",   "television-guide.png",
                            "Channel guide window and recording defaults." },
    /* SEC_ABOUT       */ { "About",           "information.png",
                            "" /* unused — rail uses the version string */ },
};
// kSections / SectionId stay-in-sync check has to live inside a
// member function (the constructor) so it can see the private enum.
// File-scope static_assert can't peek at SectionId::SEC_COUNT.

// ─── responsive sizing ────────────────────────────────────────────────
// Derive the rail width from the viewport so the tab still fits on a
// portrait Vita (960×544 logical) without devouring the detail pane.
static int railWidthForViewport() {
    float vw = brls::Application::contentWidth;
    if (vw >= 1280) return 280;
    if (vw >= 1024) return 240;
    if (vw >= 800)  return 220;
    if (vw >= 560)  return 180;
    return 160;   // really narrow — phone portrait; UI still functional
}

// ============================================================================
// Constructor & master/detail plumbing
// ============================================================================

SettingsTab::SettingsTab() {
    static_assert(sizeof(kSections) / sizeof(kSections[0]) == SEC_COUNT,
                  "kSections / SectionId out of sync");

    this->setAxis(brls::Axis::ROW);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setGrow(1.0f);
    this->setBackgroundColor(tok::bg());

    // ─── Rail (left) ────────────────────────────────────────────────
    m_railContainer = new brls::Box();
    m_railContainer->setAxis(brls::Axis::COLUMN);
    m_railContainer->setAlignItems(brls::AlignItems::STRETCH);
    m_railContainer->setWidth(railWidthForViewport());
    m_railContainer->setBackgroundColor(tok::railBg());

    // Rail header — "Settings" and the signed-in user. Mirrors the
    // tab header that the main app uses above the sidebar.
    auto* railHeader = new brls::Box();
    railHeader->setAxis(brls::Axis::COLUMN);
    railHeader->setPaddingLeft(18);
    railHeader->setPaddingRight(14);
    railHeader->setPaddingTop(18);
    railHeader->setPaddingBottom(14);

    auto* railTitle = new brls::Label();
    railTitle->setText("Settings");
    railTitle->setFontSize(22);
    railTitle->setTextColor(tok::text());
    railHeader->addView(railTitle);

    auto* railSubtitle = new brls::Label();
    {
        const auto& app = Application::getInstance();
        std::string base = app.getUsername().empty()
                               ? std::string("Not signed in")
                               : app.getUsername();
        if (!app.getCurrentHomeUserTitle().empty()) {
            base += " · " + app.getCurrentHomeUserTitle();
        }
        railSubtitle->setText(base);
    }
    railSubtitle->setFontSize(13);
    railSubtitle->setTextColor(tok::muted());
    railSubtitle->setMarginTop(2);
    railHeader->addView(railSubtitle);

    // Thin divider under the rail header.
    auto* railHairline = new brls::Box();
    railHairline->setHeight(1);
    railHairline->setBackgroundColor(tok::hairline());
    railHeader->addView(railHairline);

    m_railContainer->addView(railHeader);

    // Scrollable list of rail rows — twelve sections fit on desktop,
    // overflow scrolls on a Vita-sized viewport.
    m_railScroll = new brls::ScrollingFrame();
    m_railScroll->setGrow(1.0f);
    m_railScroll->setFocusable(false);  // descend straight onto a row

    m_railBox = new brls::Box();
    m_railBox->setAxis(brls::Axis::COLUMN);
    m_railBox->setAlignItems(brls::AlignItems::STRETCH);
    m_railBox->setPaddingTop(6);
    m_railBox->setPaddingBottom(6);

    m_railScroll->setContentView(m_railBox);
    m_railContainer->addView(m_railScroll);

    this->addView(m_railContainer);

    // ─── Detail (right) ────────────────────────────────────────────
    m_detailContainer = new brls::Box();
    m_detailContainer->setAxis(brls::Axis::COLUMN);
    m_detailContainer->setAlignItems(brls::AlignItems::STRETCH);
    m_detailContainer->setGrow(1.0f);
    m_detailContainer->setPaddingLeft(24);
    m_detailContainer->setPaddingRight(24);
    m_detailContainer->setPaddingTop(20);
    m_detailContainer->setPaddingBottom(12);

    // Section header — chip icon + title/subtitle column. The chip is
    // a tinted rounded square; updated in showSection().
    m_detailHeader = new brls::Box();
    m_detailHeader->setAxis(brls::Axis::ROW);
    m_detailHeader->setAlignItems(brls::AlignItems::CENTER);
    m_detailHeader->setMarginBottom(14);

    auto* headerTextCol = new brls::Box();
    headerTextCol->setAxis(brls::Axis::COLUMN);
    headerTextCol->setGrow(1.0f);

    m_detailTitle = new brls::Label();
    m_detailTitle->setFontSize(26);
    m_detailTitle->setTextColor(tok::text());
    headerTextCol->addView(m_detailTitle);

    m_detailSubtitle = new brls::Label();
    m_detailSubtitle->setFontSize(13);
    m_detailSubtitle->setTextColor(tok::muted());
    m_detailSubtitle->setMarginTop(3);
    headerTextCol->addView(m_detailSubtitle);

    m_detailHeader->addView(headerTextCol);
    m_detailContainer->addView(m_detailHeader);

    // Hairline under the section header.
    auto* detailHairline = new brls::Box();
    detailHairline->setHeight(1);
    detailHairline->setBackgroundColor(tok::hairline());
    detailHairline->setMarginBottom(10);
    m_detailContainer->addView(detailHairline);

    // Scrolling holder for the active section box.
    m_detailScroll = new brls::ScrollingFrame();
    m_detailScroll->setGrow(1.0f);
    m_detailScroll->setFocusable(false);

    m_detailContent = new brls::Box();
    m_detailContent->setAxis(brls::Axis::COLUMN);
    m_detailContent->setAlignItems(brls::AlignItems::STRETCH);

    m_detailScroll->setContentView(m_detailContent);
    m_detailContainer->addView(m_detailScroll);

    this->addView(m_detailContainer);

    // ─── Section boxes ─────────────────────────────────────────────
    // Build every section's Box up-front and stash it; showSection()
    // attaches one at a time. Order must match SectionId so
    // m_sectionBoxes[id] resolves correctly. SEC_ABOUT has no box —
    // its rail row is a static version readout, not a sub-page.
    m_sectionBoxes.resize(SEC_COUNT, nullptr);
    m_sectionBoxes[SEC_ACCOUNT]     = createAccountSection();
    m_sectionBoxes[SEC_INTERFACE]   = createUISection();
    m_sectionBoxes[SEC_CONTENT]     = createContentDisplaySection();
    m_sectionBoxes[SEC_PLAYBACK]    = createPlaybackSection();
    m_sectionBoxes[SEC_TRANSCODING] = createTranscodeSection();
    m_sectionBoxes[SEC_NETWORK]     = createNetworkSection();
    m_sectionBoxes[SEC_DOWNLOADS]   = createDownloadsSection();
    m_sectionBoxes[SEC_MUSIC]       = createMusicSection();
    m_sectionBoxes[SEC_LIVETV]      = createLiveTVSection();

    // Stage every section's box but do NOT add any of them to the
    // detail content yet — showSection() adds exactly one at a time.
    // The reason is borealis' Box::getDefaultFocus walks children and
    // their descendants for the first focusable view, *without*
    // checking Visibility::GONE. If we add every section box and only
    // toggle visibility, RIGHT from the rail lands on the first
    // focusable cell of section[0] (Account) regardless of which
    // section the user actually selected — focus goes to an invisible
    // cell.
    //
    // Keeping the unused section boxes detached (no parent) makes them
    // invisible to the focus walker and to Yoga; addView re-parents
    // the one we want to show, removeView(_, /*free=*/false) detaches
    // the previous one without destroying it.
    for (brls::Box* sec : m_sectionBoxes) {
        if (!sec) continue;
        sec->setVisibility(brls::Visibility::VISIBLE);
    }

    // ─── Rail rows ─────────────────────────────────────────────────
    m_railRows.resize(SEC_COUNT, nullptr);
    for (int id = 0; id < SEC_COUNT; id++) {
        if (id == SEC_ABOUT) {
            // About is a static, non-focusable readout — the version
            // string replaces the "About" label and there's no sub-page.
            // It sits at the bottom of the rail so the regular sections
            // remain reachable above it.
            brls::Box* info = makeRailInfoRow(kSections[id].icon,
                                              VITA_PLEX_DISPLAY_VERSION);
            m_railRows[id] = info;
            m_railBox->addView(info);
            continue;
        }
        brls::Box* row = makeRailRow(kSections[id].icon,
                                     kSections[id].name,
                                     id);
        m_railRows[id] = row;
        m_railBox->addView(row);
    }

    // Default landing — Account on first open.
    m_activeSection = SEC_ACCOUNT;
    showSection(m_activeSection);
}

SettingsTab::~SettingsTab() {
    // Each section box that's NOT the currently-attached one has no
    // parent; brls::Box::~Box only deletes children, so those orphans
    // would leak. Delete them here. The attached one is owned by
    // m_detailContent and will be freed by the base class destructor.
    for (brls::Box* sec : m_sectionBoxes) {
        if (sec && sec != m_attachedSection) {
            delete sec;
        }
    }
}

// Make a fresh column box with the spacing the detail pane expects.
// Returns a Box ready to hold cells; the caller owns it until the
// constructor passes it to m_detailContent.
brls::Box* SettingsTab::makeSectionBox() {
    auto* box = new brls::Box();
    box->setAxis(brls::Axis::COLUMN);
    box->setAlignItems(brls::AlignItems::STRETCH);
    box->setMarginBottom(20);
    return box;
}

// One rail row: icon + label, focusable, clickable, with a teal left
// accent bar and raised background when selected. The bar is a 4px
// ABSOLUTE child so it can sit flush with the row's edge without
// disturbing the row's content layout.
brls::Box* SettingsTab::makeRailRow(const std::string& iconPath,
                                    const std::string& title,
                                    int sectionId) {
    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setHeight(46);
    row->setMarginLeft(8);
    row->setMarginRight(8);
    row->setMarginTop(2);
    row->setMarginBottom(2);
    row->setCornerRadius(10);
    row->setPaddingLeft(12);
    row->setPaddingRight(10);
    row->setFocusable(true);

    // Teal left-edge bar (4px). Hidden until paintRailRowSelection()
    // toggles it on for the active row.
    auto* leftBar = new brls::Box();
    leftBar->setPositionType(brls::PositionType::ABSOLUTE);
    leftBar->setPositionLeft(0);
    leftBar->setPositionTop(8);
    leftBar->setWidth(4);
    leftBar->setHeight(30);
    leftBar->setCornerRadius(2);
    leftBar->setBackgroundColor(tok::accent());
    leftBar->setVisibility(brls::Visibility::INVISIBLE);
    leftBar->setId("rail/selected-bar");
    row->addView(leftBar);

    // Icon — borealis Image with FIT scaling so non-square assets keep
    // their aspect on the small chip.
    auto* icon = new brls::Image();
    icon->setWidth(20);
    icon->setHeight(20);
    icon->setScalingType(brls::ImageScalingType::FIT);
    icon->setMarginRight(12);
    icon->setImageFromRes("icons/" + iconPath);
    icon->setId("rail/icon");
    row->addView(icon);

    auto* label = new brls::Label();
    label->setText(title);
    label->setFontSize(15);
    label->setTextColor(tok::text());
    label->setGrow(1.0f);
    label->setId("rail/label");
    row->addView(label);

    // Right chevron — `right.png` is small enough to read as a hint
    // without crowding the row.
    auto* chevron = new brls::Image();
    chevron->setWidth(14);
    chevron->setHeight(14);
    chevron->setScalingType(brls::ImageScalingType::FIT);
    chevron->setImageFromRes("icons/right.png");
    chevron->setId("rail/chevron");
    row->addView(chevron);

    row->registerClickAction([this, sectionId](brls::View*) {
        showSection(sectionId);
        return true;
    });
    row->addGestureRecognizer(new brls::TapGestureRecognizer(row));

    // Make focus also select (one-press navigation feels right on a TV).
    row->getFocusEvent()->subscribe([this, sectionId](brls::View*) {
        if (m_activeSection != sectionId) {
            showSection(sectionId);
        }
    });

    return row;
}

// Static rail footer entry — same icon + label scaffolding as a regular
// rail row but with focusable=false, no click handler, no chevron, and
// muted text. Used for the version readout at the bottom of the rail.
brls::Box* SettingsTab::makeRailInfoRow(const std::string& iconPath,
                                        const std::string& title) {
    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setHeight(40);
    row->setMarginLeft(8);
    row->setMarginRight(8);
    row->setMarginTop(6);
    row->setMarginBottom(4);
    row->setPaddingLeft(12);
    row->setPaddingRight(10);
    // Explicitly non-focusable — Box defaults to false but the
    // surrounding rows are focusable=true so spell it out here too
    // to keep the contrast obvious to readers.
    row->setFocusable(false);

    auto* icon = new brls::Image();
    icon->setWidth(18);
    icon->setHeight(18);
    icon->setScalingType(brls::ImageScalingType::FIT);
    icon->setMarginRight(10);
    icon->setImageFromRes("icons/" + iconPath);
    row->addView(icon);

    auto* label = new brls::Label();
    label->setText(title);
    label->setFontSize(13);
    label->setTextColor(tok::muted());
    label->setGrow(1.0f);
    row->addView(label);

    return row;
}

void SettingsTab::showSection(int sectionId) {
    if (sectionId < 0 || sectionId >= SEC_COUNT) return;
    brls::Box* target = m_sectionBoxes[sectionId];
    if (!target) return;

    // Swap which section box owns the detail content holder. We rely on
    // m_attachedSection rather than getParent() because brls
    // removeView(_, /*free=*/false) doesn't clear the view's parent
    // pointer, so getParent() lies after a detach. Borealis' focus
    // walker only sees attached children, so detaching the previous
    // section's box stops RIGHT-from-rail from landing on its cells.
    if (m_attachedSection != target) {
        if (m_attachedSection) {
            m_detailContent->removeView(m_attachedSection, /*free=*/false);
        }
        m_detailContent->addView(target);
        m_attachedSection = target;

        // Re-point lastFocusedView along the ancestor chain so a later
        // RIGHT-from-rail walk doesn't tunnel through the stale
        // lastFocusedView (which still pointed into the detached
        // section's cells) and re-focus an invisible widget. Without
        // this the focus jumped back into Account's autoLogin toggle
        // even when Account was no longer the visible section.
        m_detailContent->setLastFocusedView(target);
        if (m_detailScroll) m_detailScroll->setLastFocusedView(m_detailContent);
        if (m_detailContainer) m_detailContainer->setLastFocusedView(m_detailScroll);

        // The scroll frame keeps its offset across the content swap, so a
        // new section opened after scrolling deep into a long one started
        // mid-list (or past its end, if shorter). Every section switch
        // starts reading from the top.
        if (m_detailScroll) m_detailScroll->setContentOffsetY(0.0f, false);
    }

    // Header text.
    if (m_detailTitle)    m_detailTitle->setText(kSections[sectionId].name);
    if (m_detailSubtitle) m_detailSubtitle->setText(kSections[sectionId].subtitle);

    m_activeSection = sectionId;
    paintRailRowSelection();
}

void SettingsTab::paintRailRowSelection() {
    for (int i = 0; i < (int)m_railRows.size(); i++) {
        brls::Box* row = m_railRows[i];
        if (!row) continue;
        bool active = (i == m_activeSection);
        row->setBackgroundColor(active ? tok::raised()
                                       : nvgRGBA(0, 0, 0, 0));
        if (auto* bar = row->getView("rail/selected-bar")) {
            bar->setVisibility(active ? brls::Visibility::VISIBLE
                                      : brls::Visibility::INVISIBLE);
        }
    }
}

// ============================================================================
// Per-section builders. Each returns a fresh Box of cells — wiring of
// each cell's change handler, persistence, and getter/setter is
// preserved from the original implementation verbatim.
// ============================================================================

brls::Box* SettingsTab::createAccountSection() {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();
    brls::Box* box = makeSectionBox();

    // User info cell. When the user has switched into a Plex Home
    // managed user, show both the account login and the current user so
    // it's obvious which library they'll see.
    m_userLabel = new brls::Label();
    {
        std::string base = app.getUsername().empty()
                               ? std::string("Not logged in")
                               : app.getUsername();
        if (!app.getCurrentHomeUserTitle().empty()) {
            base += " (as " + app.getCurrentHomeUserTitle() + ")";
        }
        m_userLabel->setText("User: " + base);
    }
    m_userLabel->setFontSize(18);
    m_userLabel->setMarginLeft(16);
    m_userLabel->setMarginBottom(8);
    box->addView(m_userLabel);

    // Server info cell
    m_serverLabel = new brls::Label();
    m_serverLabel->setText("Server: " + (app.getServerUrl().empty() ? "Not connected" : app.getServerUrl()));
    m_serverLabel->setFontSize(18);
    m_serverLabel->setMarginLeft(16);
    m_serverLabel->setMarginBottom(16);
    box->addView(m_serverLabel);

    // Plex Home: auto-login + switch-user. The switch cell sits above
    // logout so a user who wants to swap accounts (rather than fully
    // sign out) finds the right action first.
    m_autoLoginToggle = new brls::BooleanCell();
    m_autoLoginToggle->init("Auto-login as current user", settings.autoLoginAsLastUser,
        [](bool v) {
            AppSettings& s = Application::getInstance().getSettings();
            s.autoLoginAsLastUser = v;
            Application::getInstance().saveSettings();
        });
    box->addView(m_autoLoginToggle);

    m_switchUserCell = new brls::DetailCell();
    m_switchUserCell->setText("Switch User");
    m_switchUserCell->setDetailText(
        app.getCurrentHomeUserTitle().empty()
            ? "Pick another Plex Home user"
            : ("Current: " + app.getCurrentHomeUserTitle()));
    m_switchUserCell->registerClickAction([this](brls::View*) {
        onSwitchUser();
        return true;
    });
    box->addView(m_switchUserCell);

    // Logout button
    auto* logoutCell = new brls::DetailCell();
    logoutCell->setText("Logout");
    logoutCell->setDetailText("Sign out from current account");
    logoutCell->registerClickAction([this](brls::View* view) {
        onLogout();
        return true;
    });
    box->addView(logoutCell);

    return box;
}

void SettingsTab::onSwitchUser() {
    Application::getInstance().showHomeUserPicker([this]() {
        Application& app = Application::getInstance();
        if (m_switchUserCell) {
            m_switchUserCell->setDetailText(
                app.getCurrentHomeUserTitle().empty()
                    ? "Pick another Plex Home user"
                    : ("Current: " + app.getCurrentHomeUserTitle()));
        }
        if (m_userLabel) {
            std::string base = app.getUsername().empty()
                                   ? std::string("Not logged in")
                                   : app.getUsername();
            if (!app.getCurrentHomeUserTitle().empty()) {
                base += " (as " + app.getCurrentHomeUserTitle() + ")";
            }
            m_userLabel->setText("User: " + base);
        }
    });
}

brls::Box* SettingsTab::createUISection() {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();
    brls::Box* box = makeSectionBox();

    // Debug logging toggle
    m_debugLogToggle = new brls::BooleanCell();
    m_debugLogToggle->init("Debug Logging", settings.debugLogging, [&settings](bool value) {
        settings.debugLogging = value;
        Application::getInstance().applyLogLevel();
        Application::getInstance().saveSettings();
    });
    box->addView(m_debugLogToggle);

    // MPV stats overlay toggle — surfaces codec, hwdec, FPS, frame
    // drops, and cache state on top of the video so playback issues
    // can be diagnosed without an adb cable. Lives in Interface
    // alongside "Debug Logging" — both are developer-focused toggles.
    // See PlayerActivity::updateMpvStatsOverlay().
    auto* mpvStatsToggle = new brls::BooleanCell();
    mpvStatsToggle->init("MPV Stats Overlay", settings.showMpvStats,
                         [&settings](bool value) {
        settings.showMpvStats = value;
        Application::getInstance().saveSettings();
    });
    box->addView(mpvStatsToggle);

    // Manage hidden libraries (+ Live TV / Downloads). Hidden items are kept out
    // of the sidebar and the sidebar reorder editor; this is the one place to
    // toggle them.
    m_hiddenLibrariesCell = new brls::DetailCell();
    m_hiddenLibrariesCell->setText("Manage Hidden Libraries");
    {
        // Count hidden entries across both stores: libraries plus the hideable
        // built-ins (Live TV / Downloads) — mirrors what the dialog shows.
        auto tokens = [](const std::string& csv) {
            std::vector<std::string> out;
            std::string s = csv;
            size_t p;
            while ((p = s.find(',')) != std::string::npos) {
                if (p > 0) out.push_back(s.substr(0, p));
                s.erase(0, p + 1);
            }
            if (!s.empty()) out.push_back(s);
            return out;
        };
        int hiddenCount = (int)tokens(settings.hiddenLibraries).size();
        for (const auto& t : tokens(settings.hiddenSidebarItems))
            if (t == "livetv" || t == "downloads") hiddenCount++;
        m_hiddenLibrariesCell->setDetailText(
            hiddenCount > 0 ? std::to_string(hiddenCount) + " hidden" : "None hidden");
    }
    m_hiddenLibrariesCell->registerClickAction([this](brls::View* view) {
        onManageHiddenLibraries();
        return true;
    });
    box->addView(m_hiddenLibrariesCell);

    return box;
}

brls::Box* SettingsTab::createContentDisplaySection() {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();
    brls::Box* box = makeSectionBox();

    // Show collections toggle
    m_collectionsToggle = new brls::BooleanCell();
    m_collectionsToggle->init("Show Collections", settings.showCollections, [&settings](bool value) {
        settings.showCollections = value;
        Application::getInstance().saveSettings();
    });
    box->addView(m_collectionsToggle);

    // Show playlists toggle
    m_playlistsToggle = new brls::BooleanCell();
    m_playlistsToggle->init("Show Playlists", settings.showPlaylists, [&settings](bool value) {
        settings.showPlaylists = value;
        Application::getInstance().saveSettings();
    });
    box->addView(m_playlistsToggle);

    // Show genres/categories toggle
    m_genresToggle = new brls::BooleanCell();
    m_genresToggle->init("Show Categories", settings.showGenres, [&settings](bool value) {
        settings.showGenres = value;
        Application::getInstance().saveSettings();
    });
    box->addView(m_genresToggle);

    // Hide titles toggle
    m_hideTitlesToggle = new brls::BooleanCell();
    m_hideTitlesToggle->init("Hide Titles Under Posters", settings.hideTitlesInGrid, [&settings](bool value) {
        settings.hideTitlesInGrid = value;
        Application::getInstance().saveSettings();
    });
    box->addView(m_hideTitlesToggle);

    // Skip single season toggle
    m_skipSingleSeasonToggle = new brls::BooleanCell();
    m_skipSingleSeasonToggle->init("Skip Season for Single-Season Shows", settings.skipSingleSeason, [&settings](bool value) {
        settings.skipSingleSeason = value;
        Application::getInstance().saveSettings();
    });
    box->addView(m_skipSingleSeasonToggle);

    // Info label
    auto* contentInfoLabel = new brls::Label();
    contentInfoLabel->setText("Hides empty sections automatically");
    contentInfoLabel->setFontSize(14);
    contentInfoLabel->setMarginLeft(16);
    contentInfoLabel->setMarginTop(8);
    box->addView(contentInfoLabel);

    return box;
}

brls::Box* SettingsTab::createPlaybackSection() {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();
    brls::Box* box = makeSectionBox();

    // Auto-play next toggle
    m_autoPlayToggle = new brls::BooleanCell();
    m_autoPlayToggle->init("Auto-Play Next Episode", settings.autoPlayNext, [&settings](bool value) {
        settings.autoPlayNext = value;
        Application::getInstance().saveSettings();
    });
    box->addView(m_autoPlayToggle);

    // Resume playback toggle
    m_resumeToggle = new brls::BooleanCell();
    m_resumeToggle->init("Resume Playback", settings.resumePlayback, [&settings](bool value) {
        settings.resumePlayback = value;
        Application::getInstance().saveSettings();
    });
    box->addView(m_resumeToggle);

    // Show subtitles toggle
    m_subtitlesToggle = new brls::BooleanCell();
    m_subtitlesToggle->init("Show Subtitles", settings.showSubtitles, [&settings](bool value) {
        settings.showSubtitles = value;
        Application::getInstance().saveSettings();
    });
    box->addView(m_subtitlesToggle);

    // Subtitle size selector
    m_subtitleSizeSelector = new brls::SelectorCell();
    m_subtitleSizeSelector->init("Subtitle Size", {"Small", "Medium", "Large"},
        static_cast<int>(settings.subtitleSize),
        [this](int index) {
            onSubtitleSizeChanged(index);
        });
    box->addView(m_subtitleSizeSelector);

    // Default subtitle language — used as the prefill when the user
    // opens the "Search online for subtitles…" dialog on a movie detail
    // page. ISO 639-1 / -2 code (e.g. en, es, fr, ja, pt-br). DetailCell
    // shows the current value next to the label and opens an IME on
    // tap so the user can edit it without leaving the settings screen.
    auto* defaultSubLangCell = new brls::DetailCell();
    defaultSubLangCell->setText("Default Subtitle Language");
    defaultSubLangCell->setDetailText(
        settings.defaultSubtitleLanguage.empty()
            ? std::string("en")
            : settings.defaultSubtitleLanguage);
    defaultSubLangCell->registerClickAction([defaultSubLangCell](brls::View*) {
        auto* ime = brls::Application::getImeManager();
        if (!ime) return true;
        AppSettings& s = Application::getInstance().getSettings();
        std::string current = s.defaultSubtitleLanguage.empty()
            ? std::string("en")
            : s.defaultSubtitleLanguage;
        ime->openForText([defaultSubLangCell](std::string text) {
            AppSettings& s = Application::getInstance().getSettings();
            if (text.empty()) text = "en";
            s.defaultSubtitleLanguage = text;
            Application::getInstance().saveSettings();
            defaultSubLangCell->setDetailText(text);
        }, "Default subtitle language (e.g. en, es, fr)",
           current, 8, current);
        return true;
    });
    defaultSubLangCell->addGestureRecognizer(new brls::TapGestureRecognizer(defaultSubLangCell));
    box->addView(defaultSubLangCell);

    // Seek interval selector
    m_seekIntervalSelector = new brls::SelectorCell();
    m_seekIntervalSelector->init("Seek Interval",
        {"5 seconds", "10 seconds", "15 seconds", "30 seconds", "60 seconds"},
        settings.seekInterval == 5 ? 0 :
        settings.seekInterval == 10 ? 1 :
        settings.seekInterval == 15 ? 2 :
        settings.seekInterval == 30 ? 3 : 4,
        [this](int index) {
            onSeekIntervalChanged(index);
        });
    box->addView(m_seekIntervalSelector);

    // Controls auto-hide selector
    m_controlsAutoHideSelector = new brls::SelectorCell();
    m_controlsAutoHideSelector->init("Controls Auto-Hide",
        {"Never", "3 seconds", "5 seconds", "10 seconds", "15 seconds"},
        settings.controlsAutoHideSeconds == 0 ? 0 :
        settings.controlsAutoHideSeconds == 3 ? 1 :
        settings.controlsAutoHideSeconds == 5 ? 2 :
        settings.controlsAutoHideSeconds == 10 ? 3 : 4,
        [this](int index) {
            onControlsAutoHideChanged(index);
        });
    box->addView(m_controlsAutoHideSelector);

    // Auto-skip intro toggle
    m_autoSkipIntroToggle = new brls::BooleanCell();
    m_autoSkipIntroToggle->init("Auto-Skip Intro", settings.autoSkipIntro, [&settings](bool value) {
        settings.autoSkipIntro = value;
        Application::getInstance().saveSettings();
    });
    box->addView(m_autoSkipIntroToggle);

    // Auto-skip credits toggle
    m_autoSkipCreditsToggle = new brls::BooleanCell();
    m_autoSkipCreditsToggle->init("Auto-Skip Credits", settings.autoSkipCredits, [&settings](bool value) {
        settings.autoSkipCredits = value;
        Application::getInstance().saveSettings();
    });
    box->addView(m_autoSkipCreditsToggle);

    // Info label for skip settings
    auto* skipInfoLabel = new brls::Label();
    skipInfoLabel->setText("When off, a skip button appears briefly");
    skipInfoLabel->setFontSize(14);
    skipInfoLabel->setMarginLeft(16);
    skipInfoLabel->setMarginTop(8);
    box->addView(skipInfoLabel);

    // Local playback smoke test — opens the player on a known-good
    // file under ux0:data/VitaPlex/. Was previously under the
    // (now-removed) Debug section.
    auto* testLocalCell = new brls::DetailCell();
    testLocalCell->setText("Test Local Playback");
    // Show the platform's actual data dir so the user knows where to
    // drop the test file. platformPath("") yields "<root>/" — strip
    // the trailing slash for a clean display string.
    {
        std::string dataDir = platformPath("");
        if (!dataDir.empty() && dataDir.back() == '/') dataDir.pop_back();
        testLocalCell->setDetailText("Place test.mp4 or test.mp3 in " + dataDir);
    }
    testLocalCell->registerClickAction([this](brls::View*) {
        onTestLocalPlayback();
        return true;
    });
    box->addView(testLocalCell);

    return box;
}

brls::Box* SettingsTab::createMusicSection() {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();
    brls::Box* box = makeSectionBox();

    // Default track action selector
    m_trackActionSelector = new brls::SelectorCell();
    m_trackActionSelector->init("Default Track Action",
        {"Play Next", "Play Now (Replace Current)", "Add to Bottom of Queue", "Play Now (Clear Queue)", "Ask Each Time"},
        static_cast<int>(settings.trackDefaultAction),
        [](int index) {
            Application& app = Application::getInstance();
            app.getSettings().trackDefaultAction = static_cast<TrackDefaultAction>(index);
            app.saveSettings();
        });
    box->addView(m_trackActionSelector);

    // Background music toggle
    m_backgroundMusicToggle = new brls::BooleanCell();
    m_backgroundMusicToggle->init("Background Music", settings.backgroundMusic, [&settings](bool value) {
        settings.backgroundMusic = value;
        Application::getInstance().saveSettings();
    });
    box->addView(m_backgroundMusicToggle);

    // Info label for music settings
    auto* musicInfoLabel = new brls::Label();
    musicInfoLabel->setText("Background music lets you leave player to add more songs");
    musicInfoLabel->setFontSize(14);
    musicInfoLabel->setMarginLeft(16);
    musicInfoLabel->setMarginTop(8);
    box->addView(musicInfoLabel);

    return box;
}

brls::Box* SettingsTab::createTranscodeSection() {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();
    brls::Box* box = makeSectionBox();

    // Video quality selector
    m_qualitySelector = new brls::SelectorCell();
    m_qualitySelector->init("Video Quality",
        {"Original (Direct Play)", "1080p (20 Mbps)", "720p (4 Mbps)", "480p (2 Mbps)", "360p (1 Mbps)", "240p (500 Kbps)"},
        static_cast<int>(settings.videoQuality),
        [this](int index) {
            onQualityChanged(index);
        });
    box->addView(m_qualitySelector);

    // Force transcode toggle
    m_forceTranscodeToggle = new brls::BooleanCell();
    m_forceTranscodeToggle->init("Force Transcode", settings.forceTranscode, [&settings](bool value) {
        settings.forceTranscode = value;
        Application::getInstance().saveSettings();
    });
    box->addView(m_forceTranscodeToggle);

    // Direct play toggle
    m_directPlayToggle = new brls::BooleanCell();
    m_directPlayToggle->init("Try Direct Play First", settings.directPlay, [&settings](bool value) {
        settings.directPlay = value;
        Application::getInstance().saveSettings();
    });
    box->addView(m_directPlayToggle);

    return box;
}

brls::Box* SettingsTab::createNetworkSection() {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();
    brls::Box* box = makeSectionBox();

    // Connection timeout selector — split out from Transcoding so it
    // has its own rail row. Setting + handler are unchanged.
    m_connectionTimeoutSelector = new brls::SelectorCell();
    m_connectionTimeoutSelector->init("Connection Timeout",
        {"30 seconds", "60 seconds", "120 seconds", "180 seconds", "300 seconds"},
        settings.connectionTimeout == 30 ? 0 :
        settings.connectionTimeout == 60 ? 1 :
        settings.connectionTimeout == 120 ? 2 :
        settings.connectionTimeout == 300 ? 4 : 3,
        [this](int index) {
            onConnectionTimeoutChanged(index);
        });
    box->addView(m_connectionTimeoutSelector);

    // Network diagnostic — opens a dialog showing WiFi info, internet
    // reachability, and Plex server latency. Was previously under the
    // (now-removed) Debug section.
    auto* networkTestCell = new brls::DetailCell();
    networkTestCell->setText("Network Test");
    networkTestCell->setDetailText("View WiFi info and test Plex connection");
    networkTestCell->registerClickAction([this](brls::View*) {
        onNetworkTest();
        return true;
    });
    box->addView(networkTestCell);

    // Persistent SyncLounge session: while connected, an active player follows
    // the room host's play / pause / seek (receive-only for now). Click to
    // connect (prompts server + room) or, when already connected, disconnect.
    m_syncLoungeSyncCell = new brls::DetailCell();
    m_syncLoungeSyncCell->setText("SyncLounge Sync");
    {
        auto& sl = SyncLoungeSession::instance();
        const std::string savedRoom = Application::getInstance().getSettings().syncLoungeRoom;
        m_syncLoungeSyncCell->setDetailText(
            sl.isConnected()      ? ("Connected: " + sl.room())
            : !savedRoom.empty()  ? ("Disconnected — room \"" + savedRoom + "\"")
                                  : std::string("Disconnected"));
    }
    m_syncLoungeSyncCell->registerClickAction([this](brls::View*) {
        onSyncLoungeConnect();
        return true;
    });
    box->addView(m_syncLoungeSyncCell);

    // Party Pause: when on, ANY member can pause/resume the whole party. Only
    // the room host can change this (the server gates it), so toggling here is
    // effective only while connected as host; otherwise it's a no-op.
    auto* partyPauseCell = new brls::BooleanCell();
    partyPauseCell->init("Party Pause", SyncLoungeSession::instance().isPartyPauseEnabled(),
        [](bool value) {
            auto& sl = SyncLoungeSession::instance();
            if (!sl.isConnected()) {
                brls::Application::notify("Connect to SyncLounge first");
                return;
            }
            if (!sl.isHost()) {
                brls::Application::notify("Only the host can change party pause");
                return;
            }
            sl.setPartyPauseEnabled(value);
            brls::Application::notify(value ? "Party pause enabled" : "Party pause disabled");
        });
    box->addView(partyPauseCell);

    // Room Auto Host: the server-wide switch (host-only). When on, the server
    // promotes ANY non-host who starts a new video to host. Mirrors Party Pause —
    // only the current host can change it; the server disconnects a non-host
    // sender, so we gate to isHost() and otherwise notify.
    auto* roomAutoHostCell = new brls::BooleanCell();
    roomAutoHostCell->init("Room Auto Host", SyncLoungeSession::instance().isRoomAutoHostEnabled(),
        [](bool value) {
            auto& sl = SyncLoungeSession::instance();
            if (!sl.isConnected()) {
                brls::Application::notify("Connect to SyncLounge first");
                return;
            }
            if (!sl.isHost()) {
                brls::Application::notify("Only the host can change room auto-host");
                return;
            }
            sl.setRoomAutoHostEnabled(value);
            brls::Application::notify(value
                ? "Room auto-host on — anyone who starts a video becomes host"
                : "Room auto-host off");
        });
    box->addView(roomAutoHostCell);

    // Party Members: opens a dialog (styled like the join-session prompt)
    // listing everyone currently in the room. When you're the host, tapping a
    // member hands host to them.
    auto* membersCell = new brls::DetailCell();
    membersCell->setText("Party Members");
    membersCell->setDetailText("See who's here · change host");
    membersCell->registerClickAction([this](brls::View*) {
        onSyncLoungeMembers();
        return true;
    });
    box->addView(membersCell);

    auto* infoLabel = new brls::Label();
    infoLabel->setText("Raise the timeout if you're on a slow or unstable link.");
    infoLabel->setFontSize(14);
    infoLabel->setMarginLeft(16);
    infoLabel->setMarginTop(8);
    box->addView(infoLabel);

    // HTTP cache controls. Lifetime is a single global TTL applied to
    // every cached endpoint (library sections, Home hubs, Live TV
    // channels). "Off" disables both reads and writes so the user gets
    // fully live data; the other presets cover from "snappy nav" (15
    // min) to "set and forget" (1 week).
    static const std::vector<int> kCacheMinutes = { 0, 15, 60, 1440, 10080 };
    static const std::vector<std::string> kCacheLabels = {
        "Off", "15 minutes", "1 hour", "1 day", "1 week"
    };
    int cacheIdx = 2;  // default 1 hour
    for (size_t i = 0; i < kCacheMinutes.size(); i++) {
        if (kCacheMinutes[i] == settings.cacheLifetimeMinutes) {
            cacheIdx = (int)i;
            break;
        }
    }
    m_cacheLifetimeSelector = new brls::SelectorCell();
    m_cacheLifetimeSelector->init("Cache Lifetime", kCacheLabels, cacheIdx,
        [](int idx) {
            AppSettings& s = Application::getInstance().getSettings();
            s.cacheLifetimeMinutes = kCacheMinutes[idx];
            Application::getInstance().saveSettings();
        });
    box->addView(m_cacheLifetimeSelector);

    // Clear cache button. Detail text shows the live byte count so the
    // user can see whether the cache is actually doing anything.
    m_clearCacheCell = new brls::DetailCell();
    m_clearCacheCell->setText("Clear Cache");
    {
        size_t bytes = HttpCache::totalBytes();
        size_t entries = HttpCache::entryCount();
        if (entries == 0) {
            m_clearCacheCell->setDetailText("Empty");
        } else {
            char buf[64];
            if (bytes >= 1024 * 1024) {
                snprintf(buf, sizeof(buf), "%zu entries, %.1f MB",
                         entries, bytes / (1024.0 * 1024.0));
            } else {
                snprintf(buf, sizeof(buf), "%zu entries, %zu KB",
                         entries, (bytes + 1023) / 1024);
            }
            m_clearCacheCell->setDetailText(buf);
        }
    }
    m_clearCacheCell->registerClickAction([this](brls::View*) {
        HttpCache::clear();
        if (m_clearCacheCell) m_clearCacheCell->setDetailText("Empty");
        brls::Application::notify("Cache cleared");
        return true;
    });
    box->addView(m_clearCacheCell);

    return box;
}

brls::Box* SettingsTab::createDownloadsSection() {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();
    brls::Box* box = makeSectionBox();

    // Delete after watch toggle
    m_deleteAfterWatchToggle = new brls::BooleanCell();
    m_deleteAfterWatchToggle->init("Delete After Watching", settings.deleteAfterWatch, [&settings](bool value) {
        settings.deleteAfterWatch = value;
        Application::getInstance().saveSettings();
    });
    box->addView(m_deleteAfterWatchToggle);

    // Download quality. Original keeps the source as-is on HEVC-capable
    // platforms (fast, no transcode); a lower tier forces a server-side
    // transcode to that size — smaller files, faster on the Vita, plays
    // everywhere. Index maps 1:1 to VideoQuality (0 = Original).
    auto* dlQualitySelector = new brls::SelectorCell();
    dlQualitySelector->init("Download Quality",
        {"Original (no transcode)", "1080p", "720p", "480p", "360p", "240p"},
        static_cast<int>(settings.downloadQuality),
        [&settings](int index) {
            settings.downloadQuality = static_cast<VideoQuality>(index);
            Application::getInstance().saveSettings();
        });
    box->addView(dlQualitySelector);

    // Keep the source's surround audio instead of downmixing to stereo.
    auto* dlKeepAudioToggle = new brls::BooleanCell();
    dlKeepAudioToggle->init("Keep Original Audio", settings.downloadKeepOriginalAudio, [&settings](bool value) {
        settings.downloadKeepOriginalAudio = value;
        Application::getInstance().saveSettings();
    });
    box->addView(dlKeepAudioToggle);

    // Embed subtitles into transcoded downloads (otherwise they're stripped).
    auto* dlSubtitlesToggle = new brls::BooleanCell();
    dlSubtitlesToggle->init("Include Subtitles", settings.downloadIncludeSubtitles, [&settings](bool value) {
        settings.downloadIncludeSubtitles = value;
        Application::getInstance().saveSettings();
    });
    box->addView(dlSubtitlesToggle);

    // Clear all downloads
    m_clearDownloadsCell = new brls::DetailCell();
    m_clearDownloadsCell->setText("Clear All Downloads");
    auto downloads = DownloadsManager::getInstance().getDownloads();
    m_clearDownloadsCell->setDetailText(std::to_string(downloads.size()) + " items");
    m_clearDownloadsCell->registerClickAction([this](brls::View* view) {
        brls::Dialog* dialog = new brls::Dialog("Delete all downloaded content?");

        dialog->addButton("Cancel", []() {});

        dialog->addButton("Delete All", [this]() {
            auto downloads = DownloadsManager::getInstance().getDownloads();
            for (const auto& item : downloads) {
                DownloadsManager::getInstance().deleteDownload(item.ratingKey);
            }
            if (m_clearDownloadsCell) {
                m_clearDownloadsCell->setDetailText("0 items");
            }
            brls::Application::notify("All downloads deleted");
        });

        dialog->open();
        return true;
    });
    box->addView(m_clearDownloadsCell);

    // Downloads storage path info
    auto* pathLabel = new brls::Label();
    pathLabel->setText("Storage: " + DownloadsManager::getInstance().getDownloadsPath());
    pathLabel->setFontSize(14);
    pathLabel->setMarginLeft(16);
    pathLabel->setMarginTop(8);
    box->addView(pathLabel);

    return box;
}

brls::Box* SettingsTab::createLiveTVSection() {
    AppSettings& settings = Application::getInstance().getSettings();
    brls::Box* box = makeSectionBox();

    m_defaultDvrLibraryCell = new brls::DetailCell();
    m_defaultDvrLibraryCell->setText("Default DVR Library");
    m_defaultDvrLibraryCell->setDetailText(
        settings.defaultDvrSectionId.empty()
            ? "Server default"
            : (settings.defaultDvrSectionTitle.empty()
                   ? ("Library #" + settings.defaultDvrSectionId)
                   : settings.defaultDvrSectionTitle));
    m_defaultDvrLibraryCell->registerClickAction([this](brls::View*) {
        onManageDefaultDvrLibrary();
        return true;
    });
    box->addView(m_defaultDvrLibraryCell);

    // Recording padding — most over-the-air programs run 1-2 minutes
    // long; a 0/1/2/3/5/10 ladder covers the realistic range without
    // letting the user accidentally clip a 1-hour show into 45 minutes.
    static const std::vector<int> kOffsetMinutes = { 0, 1, 2, 3, 5, 10 };
    auto offsetLabels = []() {
        std::vector<std::string> v;
        for (int m : kOffsetMinutes)
            v.push_back(m == 0 ? "Off" : (std::to_string(m) + " min"));
        return v;
    };
    auto indexOfOffset = [](int v) {
        for (size_t i = 0; i < kOffsetMinutes.size(); i++)
            if (kOffsetMinutes[i] == v) return (int)i;
        return 2; // default 2 min
    };

    m_dvrStartOffsetSelector = new brls::SelectorCell();
    m_dvrStartOffsetSelector->init("Recording Start Padding",
                                   offsetLabels(),
                                   indexOfOffset(settings.dvrStartOffsetMinutes),
        [](int idx) {
            AppSettings& s = Application::getInstance().getSettings();
            s.dvrStartOffsetMinutes = kOffsetMinutes[idx];
            Application::getInstance().saveSettings();
        });
    box->addView(m_dvrStartOffsetSelector);

    m_dvrEndOffsetSelector = new brls::SelectorCell();
    m_dvrEndOffsetSelector->init("Recording End Padding",
                                 offsetLabels(),
                                 indexOfOffset(settings.dvrEndOffsetMinutes),
        [](int idx) {
            AppSettings& s = Application::getInstance().getSettings();
            s.dvrEndOffsetMinutes = kOffsetMinutes[idx];
            Application::getInstance().saveSettings();
        });
    box->addView(m_dvrEndOffsetSelector);

    m_dvrRecordPartialsToggle = new brls::BooleanCell();
    m_dvrRecordPartialsToggle->init("Keep Partial Recordings",
                                    settings.dvrRecordPartials,
        [](bool value) {
            AppSettings& s = Application::getInstance().getSettings();
            s.dvrRecordPartials = value;
            Application::getInstance().saveSettings();
        });
    box->addView(m_dvrRecordPartialsToggle);

    // minVideoQuality is a 0-100 threshold on the Plex DVR side; the
    // tuner picks a stream whose advertised quality meets or exceeds it.
    // Expose four bands that cover the practical OTA / cable spread.
    static const std::vector<int> kMinQualities = { 0, 50, 75, 100 };
    static const std::vector<std::string> kMinQualityLabels = {
        "Any quality", "Standard (≥480p)", "High (≥720p)", "Best (≥1080p)"
    };
    int qIdx = 0;
    for (size_t i = 0; i < kMinQualities.size(); i++) {
        if (kMinQualities[i] == settings.dvrMinVideoQuality) { qIdx = (int)i; break; }
    }
    m_dvrMinQualitySelector = new brls::SelectorCell();
    m_dvrMinQualitySelector->init("Minimum Recording Quality",
                                  kMinQualityLabels, qIdx,
        [](int idx) {
            AppSettings& s = Application::getInstance().getSettings();
            s.dvrMinVideoQuality = kMinQualities[idx];
            Application::getInstance().saveSettings();
        });
    box->addView(m_dvrMinQualitySelector);

    // EPG window. LiveTVTab caps at EPG_GRID_HOURS_VISIBLE internally,
    // so this is a fetch-size hint as much as a render setting. 6/12/24
    // are the three Plex-side daily slot boundaries.
    static const std::vector<int> kGuideHours = { 6, 12, 24 };
    static const std::vector<std::string> kGuideHourLabels = {
        "6 hours", "12 hours", "24 hours"
    };
    int hIdx = 1; // default 12
    for (size_t i = 0; i < kGuideHours.size(); i++) {
        if (kGuideHours[i] == settings.liveTvGuideHours) { hIdx = (int)i; break; }
    }
    m_liveTvGuideHoursSelector = new brls::SelectorCell();
    m_liveTvGuideHoursSelector->init("Program Guide Window",
                                     kGuideHourLabels, hIdx,
        [](int idx) {
            AppSettings& s = Application::getInstance().getSettings();
            s.liveTvGuideHours = kGuideHours[idx];
            Application::getInstance().saveSettings();
        });
    box->addView(m_liveTvGuideHoursSelector);

    return box;
}

// ============================================================================
// Change handlers (preserved verbatim from the original implementation)
// ============================================================================

void SettingsTab::onLogout() {
    brls::Dialog* dialog = new brls::Dialog("Are you sure you want to logout?");

    dialog->addButton("Cancel", []() {});

    dialog->addButton("Logout", [this]() {
        // Clear credentials, including the Plex Home master token and
        // the current-user pointer so the next login starts clean.
        // Also wipe the HTTP cache — the cached bodies came back with
        // the soon-to-be-revoked token in their URLs and would just be
        // dead weight for the next sign-in.
        HttpCache::clear();
        PlexClient::getInstance().logout();
        Application::getInstance().setAuthToken("");
        Application::getInstance().setMasterAuthToken("");
        Application::getInstance().setCurrentHomeUserUuid("");
        Application::getInstance().setCurrentHomeUserTitle("");
        Application::getInstance().setServerUrl("");
        Application::getInstance().setUsername("");
        Application::getInstance().saveSettings();

        // Go back to login
        Application::getInstance().pushLoginActivity();
    });

    dialog->open();
}

void SettingsTab::onThemeChanged(int index) {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();

    settings.theme = static_cast<AppTheme>(index);
    app.applyTheme();
    app.saveSettings();
}

void SettingsTab::onQualityChanged(int index) {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();

    settings.videoQuality = static_cast<VideoQuality>(index);

    // Update bitrate based on quality
    switch (settings.videoQuality) {
        case VideoQuality::ORIGINAL:
            settings.maxBitrate = 0;  // No limit
            break;
        case VideoQuality::QUALITY_1080P:
            settings.maxBitrate = 20000;
            break;
        case VideoQuality::QUALITY_720P:
            settings.maxBitrate = 4000;
            break;
        case VideoQuality::QUALITY_480P:
            settings.maxBitrate = 2000;
            break;
        case VideoQuality::QUALITY_360P:
            settings.maxBitrate = 1000;
            break;
        case VideoQuality::QUALITY_240P:
            settings.maxBitrate = 500;
            break;
    }

    app.saveSettings();
}

void SettingsTab::onSubtitleSizeChanged(int index) {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();

    settings.subtitleSize = static_cast<SubtitleSize>(index);
    app.saveSettings();
}

void SettingsTab::onConnectionTimeoutChanged(int index) {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();

    switch (index) {
        case 0: settings.connectionTimeout = 30; break;
        case 1: settings.connectionTimeout = 60; break;
        case 2: settings.connectionTimeout = 120; break;
        case 3: settings.connectionTimeout = 180; break;
        case 4: settings.connectionTimeout = 300; break;
    }

    app.saveSettings();
}

void SettingsTab::onSeekIntervalChanged(int index) {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();

    switch (index) {
        case 0: settings.seekInterval = 5; break;
        case 1: settings.seekInterval = 10; break;
        case 2: settings.seekInterval = 15; break;
        case 3: settings.seekInterval = 30; break;
        case 4: settings.seekInterval = 60; break;
    }

    app.saveSettings();
}

void SettingsTab::onControlsAutoHideChanged(int index) {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();

    switch (index) {
        case 0: settings.controlsAutoHideSeconds = 0; break;
        case 1: settings.controlsAutoHideSeconds = 3; break;
        case 2: settings.controlsAutoHideSeconds = 5; break;
        case 3: settings.controlsAutoHideSeconds = 10; break;
        case 4: settings.controlsAutoHideSeconds = 15; break;
    }

    app.saveSettings();
}

void SettingsTab::onManageHiddenLibraries() {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();

    auto csvToSet = [](const std::string& csv) {
        std::set<std::string> out;
        std::string s = csv;
        size_t pos;
        while ((pos = s.find(',')) != std::string::npos) {
            std::string t = s.substr(0, pos);
            if (!t.empty()) out.insert(t);
            s.erase(0, pos + 1);
        }
        if (!s.empty()) out.insert(s);
        return out;
    };

    // Fetch library sections.
    std::vector<LibrarySection> sections;
    PlexClient::getInstance().fetchLibrarySections(sections);

    // Committing rewrites hiddenLibraries from the dialog's selection, so every
    // library must be present for unselected ones to read as visible. If the
    // fetch came back empty (offline), bail rather than risk wiping the saved
    // hidden set — matches the prior behavior.
    if (sections.empty()) {
        brls::Dialog* dialog = new brls::Dialog("No libraries found");
        dialog->addButton("OK", []() {});
        dialog->open();
        return;
    }

    std::set<std::string> hiddenLibs  = csvToSet(settings.hiddenLibraries);
    std::set<std::string> hiddenItems = csvToSet(settings.hiddenSidebarItems);

    // One toggle row per library, then the hideable built-ins (Live TV — only
    // if the server has it — and Downloads). "selected" means hidden. Libraries
    // store their hidden state in hiddenLibraries; built-ins in
    // hiddenSidebarItems. Search is intentionally omitted — it's always shown.
    std::vector<MultiSelectItem> items;
    items.reserve(sections.size() + 2);
    for (const auto& section : sections) {
        MultiSelectItem it;
        it.key      = section.key;
        it.label    = section.title;
        it.selected = (hiddenLibs.find(section.key) != hiddenLibs.end());
        items.push_back(it);
    }
    if (PlexClient::getInstance().hasLiveTV()) {
        items.push_back({ "livetv", "Live TV",
                          hiddenItems.find("livetv") != hiddenItems.end() });
    }
    items.push_back({ "downloads", "Downloads",
                      hiddenItems.find("downloads") != hiddenItems.end() });

    // Present the filter-styled multi-toggle dialog (Hidden / Visible per row).
    MediaDetailView::showMultiToggleDialog(
        "Hidden Items",
        "Hidden libraries, Live TV, and Downloads are removed from the sidebar and Home.",
        "Hidden", "Visible",
        items,
        [this](const std::vector<std::string>& hiddenSelected) {
            Application& app = Application::getInstance();
            AppSettings& settings = app.getSettings();

            // Partition the hidden keys: built-in ids → hiddenSidebarItems,
            // everything else is a library key → hiddenLibraries. (Rewriting
            // hiddenSidebarItems from scratch also drops any stale "search".)
            std::string newLibs, newItems;
            for (const auto& k : hiddenSelected) {
                bool builtin = (k == "livetv" || k == "downloads");
                std::string& dst = builtin ? newItems : newLibs;
                if (!dst.empty()) dst += ",";
                dst += k;
            }
            settings.hiddenLibraries    = newLibs;
            settings.hiddenSidebarItems = newItems;
            app.saveSettings();

            if (m_hiddenLibrariesCell) {
                int count = static_cast<int>(hiddenSelected.size());
                m_hiddenLibrariesCell->setDetailText(
                    count > 0 ? std::to_string(count) + " hidden" : "None hidden");
            }
        });
}

void SettingsTab::onManageDefaultDvrLibrary() {
    AppSettings& settings = Application::getInstance().getSettings();

    std::vector<LibrarySection> sections;
    PlexClient::getInstance().fetchLibrarySections(sections);

    // DVR can only target Movies / TV Shows libraries. Filter the rest
    // out so the user can't pick (e.g.) a music library that the
    // server will reject when we POST /media/subscriptions.
    std::vector<LibrarySection> eligible;
    for (const auto& s : sections) {
        if (s.type == "movie" || s.type == "show") eligible.push_back(s);
    }

    if (eligible.empty()) {
        brls::Dialog* dialog = new brls::Dialog(
            "No Movies or TV Shows libraries were found on this server.");
        dialog->addButton("OK", []() {});
        dialog->open();
        return;
    }

    // Option 0 is always "Server default" so the user can revert to the
    // template's recommendation without nuking the settings file.
    std::vector<std::string> options;
    options.reserve(eligible.size() + 1);
    options.push_back("Server default");
    for (const auto& s : eligible) {
        options.push_back(s.title + "  (" + s.type + ")");
    }

    int selected = 0;
    for (size_t i = 0; i < eligible.size(); i++) {
        if (eligible[i].key == settings.defaultDvrSectionId) {
            selected = static_cast<int>(i) + 1;
            break;
        }
    }

    auto* dropdown = new brls::Dropdown(
        "Default DVR Library", options,
        [this, eligible](int picked) {
            AppSettings& s = Application::getInstance().getSettings();
            if (picked <= 0 || picked > static_cast<int>(eligible.size())) {
                s.defaultDvrSectionId.clear();
                s.defaultDvrSectionTitle.clear();
            } else {
                const auto& sec = eligible[picked - 1];
                s.defaultDvrSectionId    = sec.key;
                s.defaultDvrSectionTitle = sec.title;
            }
            Application::getInstance().saveSettings();
            if (m_defaultDvrLibraryCell) {
                m_defaultDvrLibraryCell->setDetailText(
                    s.defaultDvrSectionId.empty()
                        ? "Server default"
                        : s.defaultDvrSectionTitle);
            }
        },
        selected);
    brls::Application::pushActivity(new brls::Activity(dropdown));
}

void SettingsTab::onNetworkTest() {
    // Show a toast while tests run
    brls::Application::notify("Running network test...");

    // Run the network tests on a detached thread to avoid blocking the
    // UI. Goes through platform::launchThread() rather than std::thread
    // so the Switch newlib std::thread shim doesn't ship a thread with
    // an unregistered stack region and zeroed TLS (caught a real crash
    // on hbloader — Atmosphère Instruction Abort at a page-aligned PC).
    platform::launchThread([this]() {
        // ── 1. WiFi Check ──
        std::string ipAddress = "-";
        std::string dnsInfo = "-";
        std::string signalStr = "-";
        std::string ssid = "-";
        bool wifiConnected = false;

#ifdef __vita__
        SceNetCtlInfo info;

        int ret = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info);
        if (ret >= 0) {
            ipAddress = std::string(info.ip_address);
            wifiConnected = true;
        }

        ret = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_SSID, &info);
        if (ret >= 0) {
            ssid = std::string(info.ssid);
        }

        ret = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_RSSI_PERCENTAGE, &info);
        if (ret >= 0) {
            signalStr = std::to_string(info.rssi_percentage) + "%";
        }

        ret = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_PRIMARY_DNS, &info);
        if (ret >= 0) {
            dnsInfo = std::string(info.primary_dns);
            ret = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_SECONDARY_DNS, &info);
            if (ret >= 0) {
                dnsInfo += " / " + std::string(info.secondary_dns);
            }
        }
#elif defined(_WIN32)
        // Adapter enumeration — pick the first up, non-loopback IPv4
        // address. GetAdaptersAddresses needs a sizing pass first
        // (NO_OVERFLOW returns the required buffer size in outLen).
        ULONG outLen = 0;
        GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                             nullptr, nullptr, &outLen);
        if (outLen > 0) {
            std::vector<unsigned char> buf(outLen);
            auto* head = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
            if (GetAdaptersAddresses(AF_INET,
                                     GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                     nullptr, head, &outLen) == NO_ERROR) {
                for (auto* a = head; a; a = a->Next) {
                    if (a->OperStatus != IfOperStatusUp) continue;
                    if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
                    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                        auto* sa = u->Address.lpSockaddr;
                        if (!sa || sa->sa_family != AF_INET) continue;
                        char addrBuf[INET_ADDRSTRLEN] = {0};
                        auto* sin = reinterpret_cast<sockaddr_in*>(sa);
                        if (InetNtopA(AF_INET, &sin->sin_addr, addrBuf, sizeof(addrBuf))) {
                            ipAddress = addrBuf;
                            // a->FriendlyName is UTF-16; convert lossily.
                            if (a->FriendlyName) {
                                char nameBuf[128] = {0};
                                WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1,
                                                    nameBuf, sizeof(nameBuf), nullptr, nullptr);
                                ssid = nameBuf;
                            }
                            wifiConnected = true;
                        }
                        if (wifiConnected) break;
                    }
                    if (wifiConnected) break;
                }
            }
        }
        // DNS — GetNetworkParams returns the OS-wide resolver list.
        ULONG dnsLen = 0;
        GetNetworkParams(nullptr, &dnsLen);
        if (dnsLen > 0) {
            std::vector<unsigned char> dnsBuf(dnsLen);
            auto* params = reinterpret_cast<FIXED_INFO*>(dnsBuf.data());
            if (GetNetworkParams(params, &dnsLen) == NO_ERROR) {
                std::string out;
                for (auto* s = &params->DnsServerList; s; s = s->Next) {
                    if (s->IpAddress.String[0] == '\0') continue;
                    if (!out.empty()) out += " / ";
                    out += s->IpAddress.String;
                }
                if (!out.empty()) dnsInfo = out;
            }
        }
        // No portable wireless RSSI without WlanGetNetworkBssList; leave
        // signal blank so the dialog renders it as "-" rather than lying.
#elif defined(__SWITCH__)
        // libnx nifm. nifmInitialize must succeed before any of the
        // accessor calls; tear it down with nifmExit when done so we
        // don't keep the system service handle open for the rest of
        // the session.
        if (R_SUCCEEDED(nifmInitialize(NifmServiceType_User))) {
            u32 ipAddrBE = 0;
            if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ipAddrBE)) && ipAddrBE != 0) {
                // IP comes back in network (big-endian) byte order. Format
                // by hand instead of dragging in inet_ntop, which on libnx
                // would require setting up the bsd: service first.
                char buf[16];
                snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                         (unsigned)((ipAddrBE >>  0) & 0xff),
                         (unsigned)((ipAddrBE >>  8) & 0xff),
                         (unsigned)((ipAddrBE >> 16) & 0xff),
                         (unsigned)((ipAddrBE >> 24) & 0xff));
                ipAddress = buf;
                wifiConnected = true;
            }
            bool wireless = false;
            if (R_SUCCEEDED(nifmIsWirelessCommunicationEnabled(&wireless))) {
                ssid = wireless ? "Wireless" : "Wired";
            }
            nifmExit();
        }
        // DNS isn't queryable through the basic nifm API without the
        // full network-profile struct; leave it as "-" rather than lie.
#else
        // POSIX. Open a UDP socket, "connect" it to a public address
        // (no packets are actually sent — UDP connect just primes the
        // routing decision), then ask the kernel which local IPv4 it
        // chose via getsockname. This sidesteps getifaddrs entirely,
        // which is necessary on Android < API 24 where the function
        // isn't exposed even though <ifaddrs.h> is.
        int probe = socket(AF_INET, SOCK_DGRAM, 0);
        if (probe >= 0) {
            sockaddr_in target = {};
            target.sin_family = AF_INET;
            target.sin_port = htons(53);  // DNS port, just so the route makes sense
            inet_pton(AF_INET, "8.8.8.8", &target.sin_addr);
            if (connect(probe, reinterpret_cast<sockaddr*>(&target),
                        sizeof(target)) == 0) {
                sockaddr_in local = {};
                socklen_t locLen = sizeof(local);
                if (getsockname(probe, reinterpret_cast<sockaddr*>(&local),
                                &locLen) == 0) {
                    char addrBuf[INET_ADDRSTRLEN] = {0};
                    if (inet_ntop(AF_INET, &local.sin_addr,
                                  addrBuf, sizeof(addrBuf))) {
                        ipAddress = addrBuf;
                        wifiConnected = true;
                    }
                }
            }
            close(probe);
        }
        // The interface name isn't trivially recoverable from the
        // local IP alone without enumerating adapters; leave the
        // Interface row blank on POSIX rather than guessing.
        ssid = "-";

        // /etc/resolv.conf is the lowest-common-denominator DNS source
        // on Linux / macOS / iOS / tvOS. Android stopped populating it
        // around 8.0 (resolution moved into netd), so this branch is a
        // best-effort lookup that quietly no-ops where the file is
        // empty or missing.
        std::ifstream resolv("/etc/resolv.conf");
        if (resolv.is_open()) {
            std::string line, primary, secondary;
            while (std::getline(resolv, line)) {
                if (line.compare(0, 11, "nameserver ") != 0) continue;
                std::string addr = line.substr(11);
                while (!addr.empty() &&
                       (addr.back() == ' ' || addr.back() == '\t' ||
                        addr.back() == '\r' || addr.back() == '\n')) {
                    addr.pop_back();
                }
                if (addr.empty()) continue;
                if (primary.empty()) primary = addr;
                else if (secondary.empty()) { secondary = addr; break; }
            }
            if (!primary.empty()) {
                dnsInfo = primary;
                if (!secondary.empty()) dnsInfo += " / " + secondary;
            }
        }
        // No portable RSSI / SSID query — leave signal as "-".
#endif

        // ── 2. Internet Check (latency) ──
        std::string internetStatus = "Skipped (no WiFi)";
        if (wifiConnected) {
            HttpClient netClient;
            netClient.setTimeout(10);

            auto start = std::chrono::steady_clock::now();
            std::string response;
            bool ok = netClient.get("http://connectivitycheck.gstatic.com/generate_204", response);
            auto end = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

            if (ok) {
                internetStatus = "Reachable (" + std::to_string(ms) + "ms)";
            } else {
                internetStatus = "Unreachable (" + std::to_string(ms) + "ms)";
            }
        }

        // ── 3. Plex Server Check (latency) ──
        Application& app = Application::getInstance();
        std::string serverUrl = app.getServerUrl();
        std::string plexStatus;
        std::string plexLatency = "-";

        if (serverUrl.empty()) {
            plexStatus = "Not configured";
        } else if (!wifiConnected) {
            plexStatus = "Skipped (no WiFi)";
        } else {
            HttpClient plexClient;
            plexClient.setTimeout(10);
            plexClient.setDefaultHeader("X-Plex-Token", app.getAuthToken());
            plexClient.setDefaultHeader("Accept", "application/json");

            auto start = std::chrono::steady_clock::now();
            std::string response;
            bool ok = plexClient.get(serverUrl + "/identity", response);
            auto end = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

            plexLatency = std::to_string(ms) + "ms";
            if (ok) {
                plexStatus = "Connected (" + std::to_string(ms) + "ms)";
            } else {
                plexStatus = "Failed (" + std::to_string(ms) + "ms)";
            }
        }

        // ── Build dialog on main thread ──
        // Capture results by value for the lambda
        brls::sync([=]() {
            brls::Box* content = new brls::Box();
            content->setAxis(brls::Axis::COLUMN);
            content->setWidth(700);
            content->setHeight(420);
            content->setPadding(25);

            auto* titleLabel = new brls::Label();
            titleLabel->setText("Network Test Results");
            titleLabel->setFontSize(22);
            titleLabel->setMarginBottom(15);
            content->addView(titleLabel);

            // Helper to create info rows (item #11 style)
            auto addRow = [&content](const std::string& label, const std::string& value) {
                auto* row = new brls::Box();
                row->setAxis(brls::Axis::ROW);
                row->setMarginBottom(8);
                auto* lblA = new brls::Label();
                lblA->setText(label);
                lblA->setFontSize(16);
                lblA->setWidth(220);
                row->addView(lblA);
                auto* lblB = new brls::Label();
                lblB->setText(value);
                lblB->setFontSize(16);
                row->addView(lblB);
                content->addView(row);
            };

            // Helper for section headers
            auto addHeader = [&content](const std::string& text) {
                auto* lbl = new brls::Label();
                lbl->setText(text);
                lbl->setFontSize(16);
                lbl->setMarginBottom(6);
                lbl->setMarginTop(4);
                content->addView(lbl);
            };

            // Connection section. Header says "Connection" (not "WiFi")
            // because most non-Vita platforms may be on Ethernet.
            // "Interface" replaces "Network" so the row makes sense
            // whether the value is an SSID (Vita) or an adapter name
            // (eth0 / en0 / "Wi-Fi 2"). Signal only shows when the
            // platform actually reported one — POSIX/Win don't.
            addHeader("-- Connection --");
            addRow("Status:", wifiConnected ? "Connected" : "Not Connected");
            addRow("Interface:", ssid);
            addRow("IP Address:", ipAddress);
            addRow("DNS:", dnsInfo);
            if (signalStr != "-") addRow("Signal:", signalStr);

            // Internet section
            addHeader("-- Internet --");
            addRow("Connectivity:", internetStatus);

            // Plex server section
            addHeader("-- Plex Server --");
            addRow("Server:", serverUrl.empty() ? "Not configured" : serverUrl);
            addRow("Connection:", plexStatus);

            auto* dialog = new brls::Dialog(content);
            dialog->addButton("Close", []() {});
            dialog->open();
        });
    });
}

void SettingsTab::onTestLocalPlayback() {
    brls::Logger::info("SettingsTab: Testing local playback...");

    // Look for the first existing test file under the platform's data
    // directory (ux0:data/VitaPlex/ on Vita, sdmc:/VitaPlex/ on Switch,
    // ~/.local/share/VitaPlex/ on desktop Linux, the SDL internal
    // storage path on Android, etc.) — platformPath() resolves it.
    std::string testFile;
    const std::vector<std::string> testFiles = {
        platformPath("test.mp4"),
        platformPath("test.mp3"),
        platformPath("test.ogg"),
        platformPath("test.wav"),
    };

    for (const auto& file : testFiles) {
        FILE* f = fopen(file.c_str(), "rb");
        if (f) {
            fclose(f);
            testFile = file;
            brls::Logger::info("SettingsTab: Found test file: {}", testFile);
            break;
        }
    }

    if (testFile.empty()) {
        std::string dataDir = platformPath("");
        if (!dataDir.empty() && dataDir.back() == '/') dataDir.pop_back();
        brls::Application::notify("No test file found in " + dataDir);
        brls::Logger::error("SettingsTab: No test file found under {}", dataDir);
        return;
    }

    // Push player activity with the test file (this shows the video view properly)
    brls::Logger::info("SettingsTab: Pushing player activity for: {}", testFile);
    PlayerActivity* activity = PlayerActivity::createForDirectFile(testFile);
    brls::Application::pushActivity(activity);
}

void SettingsTab::onSyncLoungeConnect() {
    auto& sl = SyncLoungeSession::instance();

    // Already connected -> this acts as a disconnect.
    if (sl.isConnected()) {
        sl.disconnect();
        if (m_syncLoungeSyncCell) m_syncLoungeSyncCell->setDetailText("Disconnected");
        brls::Application::notify("SyncLounge disconnected");
        return;
    }

    auto* ime = brls::Application::getImeManager();
    if (!ime) {
        brls::Application::notify("No on-screen keyboard available");
        return;
    }

    // Prefill from the saved settings so the user doesn't retype them.
    AppSettings& settings = Application::getInstance().getSettings();
    const std::string curServer = settings.syncLoungeServer.empty()
        ? std::string("https://server.synclounge.tv")
        : settings.syncLoungeServer;
    const std::string curRoom = settings.syncLoungeRoom;

    auto trim = [](std::string s) {
        while (!s.empty() && (s.back() == ' '  || s.back() == '\t' ||
                              s.back() == '\r' || s.back() == '\n')) s.pop_back();
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
        return s.substr(i);
    };

    // Prompt for the server URL, then the room, then connect the persistent
    // session and save both for next time. Once connected, any active player
    // follows the room host.
    ime->openForText([this, ime, trim, curRoom](std::string server) {
        server = trim(server);
        if (server.empty()) return;

        ime->openForText([this, trim, server](std::string room) {
            room = trim(room);
            if (room.empty()) {
                brls::Application::notify("Room name is required");
                return;
            }

            Application& app = Application::getInstance();
            app.getSettings().syncLoungeServer = server;
            app.getSettings().syncLoungeRoom   = room;
            app.saveSettings();

            SyncLoungeSession::instance().connect(server, room, app.getUsername(), nullptr);
            if (m_syncLoungeSyncCell)
                m_syncLoungeSyncCell->setDetailText("Connected: " + room);
            brls::Application::notify("SyncLounge: connecting to \"" + room + "\"");
        }, "SyncLounge room name / code", "", 64, curRoom);
    }, "SyncLounge server URL", "", 128, curServer);
}

void SettingsTab::onSyncLoungeMembers() {
    auto& sl = SyncLoungeSession::instance();
    if (!sl.isConnected()) {
        brls::Application::notify("Connect to SyncLounge first");
        return;
    }
    const auto members = sl.members();
    if (members.empty()) {
        brls::Application::notify("No members in the room yet");
        return;
    }
    const bool weAreHost = sl.isHost();

    // One row per member, styled like the join-session prompt. The host row is
    // gold (primary) with a check icon; everyone else is a plain person row.
    // When we're host, tapping another member hands them host.
    std::vector<OptionRow> rows;
    for (const auto& m : members) {
        const std::string id     = m.id;
        const bool        isSelf = m.isSelf;
        const bool        isHost = m.isHost;
        std::string label = m.username + (isSelf ? " (you)" : "");
        std::string sub   = isHost ? "Host" : (weAreHost && !isSelf ? "Make host" : "");
        rows.push_back({
            isHost ? "check-circle.png" : "account.png",
            label, sub,
            /*primary=*/isHost,
            /*danger=*/false,
            [id, isSelf, isHost, weAreHost](brls::View*) {
                if (!weAreHost) {
                    brls::Application::notify("Only the host can change host");
                    return true;
                }
                if (isSelf || isHost) return true;  // it's us / already the host
                SyncLoungeSession::instance().transferHost(id);
                brls::Application::notify("Host transferred");
                return true;
            }});
    }
    rows.push_back({ "cross.png", "Close", "", false, true,
                     [](brls::View*) { return true; }});

    const std::string subtitle = weAreHost
        ? "Tap a member to make them host"
        : ("Room \"" + sl.room() + "\"");
    MediaDetailView::showCenteredChoice("Party members", subtitle, std::move(rows));
}

} // namespace vitaplex
