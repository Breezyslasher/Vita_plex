/**
 * VitaPlex - Downloads Tab Implementation
 * Queue display with start/stop/pause controls and auto-refresh progress.
 * Groups downloads by playlist/album/artist with cover art and context menus.
 *
 * Based on Vita_Suwayomi's downloads tab patterns:
 * - Start/Stop/Pause/Resume/Clear action buttons
 * - Auto-refresh with smart rebuild detection
 * - Per-item cancel/delete actions
 * - Color-coded status display
 * - Grouped view for playlists, albums, artists
 * - Cover art / poster thumbnails
 * - START button context menu (Play Now, Add to Queue, Clear Queue)
 */

#include "view/downloads_tab.hpp"
#include "app/downloads_manager.hpp"
#include "app/application.hpp"
#include "app/plex_client.hpp"
#include "app/music_queue.hpp"
#include "activity/player_activity.hpp"
#include "utils/async.hpp"
#include "utils/image_loader.hpp"
#include "platform/platform.hpp"

#include <memory>
#include <thread>
#include <chrono>
#include <set>
#include <map>

namespace vitaplex {

// Format elapsed seconds as a human-readable string like "1m 23s" or "45s"
static std::string formatElapsedTime(int totalSeconds) {
    if (totalSeconds < 60) {
        return std::to_string(totalSeconds) + "s";
    }
    int minutes = totalSeconds / 60;
    int seconds = totalSeconds % 60;
    return std::to_string(minutes) + "m " + std::to_string(seconds) + "s";
}

// Build a transcoding status string with elapsed time and animated dots
static std::string buildTranscodeStatus(int elapsedSeconds) {
    int dotCount = (elapsedSeconds % 3) + 1;
    std::string dots(dotCount, '.');

    std::string status = "Transcoding on server" + dots;
    if (elapsedSeconds > 0) {
        status += " (" + formatElapsedTime(elapsedSeconds) + " elapsed)";
    }
    return status;
}

static std::string buildItemStatusText(const DownloadItem& item) {
    switch (item.state) {
        case DownloadState::QUEUED:
            return "Queued";
        case DownloadState::TRANSCODING:
            return buildTranscodeStatus(item.transcodeElapsedSeconds);
        case DownloadState::DOWNLOADING:
            if (item.totalBytes > 0) {
                int percent = (int)((item.downloadedBytes * 100) / item.totalBytes);
                int64_t dlMB = item.downloadedBytes / (1024 * 1024);
                int64_t totalMB = item.totalBytes / (1024 * 1024);
                return "Downloading... " + std::to_string(percent) + "% (" +
                       std::to_string(dlMB) + "/" + std::to_string(totalMB) + " MB)";
            } else {
                int64_t dlMB = item.downloadedBytes / (1024 * 1024);
                return "Downloading... " + std::to_string(dlMB) + " MB";
            }
        case DownloadState::PAUSED:
            if (item.totalBytes > 0) {
                int percent = (int)((item.downloadedBytes * 100) / item.totalBytes);
                return "Paused (" + std::to_string(percent) + "%)";
            }
            return "Paused";
        case DownloadState::COMPLETED:
            if (item.viewOffset > 0) {
                int minutes = (int)(item.viewOffset / 60000);
                return "Ready to play (" + std::to_string(minutes) + " min watched)";
            }
            return "Ready to play";
        case DownloadState::FAILED:
            return "Failed";
        default:
            return "";
    }
}

static bool isDescendantOf(brls::View* view, brls::View* ancestor) {
    if (!view || !ancestor) return false;
    brls::View* current = view;
    while (current) {
        if (current == ancestor) return true;
        current = current->getParent();
    }
    return false;
}

static NVGcolor getStateColor(DownloadState state) {
    switch (state) {
        case DownloadState::TRANSCODING: return nvgRGBA(50, 30, 60, 200);
        case DownloadState::DOWNLOADING: return nvgRGBA(20, 60, 20, 200);
        case DownloadState::PAUSED:      return nvgRGBA(60, 50, 10, 200);
        case DownloadState::FAILED:      return nvgRGBA(60, 20, 20, 200);
        case DownloadState::COMPLETED:   return nvgRGBA(20, 40, 60, 200);
        default:                         return nvgRGBA(40, 40, 40, 200);
    }
}

// Group state transitions by the action-button set the row exposes. When
// the category changes we need a structural rebuild of that row to swap
// "Cancel" for "Play + Delete" etc.; when only the category-internal
// state changes (e.g. TRANSCODING -> DOWNLOADING) we can recolor +
// retext in place and skip the rebuild entirely. Reduces refresh churn
// from "every progress tick" to "only on real boundary crossings".
static int buttonCategory(DownloadState s) {
    switch (s) {
        case DownloadState::QUEUED:
        case DownloadState::TRANSCODING:
        case DownloadState::DOWNLOADING:
            return 0; // shows "Cancel"
        case DownloadState::PAUSED:
        case DownloadState::FAILED:
            return 1; // shows "Remove"
        case DownloadState::COMPLETED:
            return 2; // shows "Play" + "Delete"
        default:
            return 3;
    }
}

// ── D8 palette (literal design tokens) ─────────────────────────────
namespace {
const NVGcolor kSurface  = nvgRGB(0x38, 0x38, 0x38);  // cards / rows
const NVGcolor kSurface3 = nvgRGB(0x49, 0x49, 0x49);  // chips / 2ndary buttons / badge / meter track
const NVGcolor kLine     = nvgRGB(0x47, 0x47, 0x47);  // tab-bar baseline
const NVGcolor kText     = nvgRGB(0xFF, 0xFF, 0xFF);
const NVGcolor kMuted    = nvgRGB(0xA8, 0xA6, 0xB4);
const NVGcolor kGold     = nvgRGB(0xE5, 0xA0, 0x0D);  // accent / active underline + badge
const NVGcolor kGoldInk  = nvgRGB(0x24, 0x1C, 0x08);  // text on gold (never white)

// Bright per-state accent for the 4px left strip.
const NVGcolor kStDownloading = nvgRGB(0x3E, 0xCF, 0x8E);
const NVGcolor kStTranscoding = nvgRGB(0x9A, 0x6C, 0xFF);
const NVGcolor kStQueued      = nvgRGB(0x80, 0x7E, 0x8C);
const NVGcolor kStPaused      = nvgRGB(0xE5, 0xA0, 0x0D);
const NVGcolor kStReady       = nvgRGB(0x89, 0xF1, 0xF2);
const NVGcolor kStFailed      = nvgRGB(0xFF, 0x56, 0x58);

// Map a download item to its sub-tab bucket. Episodes (or anything in a
// SHOW group) are Shows; tracks / playlist / album / artist groups are
// Music; everything else (movies + unknown video) is Movies.
DownloadsTab::Type itemType(const DownloadItem& it) {
    if (it.mediaType == "track" ||
        it.groupType == DownloadGroupType::PLAYLIST ||
        it.groupType == DownloadGroupType::ALBUM ||
        it.groupType == DownloadGroupType::ARTIST)
        return DownloadsTab::Type::MUSIC;
    if (it.mediaType == "episode" || it.groupType == DownloadGroupType::SHOW)
        return DownloadsTab::Type::SHOWS;
    return DownloadsTab::Type::MOVIES;
}

NVGcolor stateStripColor(DownloadState s) {
    switch (s) {
        case DownloadState::DOWNLOADING: return kStDownloading;
        case DownloadState::TRANSCODING: return kStTranscoding;
        case DownloadState::PAUSED:      return kStPaused;
        case DownloadState::COMPLETED:   return kStReady;
        case DownloadState::FAILED:      return kStFailed;
        case DownloadState::QUEUED:
        default:                         return kStQueued;
    }
}

// Strip colour for a group row, mirroring the group background logic.
NVGcolor groupStripColor(int completed, int displayTotal, int downloading) {
    if (displayTotal > 0 && completed == displayTotal) return kStReady;       // done
    if (downloading > 0)                               return kStDownloading; // active
    return kStQueued;                                                         // queued
}

// Human-readable byte size, e.g. "14.2 GB", "640 MB", "0 B".
std::string formatBytes(int64_t bytes) {
    if (bytes <= 0) return "0 B";
    double gb = (double)bytes / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 1.0) {
        char buf[32]; snprintf(buf, sizeof(buf), "%.1f GB", gb); return buf;
    }
    double mb = (double)bytes / (1024.0 * 1024.0);
    if (mb >= 1.0) {
        char buf[32]; snprintf(buf, sizeof(buf), "%.0f MB", mb); return buf;
    }
    char buf[32]; snprintf(buf, sizeof(buf), "%.0f KB", (double)bytes / 1024.0); return buf;
}

// Restyle one of the existing toolbar / per-row buttons to the palette
// without disturbing its registered click action. Primary = gold fill +
// ink label; secondary = neutral surface-3 + white label. Focus is the
// borealis warm halo (the dark highlight fill is hidden), never a gold
// fill. The button keeps its child Label, so we colour that label rather
// than calling Button::setTextColor (which would re-run applyStyle and
// clobber the background we set here).
void styleToolbarButton(brls::Button* btn, brls::Label* label, bool primary) {
    if (!btn) return;
    btn->setCornerRadius(10);
    btn->setBorderThickness(0);
    btn->setHideHighlightBackground(true);
    btn->setHighlightCornerRadius(10);
    btn->setBackgroundColor(primary ? kGold : kSurface3);
    if (label) label->setTextColor(primary ? kGoldInk : kText);
}
}  // namespace

