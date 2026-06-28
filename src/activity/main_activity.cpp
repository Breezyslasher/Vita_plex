/**
 * VitaPlex - Main Activity implementation
 */

#include "activity/main_activity.hpp"
#include "view/home_tab.hpp"
#include "view/library_section_tab.hpp"
#include "view/search_tab.hpp"
#include "view/settings_tab.hpp"
#include "view/livetv_tab.hpp"
#include "view/downloads_tab.hpp"
#include "view/sidebar_editor.hpp"
#include "view/long_press_gesture.hpp"
#include "app/downloads_manager.hpp"
#include "app/application.hpp"
#include "app/plex_client.hpp"
#include "app/music_queue.hpp"
#include "activity/player_activity.hpp"
#include "utils/async.hpp"
#include "platform/platform.hpp"

#if defined(__ANDROID__)
#include <SDL2/SDL_system.h>
#endif

#include <algorithm>

namespace vitaplex {

// Cached library sections for sidebar mode
static std::vector<LibrarySection> s_cachedSections;

MainActivity* MainActivity::s_instance = nullptr;

// Helper to calculate text width (approximate based on character count).
// Average character width scales with the platform's title font size, so
// desktop builds with larger UI text reserve more pixels per character
// than the Vita build does.
static int calculateTextWidth(const std::string& text) {
    const auto& ic = platform::getImageConstraints();
    // Rough glyph width ≈ 0.6 × font size (serviceable for ASCII sidebar labels).
    int charWidth = std::max(8, (ic.titleFontSize * 3) / 4);
    int padding   = 50;
    return static_cast<int>(text.length()) * charWidth + padding;
}

MainActivity::MainActivity() {
    brls::Logger::debug("MainActivity created");
    s_instance = this;
}

MainActivity::~MainActivity() {
    if (s_instance == this) s_instance = nullptr;
}

brls::View* MainActivity::createContentView() {
    return brls::View::createFromXMLResource("activity/main.xml");
}

void MainActivity::applySidebarSizingForViewport() {
    if (!tabFrame) return;
    const auto& ic = platform::getImageConstraints();

    // Calculate dynamic sidebar width based on content. min/max are
    // platform-tuned so desktop builds get a wider sidebar (260-450px)
    // while Vita stays at 200-350px — AND in portrait the same
    // platform tightens these to leave room for the grid.
    int sidebarWidth = ic.sidebarMinWidth;

    std::vector<std::string> standardTabs =
        {"Home", "Library", "Music", "Search", "Live TV", "Downloads", "Settings"};
    for (const auto& tab : standardTabs) {
        sidebarWidth = std::max(sidebarWidth, calculateTextWidth(tab));
    }

    if (!Application::getInstance().isOfflineMode()) {
        // Reuse the section cache populated on the first pass — no point
        // refetching from Plex just because the user rotated.
        for (const auto& section : s_cachedSections) {
            sidebarWidth = std::max(sidebarWidth, calculateTextWidth(section.title));
        }
    }

    sidebarWidth = std::min(sidebarWidth, ic.sidebarMaxWidth);
    brls::View* sidebar = tabFrame->getView("brls/tab_frame/sidebar");
    if (sidebar) {
        sidebar->setWidth(sidebarWidth);
        brls::Logger::debug("MainActivity: Dynamic sidebar width: {}px", sidebarWidth);
    }
}

void MainActivity::onContentAvailable() {
    brls::Logger::debug("MainActivity content available");

#if defined(__ANDROID__)
    // Android TV remotes have no Menu/Guide hardware button, and the
    // existing GUIDE → START re-dispatch only helps remotes that *do*
    // surface one. For the bare 5-button remote (D-pad + OK + Back +
    // Home) the only way to reach the Options context menu is via the
    // OK key, so wire hold-OK as a synthetic GUIDE press. Phones and
    // tablets stay on the default press-time click semantics.
    if (SDL_IsAndroidTV()) {
        brls::Application::setHoldAToOpenMenu(true);
        brls::Logger::info("MainActivity: Android TV detected — hold-OK opens Options menu");
    }
#endif

    if (tabFrame) {
        // First pass populates s_cachedSections from Plex; afterwards
        // applySidebarSizingForViewport() reuses the cache.
        if (!Application::getInstance().isOfflineMode()) {
            PlexClient& client = PlexClient::getInstance();
            std::vector<LibrarySection> sections;
            if (client.fetchLibrarySections(sections)) {
                s_cachedSections = sections;
            }
        }
        applySidebarSizingForViewport();

        // Re-apply sidebar width when the user rotates — portrait
        // constraints squeeze the bar smaller so the grid has room.
        platform::onOrientationChanged([this]() {
            applySidebarSizingForViewport();
        });

        buildSidebarTabs();

        // Focus first tab
        tabFrame->focusTab(0);

        // Press Y while a sidebar tab is focused to open the inline editor in
        // place — no trip through Settings. Registered on the sidebar view (the
        // ancestor of every tab), so the hint + action only apply while the
        // sidebar has focus. The view persists across rebuildSidebar(), so this
        // is wired once. Offline the sidebar is just Downloads + Settings, so
        // there's nothing to reorder — skip it there.
        if (!Application::getInstance().isOfflineMode()) {
            if (brls::View* sidebar = tabFrame->getView("brls/tab_frame/sidebar")) {
                sidebar->registerAction("Edit Sidebar", brls::ControllerButton::BUTTON_Y,
                    [](brls::View*) { SidebarEditor::open(); return true; });
            }
        }

        // Register BUTTON_B on the root content view (parent of tabFrame) so it
        // intercepts back/circle regardless of which child has focus. When a
        // dialog closes, borealis may restore focus to the root Box instead of
        // a child inside tabFrame, which would bypass a handler registered only
        // on tabFrame and let AppletFrame show the "exit this app" dialog.
        brls::View* rootBox = tabFrame->getParent();
        if (rootBox) {
            rootBox->registerAction("", brls::ControllerButton::BUTTON_B, [](brls::View* view) {
                MusicQueue& queue = MusicQueue::getInstance();
                if (!queue.isEmpty() && queue.getCurrentIndex() >= 0) {
                    auto* playerActivity = PlayerActivity::createResumeQueue();
                    brls::Application::pushActivity(playerActivity);
                }
                // Always return true to prevent the exit confirmation dialog
                return true;
            });

            // Android TV remotes (and most TV-style remotes) don't have a
            // BUTTON_START. They DO have a Menu / Guide key, which SDL2
            // surfaces as SDL_CONTROLLER_BUTTON_GUIDE -> borealis
            // BUTTON_GUIDE. Every "Options" context menu in the app is
            // wired to BUTTON_START (cells, grid items, downloads
            // group rows, etc. — 30+ sites), so without this the menu
            // is literally unreachable on those remotes.
            //
            // Re-dispatch GUIDE as a synthetic START at the root level
            // by walking the focused view's parent chain and firing the
            // first BUTTON_START gamepad action we find. Mirrors what
            // brls::Application::handleAction does internally (we can't
            // call that — it's private), but only acts on START and
            // only fires the first hit, which is the matching context
            // menu's handler. Registering only on rootBox means a child
            // view that ever wires its OWN BUTTON_GUIDE action would
            // shadow this — the right precedence.
            rootBox->registerAction("", brls::ControllerButton::BUTTON_GUIDE, [](brls::View*) {
                brls::View* v = brls::Application::getCurrentFocus();
                while (v) {
                    for (auto& a : v->getActions()) {
                        if (a->getType() == brls::ActionType::ACTION_GAMEPAD &&
                            a->getButton() == brls::ControllerButton::BUTTON_START &&
                            a->isAvailable()) {
                            if (a->getActionListener()(v)) return true;
                        }
                    }
                    v = v->getParent();
                }
                return true;
            });
        }
    }
}

// Split a comma-separated id list into ordered, non-empty tokens.
static std::vector<std::string> splitSidebarCsv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur = s;
    size_t pos;
    while ((pos = cur.find(',')) != std::string::npos) {
        std::string t = cur.substr(0, pos);
        if (!t.empty()) out.push_back(t);
        cur.erase(0, pos + 1);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static bool sidebarCsvHas(const std::string& csv, const std::string& id) {
    for (const auto& t : splitSidebarCsv(csv))
        if (t == id) return true;
    return false;
}

// Attach a long-press (hold) recognizer to every sidebar item under `v` so that
// holding a tab opens the inline editor — the touch equivalent of the Y
// shortcut. Per-item (like the grid's long-press menus), so a quick tap still
// selects the tab normally.
static void attachSidebarLongPress(brls::View* v) {
    if (auto* item = dynamic_cast<brls::SidebarItem*>(v)) {
        item->addGestureRecognizer(new LongPressGestureRecognizer(
            item, [](LongPressGestureStatus s) {
                if (s.state == brls::GestureState::START) SidebarEditor::open();
            }));
        return;
    }
    if (auto* box = dynamic_cast<brls::Box*>(v)) {
        for (brls::View* child : box->getChildren())
            attachSidebarLongPress(child);
    }
}

void MainActivity::buildSidebarTabs() {
    if (!tabFrame) return;
    AppSettings& settings = Application::getInstance().getSettings();

    // Offline: only Downloads + Settings are reachable, order doesn't apply.
    if (Application::getInstance().isOfflineMode()) {
        tabFrame->addTab("Downloads", []() { return new DownloadsTab(); });
        tabFrame->addSeparator();
        tabFrame->addTab("Settings", []() { return new SettingsTab(); });
        return;
    }

    const bool hasLiveTV = PlexClient::getInstance().hasLiveTV();

    // Home is always pinned to the top.
    tabFrame->addTab("Home", []() { return new HomeTab(); });

    // Library sections — fetch synchronously + cache (libraries always live in
    // the sidebar now; there is no premade Library/Music tab mode).
    std::vector<LibrarySection> sections;
    if (PlexClient::getInstance().fetchLibrarySections(sections)) {
        s_cachedSections = sections;
    } else {
        sections = s_cachedSections;  // fall back to whatever we last saw
    }

    // Movable item universe, in DEFAULT order: each library, then Search /
    // Live TV / Downloads.
    std::vector<std::string> defaultOrder;
    for (const auto& sec : sections) defaultOrder.push_back("lib:" + sec.key);
    defaultOrder.push_back("search");
    if (hasLiveTV) defaultOrder.push_back("livetv");
    defaultOrder.push_back("downloads");

    auto isValid = [&](const std::string& id) {
        return std::find(defaultOrder.begin(), defaultOrder.end(), id) != defaultOrder.end();
    };

    // Final order: the saved order (filtered to valid ids), then any missing
    // valid ids appended in their default position — so newly discovered
    // libraries land sensibly without clobbering the user's order.
    std::vector<std::string> order;
    auto isPlaced = [&](const std::string& id) {
        return std::find(order.begin(), order.end(), id) != order.end();
    };
    for (const auto& id : splitSidebarCsv(settings.sidebarOrder))
        if (isValid(id) && !isPlaced(id)) order.push_back(id);
    for (const auto& id : defaultOrder)
        if (!isPlaced(id)) order.push_back(id);

    auto findSection = [&](const std::string& key) -> const LibrarySection* {
        for (const auto& sec : sections)
            if (sec.key == key) return &sec;
        return nullptr;
    };

    for (const auto& id : order) {
        if (id.rfind("lib:", 0) == 0) {
            std::string key = id.substr(4);
            if (sidebarCsvHas(settings.hiddenLibraries, key)) continue;
            const LibrarySection* sec = findSection(key);
            if (!sec) continue;
            std::string k = sec->key, t = sec->title, ty = sec->type;
            tabFrame->addTab(t, [k, t, ty]() { return new LibrarySectionTab(k, t, ty); });
        } else {
            // Search is always shown; only Live TV / Downloads can be hidden
            // (via Settings ▸ Interface ▸ Manage Hidden Libraries).
            if (id != "search" && sidebarCsvHas(settings.hiddenSidebarItems, id)) continue;
            if (id == "search")         tabFrame->addTab("Search",    []() { return new SearchTab(); });
            else if (id == "livetv")    tabFrame->addTab("Live TV",   []() { return new LiveTVTab(); });
            else if (id == "downloads") tabFrame->addTab("Downloads", []() { return new DownloadsTab(); });
        }
    }

    // Settings is always pinned to the bottom.
    tabFrame->addSeparator();
    tabFrame->addTab("Settings", []() { return new SettingsTab(); });

    // Hold any sidebar item to open the editor (touch equivalent of Y).
    if (brls::View* sb = tabFrame->getView("brls/tab_frame/sidebar"))
        attachSidebarLongPress(sb);
}

void MainActivity::rebuildSidebar() {
    if (!tabFrame) return;
    tabFrame->clearTabs();
    buildSidebarTabs();
    applySidebarSizingForViewport();
    tabFrame->focusTab(0);
}

float MainActivity::getSidebarWidth() {
    if (!tabFrame) return 0.0f;
    if (brls::View* sb = tabFrame->getView("brls/tab_frame/sidebar"))
        return sb->getWidth();
    return 0.0f;
}

} // namespace vitaplex
