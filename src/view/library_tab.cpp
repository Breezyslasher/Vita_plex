/**
 * VitaPlex - Library Tab implementation
 */

#include "view/library_tab.hpp"
#include "view/media_item_cell.hpp"
#include "view/media_detail_view.hpp"
#include "app/application.hpp"
#include "utils/image_loader.hpp"
#include "utils/async.hpp"

namespace vitaplex {

LibraryTab::LibraryTab() {
    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setPadding(20);
    this->setGrow(1.0f);

    // Title
    m_titleLabel = new brls::Label();
    m_titleLabel->setText("Library");
    m_titleLabel->setFontSize(28);
    m_titleLabel->setMarginBottom(20);
    this->addView(m_titleLabel);

    // Sections row with horizontal scrolling
    m_sectionsScroll = new brls::HScrollingFrame();
    m_sectionsScroll->setHeight(50);
    m_sectionsScroll->setMarginBottom(20);

    m_sectionsBox = new brls::Box();
    m_sectionsBox->setAxis(brls::Axis::ROW);
    m_sectionsBox->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_sectionsBox->setAlignItems(brls::AlignItems::CENTER);

    m_sectionsScroll->setContentView(m_sectionsBox);
    this->addView(m_sectionsScroll);

    // Content grid
    m_contentGrid = new RecyclingGrid();
    m_contentGrid->setGrow(1.0f);
    m_contentGrid->setOnItemSelected([this](const MediaItem& item) {
        onItemSelected(item);
    });
    this->addView(m_contentGrid);

    // Load sections immediately
    brls::Logger::debug("LibraryTab: Loading sections...");
    loadSections();
}

LibraryTab::~LibraryTab() {
    if (m_alive) { *m_alive = false; }
}

void LibraryTab::willDisappear(bool resetState) {
    brls::Box::willDisappear(resetState);
    if (m_alive) *m_alive = false;
    ImageLoader::cancelAll();
}

void LibraryTab::onFocusGained() {
    brls::Box::onFocusGained();
    m_alive = std::make_shared<bool>(true);

    if (!m_loaded) {
        loadSections();
    }
}

// Helper function to check if a library is hidden
static bool isLibraryHidden(const std::string& key, const std::string& hiddenLibraries) {
    if (hiddenLibraries.empty()) return false;

    std::string hidden = hiddenLibraries;
    size_t pos = 0;
    while ((pos = hidden.find(',')) != std::string::npos) {
        std::string hiddenKey = hidden.substr(0, pos);
        if (hiddenKey == key) return true;
        hidden.erase(0, pos + 1);
    }
    return (hidden == key);
}

void LibraryTab::loadSections() {
    brls::Logger::debug("LibraryTab::loadSections - Starting async load");

    asyncRun([this, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        brls::Logger::debug("LibraryTab: Fetching library sections (async)...");
        PlexClient& client = PlexClient::getInstance();
        std::vector<LibrarySection> sections;

        if (client.fetchLibrarySections(sections)) {
            brls::Logger::info("LibraryTab: Got {} sections", sections.size());

            // Get hidden libraries setting
            std::string hiddenLibraries = Application::getInstance().getSettings().hiddenLibraries;

            // Filter out hidden sections
            std::vector<LibrarySection> visibleSections;
            for (const auto& section : sections) {
                if (!isLibraryHidden(section.key, hiddenLibraries)) {
                    visibleSections.push_back(section);
                } else {
                    brls::Logger::debug("LibraryTab: Hiding section: {}", section.title);
                }
            }

            // Update UI on main thread
            brls::sync([this, visibleSections, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                m_sections = visibleSections;
                m_sectionsBox->clearViews();

                for (const auto& section : m_sections) {
                    brls::Logger::debug("LibraryTab: Adding section button: {}", section.title);
                    auto* btn = new brls::Button();
                    btn->setText(section.title);
                    btn->setMarginRight(10);

                    LibrarySection capturedSection = section;
                    btn->registerClickAction([this, capturedSection](brls::View* view) {
                        onSectionSelected(capturedSection);
                        return true;
                    });

                    m_sectionsBox->addView(btn);
                }

                // Load first section by default
                if (!m_sections.empty()) {
                    brls::Logger::debug("LibraryTab: Loading first section: {}", m_sections[0].title);
                    onSectionSelected(m_sections[0]);
                }

                m_loaded = true;
                brls::Logger::debug("LibraryTab: Sections loading complete");
            });
        } else {
            brls::Logger::error("LibraryTab: Failed to fetch sections");
            brls::sync([this, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                m_loaded = true;
            });
        }
    });
}

void LibraryTab::loadContent(const std::string& sectionKey) {
    brls::Logger::debug("LibraryTab::loadContent - section: {} (async)", sectionKey);

    std::string key = sectionKey;  // Capture by value
    asyncRun([this, key, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;

        if (client.fetchLibraryContent(key, items)) {
            brls::Logger::info("LibraryTab: Got {} items for section {}", items.size(), key);

            // Update UI on main thread
            brls::sync([this, items, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                m_items = items;
                m_contentGrid->setDataSource(m_items);
            });
        } else {
            brls::Logger::error("LibraryTab: Failed to load content for section {}", key);
        }
    });
}

void LibraryTab::onSectionSelected(const LibrarySection& section) {
    m_currentSection = section.key;
    m_titleLabel->setText("Library - " + section.title);
    loadContent(section.key);
}

void LibraryTab::onItemSelected(const MediaItem& item) {
    // For tracks, play directly instead of showing detail view
    if (item.mediaType == MediaType::MUSIC_TRACK) {
        Application::getInstance().pushPlayerActivity(item.ratingKey);
        return;
    }

    // Show media detail view for other types
    auto* detailView = new MediaDetailView(item);
    brls::Application::pushActivity(new brls::Activity(detailView));
}

} // namespace vitaplex