DownloadsTab::DownloadsTab()
    : m_alive(std::make_shared<bool>(true))
    , m_aliveAtomic(std::make_shared<std::atomic<bool>>(true))
{
    this->setAxis(brls::Axis::COLUMN);
    this->setPadding(20);
    this->setGrow(1.0f);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);

    // ── Header: page title + offline-storage readout ──
    m_headerRow = new brls::Box();
    m_headerRow->setAxis(brls::Axis::ROW);
    m_headerRow->setAlignItems(brls::AlignItems::CENTER);
    m_headerRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    m_headerRow->setMargins(0, 0, 6, 0);

    m_titleLabel = new brls::Label();
    m_titleLabel->setText("Downloads");
    m_titleLabel->setFontSize(28);
    m_titleLabel->setTextColor(kText);
    m_headerRow->addView(m_titleLabel);

    m_storageBox = new brls::Box();
    m_storageBox->setAxis(brls::Axis::COLUMN);
    m_storageBox->setWidth(210);
    m_storageBox->setAlignItems(brls::AlignItems::FLEX_END);
    m_storageBox->setJustifyContent(brls::JustifyContent::CENTER);

    auto* storageText = new brls::Box();
    storageText->setAxis(brls::Axis::ROW);
    storageText->setAlignItems(brls::AlignItems::CENTER);
    m_storageUsedLabel = new brls::Label();
    m_storageUsedLabel->setFontSize(15);
    m_storageUsedLabel->setTextColor(kGold);
    m_storageUsedLabel->setText("0 B");
    storageText->addView(m_storageUsedLabel);
    m_storageTotalLabel = new brls::Label();
    m_storageTotalLabel->setFontSize(15);
    m_storageTotalLabel->setTextColor(kMuted);
    m_storageTotalLabel->setMarginLeft(5);
    m_storageTotalLabel->setText("");
    storageText->addView(m_storageTotalLabel);
    m_storageBox->addView(storageText);

    auto* meterTrack = new brls::Box();
    meterTrack->setAxis(brls::Axis::ROW);
    meterTrack->setWidth(200);
    meterTrack->setHeight(6);
    meterTrack->setCornerRadius(3);
    meterTrack->setBackgroundColor(kSurface3);
    meterTrack->setMarginTop(5);
    m_storageMeterFill = new brls::Box();
    m_storageMeterFill->setWidth(0);
    m_storageMeterFill->setHeight(6);
    m_storageMeterFill->setCornerRadius(3);
    m_storageMeterFill->setBackgroundColor(kGold);
    meterTrack->addView(m_storageMeterFill);
    m_storageBox->addView(meterTrack);

    m_headerRow->addView(m_storageBox);
    this->addView(m_headerRow);

    // ── Media-type sub-tab bar (filters the list below) ──
    m_tabBar = buildTypeTabs();
    this->addView(m_tabBar);

    // ── Action toolbar (existing actions, restyled to the palette) ──
    m_actionsRow = new brls::Box();
    m_actionsRow->setAxis(brls::Axis::ROW);
    m_actionsRow->setMargins(0, 0, 15, 0);

    // Start/Stop button (primary)
    m_startStopBtn = new brls::Button();
    m_startStopLabel = new brls::Label();
    m_startStopLabel->setText("Start");
    m_startStopLabel->setFontSize(16);
    m_startStopBtn->addView(m_startStopLabel);
    m_startStopBtn->setMargins(0, 10, 0, 0);
    m_startStopBtn->registerClickAction([this](brls::View*) {
        auto& mgr = DownloadsManager::getInstance();
        if (mgr.isDownloading()) {
            mgr.pauseDownloads();
            m_startStopLabel->setText("Start");
            brls::Application::notify("Downloads paused");
        } else {
            mgr.startDownloads();
            m_startStopLabel->setText("Stop");
            brls::Application::notify("Downloads started");
        }
        return true;
    });
    styleToolbarButton(m_startStopBtn, m_startStopLabel, true);
    m_actionsRow->addView(m_startStopBtn);

    // Resume button
    m_resumeBtn = new brls::Button();
    auto resumeLabel = new brls::Label();
    resumeLabel->setText("Resume All");
    resumeLabel->setFontSize(16);
    m_resumeBtn->addView(resumeLabel);
    m_resumeBtn->setMargins(0, 10, 0, 0);
    m_resumeBtn->registerClickAction([this](brls::View*) {
        auto& mgr = DownloadsManager::getInstance();
        int count = mgr.countIncompleteDownloads();
        if (count > 0) {
            mgr.resumeIncompleteDownloads();
            m_startStopLabel->setText("Stop");
            brls::Application::notify("Resuming " + std::to_string(count) + " downloads");
        } else {
            brls::Application::notify("No incomplete downloads");
        }
        return true;
    });
    styleToolbarButton(m_resumeBtn, resumeLabel, false);
    m_actionsRow->addView(m_resumeBtn);

    // Sync button
    m_syncBtn = new brls::Button();
    auto syncBtn = m_syncBtn;
    auto syncLabel = new brls::Label();
    syncLabel->setText("Sync");
    syncLabel->setFontSize(16);
    syncBtn->addView(syncLabel);
    syncBtn->setMargins(0, 10, 0, 0);
    syncBtn->registerClickAction([](brls::View*) {
        DownloadsManager::getInstance().syncProgressBidirectional();
        brls::Application::notify("Progress synced");
        return true;
    });
    styleToolbarButton(syncBtn, syncLabel, false);
    m_actionsRow->addView(syncBtn);

    // Clear completed button
    m_clearBtn = new brls::Button();
    auto clearLabel = new brls::Label();
    clearLabel->setText("Clear Done");
    clearLabel->setFontSize(16);
    m_clearBtn->addView(clearLabel);
    m_clearBtn->setMargins(0, 10, 0, 0);
    m_clearBtn->registerClickAction([this](brls::View*) {
        // Debounce: ignore rapid double-taps (< 500ms apart)
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastClearTime).count();
        if (elapsed < 500) return true;
        m_lastClearTime = now;

        DownloadsManager::getInstance().clearCompleted();
        brls::Application::notify("Cleared completed downloads");
        m_lastState.clear();
        rebuildList();
        return true;
    });
    styleToolbarButton(m_clearBtn, clearLabel, false);
    m_actionsRow->addView(m_clearBtn);

    this->addView(m_actionsRow);

    // Scrollable list container
    m_scrollView = new brls::ScrollingFrame();
    m_scrollView->setGrow(1.0f);

    m_listContainer = new brls::Box();
    m_listContainer->setAxis(brls::Axis::COLUMN);
    m_listContainer->setGrow(1.0f);

    m_scrollView->setContentView(m_listContainer);
    this->addView(m_scrollView);

    // ── Focus wiring: tabs <-> toolbar ──
    // Down from any tab drops into the toolbar; Up from any toolbar
    // button returns to the active tab (kept current in setTypeFilter).
    for (auto& T : m_typeTabs) {
        if (T.tab) T.tab->setCustomNavigationRoute(brls::FocusDirection::DOWN, m_startStopBtn);
    }
    if (m_typeTabs[0].tab) {
        m_startStopBtn->setCustomNavigationRoute(brls::FocusDirection::UP, m_typeTabs[0].tab);
        m_resumeBtn->setCustomNavigationRoute(brls::FocusDirection::UP, m_typeTabs[0].tab);
        m_syncBtn->setCustomNavigationRoute(brls::FocusDirection::UP, m_typeTabs[0].tab);
        m_clearBtn->setCustomNavigationRoute(brls::FocusDirection::UP, m_typeTabs[0].tab);
    }

    applyTabVisuals();
    applyResponsiveLayout();
    // Re-flow the header on rotation. Guarded by the alive flag because
    // the orientation subscription outlives this view (tab can be torn
    // down and rebuilt as the user switches sidebar tabs).
    auto aliveWeak = std::weak_ptr<bool>(m_alive);
    platform::onOrientationChanged([this, aliveWeak]() {
        auto a = aliveWeak.lock();
        if (!a || !*a) return;
        applyResponsiveLayout();
    });
}

DownloadsTab::~DownloadsTab() {
    *m_alive = false;
    m_aliveAtomic->store(false);
    stopAutoRefresh();
}

brls::Box* DownloadsTab::buildTypeTabs() {
    m_activeType = Type::ALL;

    auto* bar = new brls::Box();
    bar->setAxis(brls::Axis::COLUMN);
    bar->setAlignItems(brls::AlignItems::STRETCH);
    bar->setMargins(6, 0, 10, 0);

    // Horizontal scroll so the strip stays usable when a narrow / portrait
    // viewport can't fit all four tabs.
    auto* scroll = new brls::HScrollingFrame();
    scroll->setHeight(52);  // 46 content + 3 underline + headroom for the focus ring
    scroll->setScrollingIndicatorVisible(false);

    auto* tabsRow = new brls::Box();
    tabsRow->setAxis(brls::Axis::ROW);
    tabsRow->setAlignItems(brls::AlignItems::FLEX_END);

    static const char* kIcons[4]  = { "format-list-group.png", "video-image.png", "show.png", "music.png" };
    static const char* kLabels[4] = { "All", "Movies", "Shows", "Music" };

    for (int i = 0; i < 4; i++) {
        auto* tab = new brls::Box();
        tab->setAxis(brls::Axis::COLUMN);
        tab->setAlignItems(brls::AlignItems::STRETCH);
        tab->setFocusable(true);
        tab->setHideHighlightBackground(true);   // focus = warm halo, never a fill
        tab->setHighlightCornerRadius(9);
        if (i < 3) tab->setMarginRight(6);

        auto* content = new brls::Box();
        content->setAxis(brls::Axis::ROW);
        content->setAlignItems(brls::AlignItems::CENTER);
        content->setJustifyContent(brls::JustifyContent::CENTER);
        content->setHeight(46);
        content->setPaddingLeft(18);
        content->setPaddingRight(18);

        auto* icon = new brls::Image();
        icon->setImageFromRes(std::string("icons/") + kIcons[i]);
        icon->setWidth(18);
        icon->setHeight(18);
        icon->setScalingType(brls::ImageScalingType::FIT);
        icon->setMarginRight(9);
        content->addView(icon);

        auto* label = new brls::Label();
        label->setText(kLabels[i]);
        label->setFontSize(15);
        label->setTextColor(kMuted);
        label->setMarginRight(9);
        content->addView(label);

        auto* badge = new brls::Box();
        badge->setAxis(brls::Axis::ROW);
        badge->setJustifyContent(brls::JustifyContent::CENTER);
        badge->setAlignItems(brls::AlignItems::CENTER);
        badge->setHeight(22);
        badge->setMinWidth(22);
        badge->setCornerRadius(11);
        badge->setPaddingLeft(7);
        badge->setPaddingRight(7);
        badge->setBackgroundColor(kSurface3);

        auto* count = new brls::Label();
        count->setText("0");
        count->setFontSize(12);
        count->setTextColor(kMuted);
        badge->addView(count);
        content->addView(badge);

        tab->addView(content);

        // Per-tab gold underline; visibility toggled by applyTabVisuals.
        auto* underline = new brls::Box();
        underline->setHeight(3);
        underline->setCornerRadius(3);
        underline->setBackgroundColor(kGold);
        underline->setVisibility(brls::Visibility::GONE);
        tab->addView(underline);

        Type t = static_cast<Type>(i);
        tab->registerClickAction([this, t](brls::View*) {
            setTypeFilter(t);
            return true;
        });
        tab->addGestureRecognizer(new brls::TapGestureRecognizer(tab));

        m_typeTabs[i] = TypeTab{ tab, icon, label, badge, count, underline };
        tabsRow->addView(tab);
    }

    scroll->setContentView(tabsRow);
    bar->addView(scroll);

    // 1px baseline the active underline sits on.
    auto* baseline = new brls::Box();
    baseline->setHeight(1);
    baseline->setBackgroundColor(kLine);
    bar->addView(baseline);

    return bar;
}

