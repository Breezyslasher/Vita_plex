/**
 * VitaPlex - Library Tab
 * Browse library sections and content
 */

#pragma once

#include <borealis.hpp>
#include <memory>
#include "app/plex_client.hpp"
#include "view/recycling_grid.hpp"

namespace vitaplex {

// View mode for library tab browsing
enum class LibraryTabViewMode {
    ALL_ITEMS,
    COLLECTIONS,
    CATEGORIES,
    FILTERED
};

class LibraryTab : public brls::Box {
public:
    LibraryTab();
    ~LibraryTab();

    void onFocusGained() override;
    void willDisappear(bool resetState) override;

private:
    void loadSections();
    void loadContent(const std::string& sectionKey);
    void loadCollections(const std::string& sectionKey);
    void loadGenres(const std::string& sectionKey);
    void onSectionSelected(const LibrarySection& section);
    void onItemSelected(const MediaItem& item);
    void onCollectionSelected(const MediaItem& collection);
    void onGenreSelected(const GenreItem& genre);
    void showAlbumContextMenu(const MediaItem& album);
    void showAllItems();
    void showCollections();
    void showCategories();
    void updateViewModeButtons();

    // Button styling
    void styleButton(brls::Button* btn, bool active);
    void updateSectionButtonStyles();
    brls::Button* m_activeSectionBtn = nullptr;

    brls::Label* m_titleLabel = nullptr;
    brls::HScrollingFrame* m_sectionsScroll = nullptr;
    brls::Box* m_sectionsBox = nullptr;

    // View mode buttons
    brls::Box* m_viewModeBox = nullptr;
    brls::Button* m_allBtn = nullptr;
    brls::Button* m_collectionsBtn = nullptr;
    brls::Button* m_categoriesBtn = nullptr;
    brls::Button* m_backBtn = nullptr;

    RecyclingGrid* m_contentGrid = nullptr;

    std::vector<LibrarySection> m_sections;
    std::vector<MediaItem> m_items;
    std::vector<MediaItem> m_collections;
    std::vector<GenreItem> m_genres;
    std::string m_currentSection;
    std::string m_currentSectionType;
    std::string m_filterTitle;
    LibraryTabViewMode m_viewMode = LibraryTabViewMode::ALL_ITEMS;
    bool m_loaded = false;
    bool m_collectionsLoaded = false;
    bool m_genresLoaded = false;

    // Alive flag for crash prevention on quick tab switching
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace vitaplex