void DownloadsTab::applyTabVisuals() {
    for (int i = 0; i < 4; i++) {
        auto& T = m_typeTabs[i];
        if (!T.tab) continue;
        bool active = (static_cast<Type>(i) == m_activeType);
        if (T.label)     T.label->setTextColor(active ? kText : kMuted);
        if (T.icon)      T.icon->setAlpha(active ? 1.0f : 0.7f);
        if (T.underline) T.underline->setVisibility(active ? brls::Visibility::VISIBLE
                                                           : brls::Visibility::GONE);
        if (T.badge)     T.badge->setBackgroundColor(active ? kGold : kSurface3);
        if (T.count)     T.count->setTextColor(active ? kGoldInk : kMuted);
    }
}

void DownloadsTab::setTypeFilter(Type type) {
    m_activeType = type;
    applyTabVisuals();

    // Keep "Up from the toolbar" landing on the now-active tab.
    brls::View* up = m_typeTabs[static_cast<int>(type)].tab;
    if (up) {
        if (m_startStopBtn) m_startStopBtn->setCustomNavigationRoute(brls::FocusDirection::UP, up);
        if (m_resumeBtn)    m_resumeBtn->setCustomNavigationRoute(brls::FocusDirection::UP, up);
        if (m_syncBtn)      m_syncBtn->setCustomNavigationRoute(brls::FocusDirection::UP, up);
        if (m_clearBtn)     m_clearBtn->setCustomNavigationRoute(brls::FocusDirection::UP, up);
    }

    // Re-filter the in-memory list (no refetch of the engine).
    rebuildList();
}

void DownloadsTab::updateTypeCounts(const std::vector<DownloadItem>& all) {
    int counts[4] = { static_cast<int>(all.size()), 0, 0, 0 };
    for (const auto& it : all) counts[static_cast<int>(itemType(it))]++;
    for (int i = 0; i < 4; i++) {
        if (m_typeTabs[i].count) m_typeTabs[i].count->setText(std::to_string(counts[i]));
    }
}

void DownloadsTab::updateStorageReadout(const std::vector<DownloadItem>& all) {
    int64_t used = 0, total = 0;
    for (const auto& it : all) {
        if (it.downloadedBytes > 0) used += it.downloadedBytes;
        total += (it.totalBytes > 0) ? it.totalBytes : it.downloadedBytes;
    }
    if (m_storageUsedLabel)  m_storageUsedLabel->setText(formatBytes(used));
    if (m_storageTotalLabel) m_storageTotalLabel->setText(total > 0 ? ("of " + formatBytes(total)) : "");
    if (m_storageMeterFill) {
        float frac = (total > 0) ? static_cast<float>((double)used / (double)total) : 0.0f;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        m_storageMeterFill->setWidth(200.0f * frac);
    }
}

void DownloadsTab::applyResponsiveLayout() {
    if (!m_headerRow || !m_storageBox) return;
    if (platform::isPortrait()) {
        // Drop the storage readout under the title on narrow screens.
        m_headerRow->setAxis(brls::Axis::COLUMN);
        m_headerRow->setAlignItems(brls::AlignItems::FLEX_START);
        m_storageBox->setAlignItems(brls::AlignItems::FLEX_START);
        m_storageBox->setMarginTop(8);
        if (m_titleLabel) m_titleLabel->setGrow(0.0f);
    } else {
        m_headerRow->setAxis(brls::Axis::ROW);
        m_headerRow->setAlignItems(brls::AlignItems::CENTER);
        m_storageBox->setAlignItems(brls::AlignItems::FLEX_END);
        m_storageBox->setMarginTop(0);
        if (m_titleLabel) m_titleLabel->setGrow(1.0f);
    }
}

void DownloadsTab::willAppear(bool resetState) {
    brls::Box::willAppear(resetState);

    if (DownloadsManager::getInstance().isDownloading()) {
        m_startStopLabel->setText("Stop");
    } else {
        m_startStopLabel->setText("Start");
    }

    m_lastState.clear();
    rebuildList();
    startAutoRefresh();
}

void DownloadsTab::willDisappear(bool resetState) {
    brls::Box::willDisappear(resetState);
    stopAutoRefresh();
}

void DownloadsTab::startAutoRefresh() {
    if (m_autoRefreshEnabled.load()) return;
    m_autoRefreshEnabled.store(true);

    auto aliveWeak = std::weak_ptr<bool>(m_alive);

    asyncRun([this, aliveWeak]() {
        while (m_autoRefreshEnabled.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(REFRESH_INTERVAL_MS));

            auto alive = aliveWeak.lock();
            if (!alive || !*alive || !m_autoRefreshEnabled.load()) break;

            brls::sync([this, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                // Tab may have hidden (willDisappear -> stopAutoRefresh)
                // between when this sync was enqueued and when the main
                // thread picked it up. If so the rebuild that refresh()
                // might trigger would be wasted work — and worse, it
                // would tear down rows whose cached lastFocusedView is
                // still pointed at the row from the previous focus,
                // producing a dangling pointer that the next willAppear
                // dereferences when restoring focus. Skip cleanly.
                if (!m_autoRefreshEnabled.load()) return;
                // Skip while a sub-activity (group-detail view, player)
                // is on top of us. brls's pushActivity stashes the
                // currently-focused View* into focusStack so it can
                // restore focus on the matching pop; if we rebuilt the
                // list now, the row that's saved in focusStack would
                // be freed and the pop would crash dereferencing it.
                // The classic trigger is "delete after watching" wiping
                // the only download in a group while the user is still
                // inside its detail view. The detail view runs its own
                // 1Hz refresh that auto-pops once the group is empty,
                // so this throttle isn't blocking that cleanup.
                if (brls::Application::getActivitiesStack().size() > 1) return;
                refresh();
            });
        }
    });
}

void DownloadsTab::stopAutoRefresh() {
    m_autoRefreshEnabled.store(false);
}

void DownloadsTab::refresh() {
    auto downloads = DownloadsManager::getInstance().getDownloads();

    // Keep the sub-tab count badges and storage readout live. Counts only
    // change on add/remove (which forces a structural rebuild below), but
    // the storage figure ticks up byte-by-byte during a download, so both
    // are refreshed here every cycle off the full list.
    updateTypeCounts(downloads);
    updateStorageReadout(downloads);

    if (DownloadsManager::getInstance().isDownloading()) {
        m_startStopLabel->setText("Stop");
    } else {
        m_startStopLabel->setText("Start");
    }

    // Build current state for comparison
    std::vector<CachedItem> currentState;
    currentState.reserve(downloads.size());
    for (const auto& d : downloads) {
        CachedItem ci;
        ci.ratingKey = d.ratingKey;
        ci.downloadedBytes = d.downloadedBytes;
        ci.totalBytes = d.totalBytes;
        ci.state = static_cast<int>(d.state);
        ci.viewOffset = d.viewOffset;
        ci.transcodeElapsedSeconds = d.transcodeElapsedSeconds;
        currentState.push_back(ci);
    }

    // Check if anything changed at all
    bool anyChange = (currentState.size() != m_lastState.size());
    if (!anyChange) {
        for (size_t i = 0; i < currentState.size(); i++) {
            if (currentState[i].ratingKey != m_lastState[i].ratingKey ||
                currentState[i].state != m_lastState[i].state ||
                currentState[i].downloadedBytes != m_lastState[i].downloadedBytes ||
                currentState[i].transcodeElapsedSeconds != m_lastState[i].transcodeElapsedSeconds) {
                anyChange = true;
                break;
            }
        }
    }

    if (!anyChange) return;

    // Decide whether the list LAYOUT has to be rebuilt. Item identity
    // (added/removed/reordered) always requires a rebuild. A pure state
    // transition only requires a rebuild when the action-button category
    // changes (e.g. DOWNLOADING -> COMPLETED swaps "Cancel" for "Play +
    // Delete"); transitions inside the same category (e.g. QUEUED ->
    // TRANSCODING -> DOWNLOADING) are pure visual updates and stay in
    // place. This is what keeps the tab from rebuilding once per second
    // for the duration of a download.
    bool structureChanged = (currentState.size() != m_lastState.size());
    if (!structureChanged) {
        for (size_t i = 0; i < currentState.size(); i++) {
            if (currentState[i].ratingKey != m_lastState[i].ratingKey) {
                structureChanged = true;
                break;
            }
        }
    }
    if (!structureChanged) {
        for (size_t i = 0; i < currentState.size(); i++) {
            DownloadState oldS = static_cast<DownloadState>(m_lastState[i].state);
            DownloadState newS = static_cast<DownloadState>(currentState[i].state);
            if (buttonCategory(oldS) != buttonCategory(newS)) {
                structureChanged = true;
                break;
            }
        }
    }

    m_lastState = currentState;

    if (structureChanged) {
        // Full rebuild needed: items added/removed or state type changed
        rebuildList();
    } else {
        // Progress-only update: just update status text labels in-place
        updateProgressInPlace(downloads);
    }
}

void DownloadsTab::rebuildList() {
    auto allDownloads = DownloadsManager::getInstance().getDownloads();

    // Keep the sub-tab count badges and storage readout live off the
    // full list, then narrow to the active type for what's rendered.
    updateTypeCounts(allDownloads);
    updateStorageReadout(allDownloads);

    std::vector<DownloadItem> downloads;
    downloads.reserve(allDownloads.size());
    for (const auto& d : allDownloads) {
        if (m_activeType == Type::ALL || itemType(d) == m_activeType)
            downloads.push_back(d);
    }

    // Remember which row currently has focus, by stable identifier (item
    // ratingKey or group compositeKey). The previous behaviour just
    // yanked focus onto the Clear button on every rebuild — so the
    // moment any download crossed a button-category boundary
    // (DOWNLOADING -> COMPLETED) and triggered refresh() to rebuild,
    // the user's selection jumped to the toolbar. Capturing the key
    // first lets us restore focus to the same item (now in a freshly
    // built row) once the list is rebuilt.
    std::string rememberedItemKey;
    std::string rememberedGroupKey;
    brls::View* focusedView = brls::Application::getCurrentFocus();
    bool focusInList = m_clearBtn && isDescendantOf(focusedView, m_listContainer);
    if (focusInList) {
        for (const auto& kv : m_itemRows) {
            if (isDescendantOf(focusedView, kv.second)) { rememberedItemKey = kv.first; break; }
        }
        if (rememberedItemKey.empty()) {
            for (const auto& kv : m_groupRows) {
                if (isDescendantOf(focusedView, kv.second)) { rememberedGroupKey = kv.first; break; }
            }
        }
    }

    // Clear in-place update maps (old pointers are about to be destroyed)
    m_itemStatusLabels.clear();
    m_itemRows.clear();
    m_groupStatusLabels.clear();
    m_groupRows.clear();
    m_itemStrips.clear();
    m_groupStrips.clear();

    // Invalidate all in-flight async image loads from previous rebuild cycle.
    // This prevents use-after-free when old brls::Image* targets are destroyed
    // below but their async callbacks haven't fired yet.
    m_aliveAtomic->store(false);
    m_aliveAtomic = std::make_shared<std::atomic<bool>>(true);

    // Park focus on the Clear button only while the old rows are being
    // freed — if we left focus on a row that's about to be deleted, the
    // input system would dereference a dangling View*. We move it back
    // to the matching new row at the end of this function (see
    // rememberedItemKey / rememberedGroupKey above), so the user
    // doesn't experience the toolbar jump.
    if (focusInList) {
        brls::Application::giveFocus(m_clearBtn);
    }

    // brls::Box::removeView does NOT clear the parent's lastFocusedView
    // cache — only clearViews does. If a row in m_listContainer had been
    // focused before (e.g. the user clicked a row to push the group
    // detail activity), m_listContainer->lastFocusedView still points at
    // that row. The removeView loop below frees it, leaving a dangling
    // pointer that any future getDefaultFocus() call on m_listContainer
    // (such as the focus-restoration that runs after willAppear) will
    // dereference -> segfault. Clear it explicitly first.
    m_listContainer->setLastFocusedView(nullptr);

    // Clear existing rows. The empty-state placeholder lives outside the
    // scroll now, so every child here is a real row.
    while (!m_listContainer->getChildren().empty()) {
        m_listContainer->removeView(m_listContainer->getChildren()[0]);
    }

    if (downloads.empty()) {
        std::string msg;
        if (allDownloads.empty()) {
            msg = "No downloads yet.\nUse the download button on media details to save for offline viewing.";
        } else {
            const char* tn = (m_activeType == Type::MOVIES) ? "Movies"
                           : (m_activeType == Type::SHOWS)  ? "Shows"
                           : (m_activeType == Type::MUSIC)  ? "Music" : "";
            msg = std::string("No ") + tn + " downloads";
        }
        // Give the scroll a single non-focusable placeholder item. An empty
        // ScrollingFrame collapses (the column then drifts to the middle);
        // one real item makes it lay out exactly like the populated case,
        // which is the only configuration we've seen render correctly.
        auto* placeholder = new brls::Box();
        placeholder->setAxis(brls::Axis::COLUMN);
        placeholder->setJustifyContent(brls::JustifyContent::CENTER);
        placeholder->setAlignItems(brls::AlignItems::CENTER);
        placeholder->setHeight(220);
        m_emptyLabel = new brls::Label();
        m_emptyLabel->setText(msg);
        m_emptyLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        m_emptyLabel->setTextColor(kMuted);
        placeholder->addView(m_emptyLabel);
        m_listContainer->addView(placeholder);
        return;
    }

    // Group information
    struct GroupInfo {
        DownloadGroupType type;
        std::string key;
        std::string title;
        std::string thumb;
        int total = 0;
        int contentTotal = 0;  // Stable total from groupTotalItems
        int completed = 0;
        int downloading = 0;
    };

    // Collect groups and ungrouped items in order of first appearance
    std::vector<std::string> groupOrder;
    std::map<std::string, GroupInfo> groups;
    std::vector<DownloadItem> ungrouped;

    for (const auto& item : downloads) {
        if (item.groupType != DownloadGroupType::NONE && !item.groupKey.empty()) {
            std::string compositeKey = std::to_string(static_cast<int>(item.groupType)) + ":" + item.groupKey;
            auto it = groups.find(compositeKey);
            if (it == groups.end()) {
                GroupInfo gi;
                gi.type = item.groupType;
                gi.key = item.groupKey;
                gi.title = item.groupTitle;
                gi.thumb = item.groupThumb;
                gi.total = 1;
                gi.contentTotal = item.groupTotalItems;
                gi.completed = (item.state == DownloadState::COMPLETED) ? 1 : 0;
                gi.downloading = (item.state == DownloadState::DOWNLOADING || item.state == DownloadState::TRANSCODING) ? 1 : 0;
                groups[compositeKey] = gi;
                groupOrder.push_back(compositeKey);
            } else {
                it->second.total++;
                if (item.groupTotalItems > it->second.contentTotal) it->second.contentTotal = item.groupTotalItems;
                if (item.state == DownloadState::COMPLETED) it->second.completed++;
                if (item.state == DownloadState::DOWNLOADING || item.state == DownloadState::TRANSCODING) it->second.downloading++;
                // Use first non-empty thumb
                if (it->second.thumb.empty() && !item.groupThumb.empty()) {
                    it->second.thumb = item.groupThumb;
                }
            }
        } else {
            ungrouped.push_back(item);
        }
    }

    // Add grouped entries
    for (const auto& compositeKey : groupOrder) {
        const auto& gi = groups[compositeKey];
        auto* row = buildGroupRow(gi.type, gi.key, gi.title, gi.thumb,
                                   gi.total, gi.completed, gi.downloading,
                                   gi.contentTotal);
        m_listContainer->addView(row);
    }

    // Add ungrouped items
    for (const auto& item : ungrouped) {
        auto* row = buildItemRow(item);
        m_listContainer->addView(row);
    }

    // Set up focus navigation between action buttons and list items
    auto& children = m_listContainer->getChildren();
    brls::View* firstListItem = nullptr;
    for (auto* child : children) {
        if (child->isFocusable()) {
            firstListItem = child;
            break;
        }
    }

    if (firstListItem) {
        // DOWN from each action button -> first list item
        if (m_startStopBtn) m_startStopBtn->setCustomNavigationRoute(brls::FocusDirection::DOWN, firstListItem);
        if (m_resumeBtn) m_resumeBtn->setCustomNavigationRoute(brls::FocusDirection::DOWN, firstListItem);
        if (m_syncBtn) m_syncBtn->setCustomNavigationRoute(brls::FocusDirection::DOWN, firstListItem);
        if (m_clearBtn) m_clearBtn->setCustomNavigationRoute(brls::FocusDirection::DOWN, firstListItem);

        // UP from each list item -> first action button (Start/Stop)
        for (auto* child : children) {
            if (child->isFocusable()) {
                child->setCustomNavigationRoute(brls::FocusDirection::UP, m_startStopBtn);
            }
        }
    }

    // Restore focus to whichever item was selected before the rebuild,
    // matched by stable identifier (ratingKey / compositeKey). If the
    // item no longer exists in the new layout (e.g. user clicked Clear
    // and it was wiped) focus simply remains on the Clear button.
    if (focusInList) {
        brls::View* restoreTarget = nullptr;
        if (!rememberedItemKey.empty()) {
            auto it = m_itemRows.find(rememberedItemKey);
            if (it != m_itemRows.end()) restoreTarget = it->second;
        }
        if (!restoreTarget && !rememberedGroupKey.empty()) {
            auto it = m_groupRows.find(rememberedGroupKey);
            if (it != m_groupRows.end()) restoreTarget = it->second;
        }
        if (restoreTarget) {
            brls::Application::giveFocus(restoreTarget);
        }
    }
}

void DownloadsTab::updateProgressInPlace(const std::vector<DownloadItem>& downloads) {
    // Update individual item status labels and recolor the row to match
    // the new state. The row's action buttons stay as-is — refresh()
    // already guarantees no button-category change reaches this path, so
    // a row that started life as "Cancel"-bearing is still showing the
    // right button.
    for (const auto& item : downloads) {
        auto it = m_itemStatusLabels.find(item.ratingKey);
        if (it != m_itemStatusLabels.end()) {
            it->second->setText(buildItemStatusText(item));
        }
        auto stripIt = m_itemStrips.find(item.ratingKey);
        if (stripIt != m_itemStrips.end()) {
            stripIt->second->setBackgroundColor(stateStripColor(item.state));
        }
    }

    // Update group status labels
    struct GroupProgress {
        std::string typePrefix;
        int total = 0;
        int contentTotal = 0;  // Stable total from groupTotalItems
        int completed = 0;
        int downloading = 0;
    };
    std::map<std::string, GroupProgress> groupStats;

    for (const auto& item : downloads) {
        if (item.groupType != DownloadGroupType::NONE && !item.groupKey.empty()) {
            std::string compositeKey = std::to_string(static_cast<int>(item.groupType)) + ":" + item.groupKey;
            auto& gp = groupStats[compositeKey];
            if (gp.total == 0) {
                switch (item.groupType) {
                    case DownloadGroupType::PLAYLIST: gp.typePrefix = "Playlist"; break;
                    case DownloadGroupType::ALBUM:    gp.typePrefix = "Album"; break;
                    case DownloadGroupType::ARTIST:   gp.typePrefix = "Artist"; break;
                    case DownloadGroupType::SHOW:     gp.typePrefix = "Show"; break;
                    default: break;
                }
            }
            gp.total++;
            if (item.groupTotalItems > gp.contentTotal) gp.contentTotal = item.groupTotalItems;
            if (item.state == DownloadState::COMPLETED) gp.completed++;
            if (item.state == DownloadState::DOWNLOADING || item.state == DownloadState::TRANSCODING) gp.downloading++;
        }
    }

    for (const auto& pair : groupStats) {
        const auto& gp = pair.second;
        int displayTotal = (gp.contentTotal > 0) ? gp.contentTotal : gp.total;

        auto it = m_groupStatusLabels.find(pair.first);
        if (it != m_groupStatusLabels.end()) {
            std::string statusText = gp.typePrefix + " - " + std::to_string(gp.completed) + "/" +
                                     std::to_string(displayTotal) + " ready";
            if (gp.downloading > 0) {
                statusText += " (" + std::to_string(gp.downloading) + " downloading)";
            }
            it->second->setText(statusText);
        }
        auto stripIt = m_groupStrips.find(pair.first);
        if (stripIt != m_groupStrips.end()) {
            stripIt->second->setBackgroundColor(
                groupStripColor(gp.completed, displayTotal, gp.downloading));
        }
    }
}

brls::Box* DownloadsTab::buildGroupRow(DownloadGroupType groupType, const std::string& groupKey,
                                        const std::string& groupTitle, const std::string& groupThumb,
                                        int totalItems, int completedItems, int downloadingItems,
                                        int contentTotal) {
    // Use contentTotal (stable) for Y if available, otherwise fall back to current count
    int displayTotal = (contentTotal > 0) ? contentTotal : totalItems;
    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setPadding(8);
    row->setMargins(0, 0, 8, 0);
    row->setCornerRadius(13);
    row->setFocusable(true);
    row->setHideHighlightBackground(true);
    row->setHighlightCornerRadius(13);
    row->setBackgroundColor(kSurface);

    // 4px left state-accent strip (recoloured in place by updateProgressInPlace)
    auto* strip = new brls::Box();
    strip->setWidth(4);
    strip->setCornerRadius(2);
    strip->setAlignSelf(brls::AlignSelf::STRETCH);
    strip->setMarginRight(8);
    strip->setBackgroundColor(groupStripColor(completedItems, displayTotal, downloadingItems));
    row->addView(strip);
    m_groupStrips[std::to_string(static_cast<int>(groupType)) + ":" + groupKey] = strip;

    // Cover art thumbnail
    auto* thumbImage = new brls::Image();
    bool isMusic = (groupType == DownloadGroupType::PLAYLIST ||
                    groupType == DownloadGroupType::ALBUM ||
                    groupType == DownloadGroupType::ARTIST);
    int thumbW = isMusic ? 60 : 45;
    int thumbH = isMusic ? 60 : 67;
    thumbImage->setSize(brls::Size(thumbW, thumbH));
    thumbImage->setScalingType(brls::ImageScalingType::FIT);
    thumbImage->setMargins(0, 10, 0, 0);
    thumbImage->setCornerRadius(4);

    // Try to load cover from first completed item in this group, or use async URL
    // Hide thumbnail initially to prevent null texture rendering crash on Vita
    thumbImage->setVisibility(brls::Visibility::GONE);
    if (!groupThumb.empty()) {
        // Check if any item in the group has a downloaded cover
        auto groupItems = DownloadsManager::getInstance().getDownloadsByGroup(groupType, groupKey);
        bool loaded = false;
        for (const auto& gi : groupItems) {
            if (!gi.thumbPath.empty() && gi.state == DownloadState::COMPLETED) {
                if (ImageLoader::loadFromFile(gi.thumbPath, thumbImage)) {
                    thumbImage->setVisibility(brls::Visibility::VISIBLE);
                    loaded = true;
                    break;
                }
            }
        }
        if (!loaded) {
            // Load from server URL - show only when texture loads successfully
            std::string thumbUrl = PlexClient::getInstance().getThumbnailUrl(groupThumb, thumbW * 2, thumbH * 2);
            if (!thumbUrl.empty()) {
                ImageLoader::loadAsync(thumbUrl, [](brls::Image* img) {
                    img->setVisibility(brls::Visibility::VISIBLE);
                }, thumbImage, m_aliveAtomic);
            }
        }
    }
    row->addView(thumbImage);

    // Info column
    auto* infoBox = new brls::Box();
    infoBox->setAxis(brls::Axis::COLUMN);
    infoBox->setGrow(1.0f);

    // Group type label
    std::string typePrefix;
    switch (groupType) {
        case DownloadGroupType::PLAYLIST: typePrefix = "Playlist"; break;
        case DownloadGroupType::ALBUM:    typePrefix = "Album"; break;
        case DownloadGroupType::ARTIST:   typePrefix = "Artist"; break;
        case DownloadGroupType::SHOW:     typePrefix = "Show"; break;
        default: break;
    }

    auto* titleLabel = new brls::Label();
    titleLabel->setText(groupTitle);
    titleLabel->setFontSize(18);
    titleLabel->setTextColor(kText);
    infoBox->addView(titleLabel);

    auto* statusLabel = new brls::Label();
    statusLabel->setFontSize(14);
    statusLabel->setTextColor(kMuted);
    std::string statusText = typePrefix + " - " + std::to_string(completedItems) + "/" +
                             std::to_string(displayTotal) + " ready";
    if (downloadingItems > 0) {
        statusText += " (" + std::to_string(downloadingItems) + " downloading)";
    }
    statusLabel->setText(statusText);
    infoBox->addView(statusLabel);

    // Track for in-place progress updates
    std::string compositeKey = std::to_string(static_cast<int>(groupType)) + ":" + groupKey;
    m_groupStatusLabels[compositeKey] = statusLabel;
    m_groupRows[compositeKey] = row;

    row->addView(infoBox);

    // Click to view tracks in this group
    DownloadGroupType capturedType = groupType;
    std::string capturedKey = groupKey;
    std::string capturedTitle = groupTitle;
    row->registerClickAction([this, capturedType, capturedKey, capturedTitle](brls::View*) {
        showGroupDetail(capturedType, capturedKey, capturedTitle);
        return true;
    });
    row->addGestureRecognizer(new brls::TapGestureRecognizer(row));

    // START button context menu
    row->registerAction("Options", brls::ControllerButton::BUTTON_START,
        [this, capturedType, capturedKey, capturedTitle](brls::View*) {
            showGroupContextMenu(capturedType, capturedKey, capturedTitle);
            return true;
        });

    return row;
}

brls::Box* DownloadsTab::buildItemRow(const DownloadItem& item) {
    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setPadding(8);
    row->setMargins(0, 0, 8, 0);
    row->setCornerRadius(13);
    row->setFocusable(true);
    row->setHideHighlightBackground(true);
    row->setHighlightCornerRadius(13);
    row->setBackgroundColor(kSurface);

    // 4px left state-accent strip (recoloured in place by updateProgressInPlace)
    auto* strip = new brls::Box();
    strip->setWidth(4);
    strip->setCornerRadius(2);
    strip->setAlignSelf(brls::AlignSelf::STRETCH);
    strip->setMarginRight(8);
    strip->setBackgroundColor(stateStripColor(item.state));
    row->addView(strip);
    m_itemStrips[item.ratingKey] = strip;

    // Cover art / poster thumbnail
    auto* thumbImage = new brls::Image();
    bool isMusic = (item.mediaType == "track");
    bool isMovie = (item.mediaType == "movie");
    int thumbW = isMusic ? 60 : (isMovie ? 45 : 50);
    int thumbH = isMusic ? 60 : (isMovie ? 67 : 38);
    thumbImage->setSize(brls::Size(thumbW, thumbH));
    thumbImage->setScalingType(brls::ImageScalingType::FIT);
    thumbImage->setMargins(0, 10, 0, 0);
    thumbImage->setCornerRadius(4);

    // Load thumbnail - hide initially to prevent null texture rendering crash on Vita
    thumbImage->setVisibility(brls::Visibility::GONE);
    if (!item.thumbPath.empty() && item.state == DownloadState::COMPLETED) {
        if (ImageLoader::loadFromFile(item.thumbPath, thumbImage)) {
            thumbImage->setVisibility(brls::Visibility::VISIBLE);
        }
    } else if (!item.thumbUrl.empty()) {
        std::string thumbUrl = PlexClient::getInstance().getThumbnailUrl(item.thumbUrl, thumbW * 2, thumbH * 2);
        if (!thumbUrl.empty()) {
            ImageLoader::loadAsync(thumbUrl, [](brls::Image* img) {
                img->setVisibility(brls::Visibility::VISIBLE);
            }, thumbImage, m_aliveAtomic);
        }
    }
    row->addView(thumbImage);

    // Info column
    auto* infoBox = new brls::Box();
    infoBox->setAxis(brls::Axis::COLUMN);
    infoBox->setGrow(1.0f);

    auto* titleLabel = new brls::Label();
    std::string displayTitle = item.title;
    if (!item.parentTitle.empty()) {
        displayTitle = item.parentTitle + " - " + item.title;
    }
    if (item.seasonNum > 0 && item.episodeNum > 0) {
        char epStr[32];
        snprintf(epStr, sizeof(epStr), " (S%02dE%02d)", item.seasonNum, item.episodeNum);
        displayTitle += epStr;
    }
    titleLabel->setText(displayTitle);
    titleLabel->setFontSize(18);
    titleLabel->setTextColor(kText);
    infoBox->addView(titleLabel);

    auto* statusLabel = new brls::Label();
    statusLabel->setFontSize(14);
    statusLabel->setText(buildItemStatusText(item));
    statusLabel->setTextColor(kMuted);
    infoBox->addView(statusLabel);

    // Track for in-place progress updates
    m_itemStatusLabels[item.ratingKey] = statusLabel;
    m_itemRows[item.ratingKey] = row;

    row->addView(infoBox);

    // Action buttons
    auto* buttonsBox = new brls::Box();
    buttonsBox->setAxis(brls::Axis::ROW);

    if (item.state == DownloadState::COMPLETED) {
        // START button for context menu on completed items
        DownloadItem capturedItem = item;
        row->registerAction("Options", brls::ControllerButton::BUTTON_START,
            [this, capturedItem](brls::View*) {
                showItemContextMenu(capturedItem);
                return true;
            });

        auto* playBtn = new brls::Button();
        auto* playLabel = new brls::Label();
        playLabel->setText("Play");
        playLabel->setFontSize(14);
        playBtn->addView(playLabel);
        playBtn->setMargins(0, 0, 0, 5);

        std::string ratingKey = item.ratingKey;
        bool isTrack = (item.mediaType == "track");
        DownloadItem capturedForPlay = item;
        playBtn->registerClickAction([ratingKey, isTrack, capturedForPlay](brls::View*) {
            if (isTrack) {
                MediaItem mi;
                mi.ratingKey = capturedForPlay.ratingKey;
                mi.title = capturedForPlay.title;
                mi.grandparentTitle = capturedForPlay.parentTitle;
                mi.parentTitle = capturedForPlay.parentTitle;
                mi.duration = capturedForPlay.duration;
                mi.thumb = capturedForPlay.thumbUrl;

                std::vector<MediaItem> single = {mi};
                brls::Application::pushActivity(
                    PlayerActivity::createWithQueue(single, 0));
            } else {
                brls::Application::pushActivity(new PlayerActivity(ratingKey, true));
            }
            return true;
        });
        styleToolbarButton(playBtn, playLabel, true);
        buttonsBox->addView(playBtn);

        auto* deleteBtn = new brls::Button();
        auto* delLabel = new brls::Label();
        delLabel->setText("Delete");
        delLabel->setFontSize(14);
        deleteBtn->addView(delLabel);

        std::string key = item.ratingKey;
        deleteBtn->registerClickAction([this, key](brls::View*) {
            DownloadsManager::getInstance().deleteDownload(key);
            brls::Application::notify("Download deleted");
            m_lastState.clear();
            return true;
        });
        styleToolbarButton(deleteBtn, delLabel, false);
        buttonsBox->addView(deleteBtn);
    } else if (item.state == DownloadState::DOWNLOADING ||
               item.state == DownloadState::TRANSCODING ||
               item.state == DownloadState::QUEUED) {
        auto* cancelBtn = new brls::Button();
        auto* cancelLabel = new brls::Label();
        cancelLabel->setText("Cancel");
        cancelLabel->setFontSize(14);
        cancelBtn->addView(cancelLabel);

        std::string key = item.ratingKey;
        cancelBtn->registerClickAction([this, key](brls::View*) {
            DownloadsManager::getInstance().cancelDownload(key);
            brls::Application::notify("Download cancelled");
            m_lastState.clear();
            return true;
        });
        styleToolbarButton(cancelBtn, cancelLabel, false);
        buttonsBox->addView(cancelBtn);
    } else if (item.state == DownloadState::PAUSED ||
               item.state == DownloadState::FAILED) {
        auto* cancelBtn = new brls::Button();
        auto* cancelLabel = new brls::Label();
        cancelLabel->setText("Remove");
        cancelLabel->setFontSize(14);
        cancelBtn->addView(cancelLabel);

        std::string key = item.ratingKey;
        cancelBtn->registerClickAction([this, key](brls::View*) {
            DownloadsManager::getInstance().cancelDownload(key);
            brls::Application::notify("Download removed");
            m_lastState.clear();
            return true;
        });
        styleToolbarButton(cancelBtn, cancelLabel, false);
        buttonsBox->addView(cancelBtn);
    }

    row->addView(buttonsBox);
    return row;
}

void DownloadsTab::showGroupDetail(DownloadGroupType groupType, const std::string& groupKey,
                                    const std::string& groupTitle) {
    auto items = DownloadsManager::getInstance().getDownloadsByGroup(groupType, groupKey);
    if (items.empty()) return;

    // Alive flag for async image loads - invalidated when activity is destroyed
    auto viewAlive = std::make_shared<std::atomic<bool>>(true);

    // Per-row handles for the 1 Hz live update below. Each track / episode
    // row keeps a pointer to its container Box (for recolouring) and to
    // its status Label (for "Downloading 12% / Transcoding..." text). The
    // status label is always created — for COMPLETED items it's just
    // hidden — so we don't have to attach a new view on a state
    // transition, which would require focus juggling. The header
    // typeLabel ("Album - 3/10 ready (1 downloading)") gets the same
    // treatment by capture below.
    struct DetailRowHandles {
        brls::Box*   row         = nullptr;
        brls::Label* statusLabel = nullptr;
        DownloadState lastState  = DownloadState::QUEUED;
        bool         hasLastState = false;
    };
    auto rowHandles = std::make_shared<std::map<std::string, DetailRowHandles>>();

    // Build a full-screen view mirroring the online MediaDetailView layout
    auto* mainBox = new brls::Box();
    mainBox->setAxis(brls::Axis::COLUMN);
    mainBox->setJustifyContent(brls::JustifyContent::FLEX_START);
    mainBox->setAlignItems(brls::AlignItems::STRETCH);
    mainBox->setGrow(1.0f);

    // Register back button (B/Circle) to pop this activity
    mainBox->registerAction("Back", brls::ControllerButton::BUTTON_B, [viewAlive](brls::View* view) {
        viewAlive->store(false);
        brls::Application::popActivity();
        return true;
    }, false, false, brls::Sound::SOUND_BACK);

    // Top section: cover art + info (fixed, non-scrolling)
    auto* topRow = new brls::Box();
    topRow->setAxis(brls::Axis::ROW);
    topRow->setJustifyContent(brls::JustifyContent::FLEX_START);
    topRow->setAlignItems(brls::AlignItems::FLEX_START);
    topRow->setPadding(30, 30, 15, 30);

    // Cover art
    bool isMusic = (groupType == DownloadGroupType::PLAYLIST ||
                    groupType == DownloadGroupType::ALBUM ||
                    groupType == DownloadGroupType::ARTIST);
    int artW = isMusic ? 150 : 120;
    int artH = isMusic ? 150 : 180;

    auto* coverImage = new brls::Image();
    coverImage->setSize(brls::Size(artW, artH));
    coverImage->setScalingType(brls::ImageScalingType::FIT);
    coverImage->setMarginRight(20);
    coverImage->setCornerRadius(8);
    coverImage->setVisibility(brls::Visibility::GONE);

    // Try local cover first, then server URL
    bool coverLoaded = false;
    for (const auto& item : items) {
        if (!item.thumbPath.empty() && item.state == DownloadState::COMPLETED) {
            if (ImageLoader::loadFromFile(item.thumbPath, coverImage)) {
                coverImage->setVisibility(brls::Visibility::VISIBLE);
                coverLoaded = true;
                break;
            }
        }
    }
    if (!coverLoaded && !items.empty() && !items[0].groupThumb.empty()) {
        std::string thumbUrl = PlexClient::getInstance().getThumbnailUrl(items[0].groupThumb, artW * 2, artH * 2);
        if (!thumbUrl.empty()) {
            ImageLoader::loadAsync(thumbUrl, [](brls::Image* img) {
                img->setVisibility(brls::Visibility::VISIBLE);
            }, coverImage, viewAlive);
        }
    }
    topRow->addView(coverImage);

    // Info column
    auto* infoBox = new brls::Box();
    infoBox->setAxis(brls::Axis::COLUMN);
    infoBox->setGrow(1.0f);

    auto* titleLabel = new brls::Label();
    titleLabel->setText(groupTitle);
    titleLabel->setFontSize(26);
    titleLabel->setMarginBottom(8);
    infoBox->addView(titleLabel);

    // Type label
    std::string typeStr;
    switch (groupType) {
        case DownloadGroupType::PLAYLIST: typeStr = "Playlist"; break;
        case DownloadGroupType::ALBUM:    typeStr = "Album"; break;
        case DownloadGroupType::ARTIST:   typeStr = "Artist"; break;
        case DownloadGroupType::SHOW:     typeStr = "TV Show"; break;
        default: break;
    }

    int completed = 0, total = (int)items.size();
    int contentTotal = 0;
    for (const auto& item : items) {
        if (item.state == DownloadState::COMPLETED) completed++;
        if (item.groupTotalItems > contentTotal) contentTotal = item.groupTotalItems;
    }
    int stableTotal = (contentTotal > 0) ? contentTotal : total;

    auto* typeLabel = new brls::Label();
    typeLabel->setFontSize(16);
    typeLabel->setTextColor(nvgRGBA(180, 180, 180, 255));
    std::string itemWord = isMusic ? "tracks" : "items";
    typeLabel->setText(typeStr + " - " + std::to_string(completed) + "/" +
                       std::to_string(stableTotal) + " " + itemWord + " ready");
    typeLabel->setMarginBottom(15);
    infoBox->addView(typeLabel);

    // Action buttons. firstActionBtn is captured into the navigation
    // wiring after the track list is built so that UP from the first
    // row lands on Play (Issue #2) instead of bouncing back to the row
    // itself.
    brls::Button* firstActionBtn = nullptr;
    auto addActionBtn = [&infoBox, &firstActionBtn](const std::string& text, std::function<bool(brls::View*)> action) -> brls::Button* {
        auto* btn = new brls::Button();
        btn->setText(text);
        btn->setWidth(180);
        btn->setHeight(40);
        btn->setMarginBottom(8);
        btn->registerClickAction(action);
        btn->addGestureRecognizer(new brls::TapGestureRecognizer(btn));
        infoBox->addView(btn);
        if (!firstActionBtn) firstActionBtn = btn;
        return btn;
    };

    // Build completed MediaItem list for queue operations
    auto completedItems = std::make_shared<std::vector<MediaItem>>();
    for (const auto& item : items) {
        if (item.state == DownloadState::COMPLETED) {
            MediaItem mi;
            mi.ratingKey = item.ratingKey;
            mi.title = item.title;
            mi.grandparentTitle = item.parentTitle;
            mi.parentTitle = item.parentTitle;
            mi.duration = item.duration;
            mi.thumb = item.thumbUrl;
            completedItems->push_back(mi);
        }
    }

    if (isMusic) {
        addActionBtn("Play All", [completedItems](brls::View*) {
            if (!completedItems->empty()) {
                brls::Application::pushActivity(
                    PlayerActivity::createWithQueue(*completedItems, 0));
            } else {
                brls::Application::notify("No completed tracks to play");
            }
            return true;
        });

        addActionBtn("Add to Queue", [completedItems](brls::View*) {
            if (completedItems->empty()) {
                brls::Application::notify("No completed tracks to add");
                return true;
            }
            MusicQueue& queue = MusicQueue::getInstance();
            if (queue.isEmpty()) {
                brls::Application::pushActivity(
                    PlayerActivity::createWithQueue(*completedItems, 0));
            } else {
                queue.addTracks(*completedItems);
                brls::Application::notify("Added " + std::to_string(completedItems->size()) + " tracks to queue");
            }
            return true;
        });
    } else {
        // TV Show / Movie - just "Play All" which plays first item
        addActionBtn("Play", [completedItems](brls::View*) {
            if (!completedItems->empty()) {
                brls::Application::pushActivity(
                    new PlayerActivity(completedItems->front().ratingKey, true));
            } else {
                brls::Application::notify("No completed items to play");
            }
            return true;
        });
    }

    topRow->addView(infoBox);
    mainBox->addView(topRow);

    // Track / Episode list header
    auto* listHeader = new brls::Label();
    if (groupType == DownloadGroupType::SHOW) {
        listHeader->setText("Episodes");
    } else {
        listHeader->setText("Tracks");
    }
    listHeader->setFontSize(20);
    listHeader->setMargins(0, 30, 10, 30);
    mainBox->addView(listHeader);

    // Scrollable track / episode list
    auto* trackScroll = new brls::ScrollingFrame();
    trackScroll->setGrow(1.0f);

    auto* trackListBox = new brls::Box();
    trackListBox->setAxis(brls::Axis::COLUMN);
    trackListBox->setJustifyContent(brls::JustifyContent::FLEX_START);
    trackListBox->setAlignItems(brls::AlignItems::STRETCH);
    trackListBox->setPadding(0, 30, 20, 30);

    for (size_t i = 0; i < items.size(); i++) {
        const auto& item = items[i];

        const auto& ic = platform::getImageConstraints();
        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setHeight(ic.listRowHeight);
        row->setPadding(10, 16, 10, 16);
        row->setMarginBottom(4);
        row->setCornerRadius(8);
        row->setFocusable(true);

        // Color-code by download state
        if (item.state == DownloadState::COMPLETED) {
            row->setBackgroundColor(nvgRGBA(50, 50, 60, 200));
        } else {
            row->setBackgroundColor(getStateColor(item.state));
        }

        // Left side: number + title
        auto* leftBox = new brls::Box();
        leftBox->setAxis(brls::Axis::ROW);
        leftBox->setAlignItems(brls::AlignItems::CENTER);
        leftBox->setGrow(1.0f);

        auto* numLabel = new brls::Label();
        numLabel->setFontSize(14);
        numLabel->setMarginRight(12);
        numLabel->setTextColor(nvgRGBA(150, 150, 150, 255));

        if (groupType == DownloadGroupType::SHOW && item.seasonNum > 0 && item.episodeNum > 0) {
            char epStr[16];
            snprintf(epStr, sizeof(epStr), "S%02dE%02d", item.seasonNum, item.episodeNum);
            numLabel->setText(epStr);
        } else {
            numLabel->setText(std::to_string(i + 1));
        }
        leftBox->addView(numLabel);

        auto* titleLabel2 = new brls::Label();
        titleLabel2->setFontSize(14);
        std::string displayTitle = item.title;
        if (!item.albumTitle.empty() && groupType == DownloadGroupType::ARTIST) {
            displayTitle += " (" + item.albumTitle + ")";
        }
        // Truncate to whatever the platform budget allows.
        {
            size_t maxChars = (size_t)ic.maxListTitleChars;
            if (maxChars > 3 && displayTitle.length() > maxChars) {
                displayTitle = displayTitle.substr(0, maxChars - 3) + "...";
            }
        }
        titleLabel2->setText(displayTitle);
        leftBox->addView(titleLabel2);

        row->addView(leftBox);

        // Right side: status + duration
        auto* rightSide = new brls::Box();
        rightSide->setAxis(brls::Axis::ROW);
        rightSide->setAlignItems(brls::AlignItems::CENTER);

        // Always create the status label, even for completed items, so
        // the 1 Hz refresh loop below can flip it visible the moment a
        // download starts progressing — instead of needing a full
        // rebuild. GONE costs zero rendering when not in use.
        auto* statusLabel = new brls::Label();
        statusLabel->setFontSize(11);
        statusLabel->setTextColor(nvgRGBA(200, 180, 100, 255));
        statusLabel->setMarginRight(10);
        if (item.state == DownloadState::COMPLETED) {
            statusLabel->setVisibility(brls::Visibility::GONE);
        } else {
            statusLabel->setText(buildItemStatusText(item));
        }
        rightSide->addView(statusLabel);

        // Register row + label for the live update loop.
        DetailRowHandles handles;
        handles.row          = row;
        handles.statusLabel  = statusLabel;
        handles.lastState    = item.state;
        handles.hasLastState = true;
        (*rowHandles)[item.ratingKey] = handles;

        if (item.duration > 0) {
            auto* durLabel = new brls::Label();
            durLabel->setFontSize(12);
            durLabel->setTextColor(nvgRGBA(150, 150, 150, 255));
            int totalSec = (int)(item.duration / 1000);
            int min = totalSec / 60;
            int sec = totalSec % 60;
            char durStr[16];
            snprintf(durStr, sizeof(durStr), "%d:%02d", min, sec);
            durLabel->setText(durStr);
            rightSide->addView(durLabel);
        }

        row->addView(rightSide);

        // Click handler. Always registered (Issue #3) — the lambda
        // queries DownloadsManager at click time for current state and
        // builds the completed-queue snapshot fresh. This way a row
        // that wasn't ready when the view opened becomes playable the
        // instant the download finishes, with no need to leave and
        // re-enter the detail view. For not-yet-ready rows the click
        // is a polite notify instead of a no-op so the user gets
        // feedback that the tap registered.
        DownloadItem capturedItem = item;
        DownloadGroupType capturedGroupType = groupType;
        std::string capturedGroupKey = groupKey;
        bool capturedIsMusic = isMusic;

        auto buildCurrentCompletedQueue = [capturedGroupType, capturedGroupKey](size_t* outIdx, const std::string& targetKey) {
            std::vector<MediaItem> q;
            auto all = DownloadsManager::getInstance().getDownloadsByGroup(capturedGroupType, capturedGroupKey);
            size_t idx = 0;
            for (const auto& it : all) {
                if (it.state != DownloadState::COMPLETED) continue;
                MediaItem mi;
                mi.ratingKey        = it.ratingKey;
                mi.title            = it.title;
                mi.grandparentTitle = it.parentTitle;
                mi.parentTitle      = it.parentTitle;
                mi.duration         = it.duration;
                mi.thumb            = it.thumbUrl;
                if (it.ratingKey == targetKey) idx = q.size();
                q.push_back(std::move(mi));
            }
            if (outIdx) *outIdx = idx;
            return q;
        };

        row->registerClickAction([capturedItem, capturedIsMusic, buildCurrentCompletedQueue](brls::View*) {
            // Snapshot current state from the manager — guaranteed to
            // reflect any completions that happened after the detail
            // view was constructed.
            DownloadItem cur;
            bool found = DownloadsManager::getInstance().getDownloadCopy(capturedItem.ratingKey, cur);
            if (!found || cur.state != DownloadState::COMPLETED) {
                brls::Application::notify("Not ready yet");
                return true;
            }

            if (!capturedIsMusic) {
                brls::Application::pushActivity(new PlayerActivity(capturedItem.ratingKey, true));
                return true;
            }

            // Music — honour the user's track-default action.
            TrackDefaultAction action = Application::getInstance().getSettings().trackDefaultAction;
            MusicQueue& queue = MusicQueue::getInstance();
            size_t completedIdx = 0;
            std::vector<MediaItem> completedQueue =
                buildCurrentCompletedQueue(&completedIdx, capturedItem.ratingKey);
            if (completedQueue.empty()) {
                brls::Application::notify("Nothing ready in this group");
                return true;
            }

            MediaItem mi;
            mi.ratingKey        = capturedItem.ratingKey;
            mi.title            = capturedItem.title;
            mi.grandparentTitle = capturedItem.parentTitle;
            mi.parentTitle      = capturedItem.parentTitle;
            mi.duration         = capturedItem.duration;
            mi.thumb            = capturedItem.thumbUrl;

            switch (action) {
                case TrackDefaultAction::PLAY_NOW_CLEAR:
                default:
                    brls::Application::pushActivity(
                        PlayerActivity::createWithQueue(completedQueue, completedIdx));
                    break;
                case TrackDefaultAction::PLAY_NEXT:
                    if (queue.isEmpty()) {
                        brls::Application::pushActivity(
                            PlayerActivity::createWithQueue(completedQueue, completedIdx));
                    } else {
                        queue.insertTrackAfterCurrent(mi);
                        brls::Application::notify("Playing next: " + mi.title);
                    }
                    break;
                case TrackDefaultAction::ADD_TO_BOTTOM:
                    if (queue.isEmpty()) {
                        brls::Application::pushActivity(
                            PlayerActivity::createWithQueue(completedQueue, completedIdx));
                    } else {
                        queue.addTrack(mi);
                        brls::Application::notify("Added to queue: " + mi.title);
                    }
                    break;
                case TrackDefaultAction::PLAY_NOW_REPLACE:
                    if (queue.isEmpty()) {
                        brls::Application::pushActivity(
                            PlayerActivity::createWithQueue(completedQueue, completedIdx));
                    } else {
                        queue.insertTrackAfterCurrent(mi);
                        if (queue.playNext()) {
                            brls::Application::notify("Now playing: " + mi.title);
                        }
                    }
                    break;
                case TrackDefaultAction::ASK_EACH_TIME: {
                    auto* dlg = new brls::Dialog("Choose Action");
                    auto* opts = new brls::Box();
                    opts->setAxis(brls::Axis::COLUMN);
                    opts->setPadding(20);

                    auto addBtn = [&opts](const std::string& text, std::function<bool(brls::View*)> act) {
                        auto* btn = new brls::Button();
                        btn->setText(text);
                        btn->setHeight(44);
                        btn->setMarginBottom(10);
                        btn->registerClickAction(act);
                        btn->addGestureRecognizer(new brls::TapGestureRecognizer(btn));
                        opts->addView(btn);
                    };

                    addBtn("Play from Here", [dlg, completedQueue, completedIdx](brls::View*) {
                        dlg->dismiss();
                        brls::Application::pushActivity(
                            PlayerActivity::createWithQueue(completedQueue, completedIdx));
                        return true;
                    });
                    addBtn("Play Next", [dlg, mi](brls::View*) {
                        dlg->dismiss();
                        MusicQueue& q = MusicQueue::getInstance();
                        if (q.isEmpty()) {
                            std::vector<MediaItem> single = {mi};
                            brls::Application::pushActivity(
                                PlayerActivity::createWithQueue(single, 0));
                        } else {
                            q.insertTrackAfterCurrent(mi);
                            brls::Application::notify("Playing next: " + mi.title);
                        }
                        return true;
                    });
                    addBtn("Add to Queue", [dlg, mi](brls::View*) {
                        dlg->dismiss();
                        MusicQueue& q = MusicQueue::getInstance();
                        if (q.isEmpty()) {
                            std::vector<MediaItem> single = {mi};
                            brls::Application::pushActivity(
                                PlayerActivity::createWithQueue(single, 0));
                        } else {
                            q.addTrack(mi);
                            brls::Application::notify("Added to queue: " + mi.title);
                        }
                        return true;
                    });
                    addBtn("Cancel", [dlg](brls::View*) {
                        dlg->dismiss();
                        return true;
                    });

                    dlg->addView(opts);
                    dlg->open();
                    break;
                }
            }
            return true;
        });
        row->addGestureRecognizer(new brls::TapGestureRecognizer(row));

        trackListBox->addView(row);
    }

    trackScroll->setContentView(trackListBox);
    mainBox->addView(trackScroll);

    // Issue #2: link the first track row and the first action button
    // (Play / Play All) so vertical navigation between the two sections
    // works without sliding off through whatever happens to be in
    // between. Otherwise UP from the first row falls into the static
    // header label which is not focusable and the input gets eaten.
    brls::View* firstTrackRow = nullptr;
    for (auto* child : trackListBox->getChildren()) {
        if (child->isFocusable()) { firstTrackRow = child; break; }
    }
    if (firstTrackRow && firstActionBtn) {
        firstTrackRow->setCustomNavigationRoute(brls::FocusDirection::UP,   firstActionBtn);
        firstActionBtn->setCustomNavigationRoute(brls::FocusDirection::DOWN, firstTrackRow);
    }

    // Live 1 Hz refresh of the per-row status text and background colour,
    // plus the "Album - 3/10 ready" header. Without this the detail view
    // is a snapshot: the user only sees the new state after popping back
    // out and re-entering. Row click handlers are registered for every
    // row up front (Issue #3) — they consult the manager at click time
    // for current state, so a row that wasn't ready when the view
    // opened becomes playable the moment its download finishes
    // without any view-tree mutation here.
    DownloadGroupType captGroupType = groupType;
    std::string       captGroupKey  = groupKey;
    std::string       captTypeStr   = typeStr;
    bool              captIsMusic   = isMusic;
    brls::Label*      captTypeLabel = typeLabel;

    // Hoist the brls::Activity allocation so the refresh lambda can
    // capture it and tell whether *we* are still the top of the stack
    // before popping. Without this check, an auto-pop fired while a
    // PlayerActivity is on top of us would pop the player by mistake.
    auto* detailActivity = new brls::Activity(mainBox);

    asyncRun([viewAlive, rowHandles, captGroupType, captGroupKey,
              captTypeStr, captIsMusic, captTypeLabel, detailActivity]() {
        while (viewAlive->load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            if (!viewAlive->load()) break;
            brls::sync([viewAlive, rowHandles, captGroupType, captGroupKey,
                        captTypeStr, captIsMusic, captTypeLabel, detailActivity]() {
                // viewAlive is set to false by the BUTTON_B handler
                // before popActivity() runs, so any sync tick enqueued
                // before that point sees false here and exits before
                // touching the (about-to-be-destroyed) Label / Box
                // pointers captured above.
                if (!viewAlive->load()) return;

                auto items = DownloadsManager::getInstance()
                                 .getDownloadsByGroup(captGroupType, captGroupKey);

                // Auto-pop when the group has nothing left. Classic
                // trigger: "delete after watching" wiped the only
                // remaining episode while the user is staring at the
                // detail list. The stale rows can never be played
                // (their downloads are gone), and the outer downloads
                // tab refresh is throttled while we're on top so it
                // won't have cleared the group row yet. Pop ourselves,
                // and brls' focus restoration drops the user back on
                // the group row they originally clicked; once we're
                // gone the outer tab's next refresh tick rebuilds the
                // list (group missing -> focus falls back to Clear).
                if (items.empty()) {
                    auto stack = brls::Application::getActivitiesStack();
                    if (!stack.empty() && stack.back() == detailActivity) {
                        viewAlive->store(false);
                        brls::Application::popActivity();
                    }
                    // Either we just popped, or a sub-activity (player)
                    // is on top of us — defer the pop until that pop
                    // brings us back to the top.
                    return;
                }

                int completed = 0;
                int contentTotal = 0;
                for (const auto& item : items) {
                    if (item.state == DownloadState::COMPLETED) completed++;
                    if (item.groupTotalItems > contentTotal) contentTotal = item.groupTotalItems;

                    auto it = rowHandles->find(item.ratingKey);
                    if (it == rowHandles->end()) continue;
                    auto& h = it->second;

                    // Always refresh the progress text — buildItemStatus
                    // text changes byte-by-byte during DOWNLOADING, and
                    // the elapsed seconds during TRANSCODING.
                    if (item.state != DownloadState::COMPLETED) {
                        h.statusLabel->setText(buildItemStatusText(item));
                    }

                    if (!h.hasLastState || h.lastState != item.state) {
                        h.lastState     = item.state;
                        h.hasLastState  = true;
                        // Recolour to match new state, mirroring the
                        // palette the initial build used (COMPLETED gets
                        // a slightly lifted neutral; everything else
                        // uses getStateColor).
                        if (item.state == DownloadState::COMPLETED) {
                            h.row->setBackgroundColor(nvgRGBA(50, 50, 60, 200));
                            h.statusLabel->setVisibility(brls::Visibility::GONE);
                        } else {
                            h.row->setBackgroundColor(getStateColor(item.state));
                            h.statusLabel->setVisibility(brls::Visibility::VISIBLE);
                        }
                    }
                }

                int total       = (int)items.size();
                int stableTotal = (contentTotal > 0) ? contentTotal : total;
                std::string itemWord = captIsMusic ? "tracks" : "items";
                captTypeLabel->setText(captTypeStr + " - " +
                                       std::to_string(completed) + "/" +
                                       std::to_string(stableTotal) + " " + itemWord + " ready");
            });
        }
    });

    brls::Application::pushActivity(detailActivity);
}

void DownloadsTab::showGroupContextMenu(DownloadGroupType groupType, const std::string& groupKey,
                                         const std::string& groupTitle) {
    auto items = DownloadsManager::getInstance().getDownloadsByGroup(groupType, groupKey);
    if (items.empty()) return;

    auto* dialog = new brls::Dialog(groupTitle);

    auto* optionsBox = new brls::Box();
    optionsBox->setAxis(brls::Axis::COLUMN);
    optionsBox->setPadding(20);

    // Track count info
    int completed = 0;
    int contentTotal = 0;
    for (const auto& item : items) {
        if (item.state == DownloadState::COMPLETED) completed++;
        if (item.groupTotalItems > contentTotal) contentTotal = item.groupTotalItems;
    }
    int stableTotal = (contentTotal > 0) ? contentTotal : (int)items.size();
    auto* infoLabel = new brls::Label();
    infoLabel->setText(std::to_string(completed) + "/" + std::to_string(stableTotal) + " tracks ready");
    infoLabel->setFontSize(14);
    infoLabel->setTextColor(nvgRGBA(180, 180, 180, 255));
    infoLabel->setMarginBottom(10);
    optionsBox->addView(infoLabel);

    auto addBtn = [&optionsBox](const std::string& text, std::function<bool(brls::View*)> action) {
        auto* btn = new brls::Button();
        btn->setText(text);
        btn->setHeight(44);
        btn->setMarginBottom(10);
        btn->registerClickAction(action);
        btn->addGestureRecognizer(new brls::TapGestureRecognizer(btn));
        optionsBox->addView(btn);
    };

    // Build a list of completed tracks as MediaItems
    std::vector<MediaItem> completedTracks;
    for (const auto& item : items) {
        if (item.state == DownloadState::COMPLETED && item.mediaType == "track") {
            MediaItem mi;
            mi.ratingKey = item.ratingKey;
            mi.title = item.title;
            mi.grandparentTitle = item.parentTitle;
            mi.parentTitle = item.parentTitle;
            mi.duration = item.duration;
            mi.thumb = item.thumbUrl;
            completedTracks.push_back(mi);
        }
    }

    // Play Now (Clear Queue)
    addBtn("Play Now (Clear Queue)", [dialog, completedTracks](brls::View*) {
        dialog->dismiss();
        if (!completedTracks.empty()) {
            brls::Application::pushActivity(
                PlayerActivity::createWithQueue(completedTracks, 0));
        } else {
            brls::Application::notify("No completed tracks to play");
        }
        return true;
    });

    // Add to Queue
    addBtn("Add to Queue", [dialog, completedTracks](brls::View*) {
        dialog->dismiss();
        if (completedTracks.empty()) {
            brls::Application::notify("No completed tracks to add");
            return true;
        }
        MusicQueue& queue = MusicQueue::getInstance();
        if (queue.isEmpty()) {
            brls::Application::pushActivity(
                PlayerActivity::createWithQueue(completedTracks, 0));
        } else {
            queue.addTracks(completedTracks);
            brls::Application::notify("Added " + std::to_string(completedTracks.size()) + " tracks to queue");
        }
        return true;
    });

    // Delete All
    DownloadGroupType capturedType = groupType;
    std::string capturedKey = groupKey;
    addBtn("Delete All", [this, dialog, capturedType, capturedKey](brls::View*) {
        dialog->dismiss();
        auto groupItems = DownloadsManager::getInstance().getDownloadsByGroup(capturedType, capturedKey);
        for (const auto& item : groupItems) {
            DownloadsManager::getInstance().deleteDownload(item.ratingKey);
        }
        m_lastState.clear();
        brls::Application::notify("Deleted all items in group");
        return true;
    });

    addBtn("Cancel", [dialog](brls::View*) {
        dialog->dismiss();
        return true;
    });

    dialog->addView(optionsBox);
    dialog->registerAction("Back", brls::ControllerButton::BUTTON_B, [dialog](brls::View*) {
        dialog->dismiss();
        return true;
    });
    dialog->open();
}

void DownloadsTab::showItemContextMenu(const DownloadItem& item) {
    auto* dialog = new brls::Dialog(item.title);

    auto* optionsBox = new brls::Box();
    optionsBox->setAxis(brls::Axis::COLUMN);
    optionsBox->setPadding(20);

    auto addBtn = [&optionsBox](const std::string& text, std::function<bool(brls::View*)> action) {
        auto* btn = new brls::Button();
        btn->setText(text);
        btn->setHeight(44);
        btn->setMarginBottom(10);
        btn->registerClickAction(action);
        btn->addGestureRecognizer(new brls::TapGestureRecognizer(btn));
        optionsBox->addView(btn);
    };

    DownloadItem capturedItem = item;

    if (item.mediaType == "track") {
        MediaItem mi;
        mi.ratingKey = item.ratingKey;
        mi.title = item.title;
        mi.grandparentTitle = item.parentTitle;
        mi.parentTitle = item.parentTitle;
        mi.duration = item.duration;
        mi.thumb = item.thumbUrl;

        addBtn("Play Now (Clear Queue)", [dialog, mi](brls::View*) {
            dialog->dismiss();
            std::vector<MediaItem> single = {mi};
            brls::Application::pushActivity(PlayerActivity::createWithQueue(single, 0));
            return true;
        });

        addBtn("Add to Queue", [dialog, mi](brls::View*) {
            dialog->dismiss();
            MusicQueue& queue = MusicQueue::getInstance();
            if (queue.isEmpty()) {
                std::vector<MediaItem> single = {mi};
                brls::Application::pushActivity(PlayerActivity::createWithQueue(single, 0));
            } else {
                queue.addTrack(mi);
                brls::Application::notify("Added to queue: " + mi.title);
            }
            return true;
        });
    } else {
        // Video content
        std::string ratingKey = item.ratingKey;
        addBtn("Play", [dialog, ratingKey](brls::View*) {
            dialog->dismiss();
            brls::Application::pushActivity(new PlayerActivity(ratingKey, true));
            return true;
        });
    }

    std::string key = item.ratingKey;
    addBtn("Delete", [this, dialog, key](brls::View*) {
        dialog->dismiss();
        DownloadsManager::getInstance().deleteDownload(key);
        m_lastState.clear();
        brls::Application::notify("Download deleted");
        return true;
    });

    addBtn("Cancel", [dialog](brls::View*) {
        dialog->dismiss();
        return true;
    });

    dialog->addView(optionsBox);
    dialog->registerAction("Back", brls::ControllerButton::BUTTON_B, [dialog](brls::View*) {
        dialog->dismiss();
        return true;
    });
    dialog->open();
}

} // namespace vitaplex
