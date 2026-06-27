/**
 * VitaPlex - Library Section Tab
 * Shows content for a single library section (for sidebar mode)
 * Collections, categories (genres) appear as browsable content within the tab
 */

#pragma once

#include <borealis.hpp>
#include <memory>
#include <vector>
#include <map>
#include <string>
#include <utility>
#include "app/plex_client.hpp"
#include "view/recycling_grid.hpp"

namespace vitaplex {

// View mode for the library section
enum class LibraryViewMode {
    ALL_ITEMS,      // Show all items in the library
    COLLECTIONS,    // Show collections as browsable items
    CATEGORIES,     // Show categories/genres as browsable items
    PLAYLISTS,      // Show playlists (music sections only)
    FILTERED        // Showing items filtered by collection or category
};

class LibrarySectionTab : public brls::Box {
public:
    LibrarySectionTab(const std::string& sectionKey, const std::string& title, const std::string& sectionType = "");
    ~LibrarySectionTab() override;

    void onFocusGained() override;
    void willDisappear(bool resetState) override;

private:
    void loadContent();
    void loadCollections();
    void loadGenres();
    void loadPlaylists();
    void showAllItems();
    void showCollections();
    void showCategories();
    void showPlaylists();

    // Back navigation shared by the on-screen "< Back" button and the
    // controller B / remote back / keyboard Esc handler. Returns true
    // if it handled the input, false to let the default back action
    // pop the activity (only relevant when already at ALL_ITEMS).
    bool navigateBack();
    void onItemSelected(const MediaItem& item);
    void onCollectionSelected(const MediaItem& collection);
    void onGenreSelected(const GenreItem& genre);
    void onPlaylistSelected(const Playlist& playlist);
    void showPlaylistContextMenu(const Playlist& playlist);
    void showPlaylistOptionsDialog(const Playlist& playlist);
    void playPlaylistWithQueue(const std::string& playlistId, int startIndex);
    void showPlaylistTrackList(std::vector<MediaItem>&& tracks, const std::string& playlistTitle, const std::string& playlistId);
    void appendTrackListPage();
    void performPlaylistTrackAction(size_t trackIndex);
    void updateViewModeButtons();
    void styleButton(brls::Button* btn, bool active);

    // Direction-A discovery controls (Movies-style sort + filter toolbar).
    // Build the "&sort=…&unwatched=1" fragment from the current sort/filter
    // state; re-query the ALL_ITEMS grid from offset 0 with it; open the sort
    // menu anchored to the Sort chip.
    std::string buildListParams() const;
    void reloadAllItems();
    void showSortMenu();

    // Movies-style Filters menu (video sections): inline filtering across all of
    // Plex's library filters (genre, year, decade, content rating, resolution,
    // studio, country). showFilterMenu lists the fields in a centered dialog
    // (audio-picker style); picking a field fetches its values (cached) and
    // opens a scrollable value picker; apply rebuilds the applied-filter chips +
    // count badge and re-queries the grid.
    void showFilterMenu();
    void openFilterValuePicker(const std::string& field, const std::string& fieldLabel);
    void showFilterValues(const std::string& field, const std::string& fieldLabel,
                          const std::vector<GenreItem>& values);
    void applyFilters();
    void rebuildAppliedFilterChips();
    int  activeFilterCount() const;

    // A-Z jump rail (right edge): build it, jump the grid to the first title in
    // a letter bucket, and refresh its visibility (sort-dependent) + highlight.
    void buildAzRail();
    void jumpToLetter(char letter);
    void refreshAzRail();
    void updateCountLabel();   // "312 titles" beside the page title

    // Check if this tab is still valid (not destroyed)
    bool isValid() const { return m_alive && *m_alive; }

    std::string m_sectionKey;
    std::string m_title;
    std::string m_sectionType;  // "movie", "show", "artist"

    brls::Label* m_titleLabel = nullptr;
    brls::Label* m_countLabel = nullptr;   // total-count text beside the title

    // A-Z jump rail (shown only when sorted Title A-Z). m_azLetters is parallel
    // to the rail's letter string; m_currentAzLetter is the highlighted bucket.
    brls::Box* m_azRail = nullptr;
    std::vector<brls::Label*> m_azLetters;
    char m_currentAzLetter = 0;

    // View mode selector buttons. (No "All" chip — the grid shows all items by
    // default and the Filters menu narrows it; Back returns from a sub-mode.)
    brls::Box* m_viewModeBox = nullptr;
    brls::Button* m_collectionsBtn = nullptr;
    brls::Button* m_categoriesBtn = nullptr;
    brls::Button* m_playlistsBtn = nullptr;
    brls::Button* m_backBtn = nullptr;  // Back button when in filtered view

    // Direction-A toolbar: a flex spacer pushes the Sort chip to the right.
    // (Unwatched is now a "Watch Status" entry inside the Filters menu.)
    brls::Box*    m_toolbarSpacer = nullptr;
    brls::Button* m_sortBtn = nullptr;

    // Filters chip (video sections) + its active-count badge + the row of
    // removable applied-filter chips that sits to its right.
    brls::Button* m_filtersBtn = nullptr;
    brls::Box*    m_filtersBadge = nullptr;
    brls::Label*  m_filtersBadgeLabel = nullptr;
    brls::Box*    m_appliedFiltersBox = nullptr;

    // Current ALL_ITEMS sort + filter. Default = Recently Added (Plex addedAt).
    std::string m_sortParam = "addedAt:desc";
    std::string m_sortLabel = "Recently Added";

    // Inline filter state (video sections): active filters keyed by Plex filter
    // field ("genre", "year", "decade", "contentRating", "resolution", "studio",
    // "country"). Mapped value = {filter value/key, display label}. Empty = none.
    std::map<std::string, std::pair<std::string, std::string>> m_activeFilters;
    // Per-field cache of fetched filter values, so reopening a picker is instant.
    std::map<std::string, std::vector<GenreItem>> m_filterValueCache;

    // Main content grid
    RecyclingGrid* m_contentGrid = nullptr;

    // Track list view (for playlist contents)
    brls::ScrollingFrame* m_trackListScroll = nullptr;
    brls::Box* m_trackListBox = nullptr;

    // Pagination for infinite scroll
    void loadNextPage();
    size_t m_pageOffset = 0;
    int m_totalItemCount = 0;
    // Page size comes from the platform layer now so desktop builds can
    // request hundreds per page while Vita stays at ~60.
    static size_t libraryPageSize();

    // Data
    std::vector<MediaItem> m_items;
    std::vector<MediaItem> m_collections;
    std::vector<GenreItem> m_genres;
    std::vector<Playlist> m_playlists;

    // Current playlist track list (stored as member to avoid per-row copies)
    std::vector<MediaItem> m_playlistTracks;
    std::string m_currentPlaylistId;
    size_t m_trackListRendered = 0;  // How many track rows rendered so far
    brls::Button* m_trackListLoadMoreBtn = nullptr;

    static size_t playlistTrackPageSize();

    LibraryViewMode m_viewMode = LibraryViewMode::ALL_ITEMS;
    // Mode we were in just before transitioning into FILTERED — lets
    // the B-button back action drop the user back where they came from
    // (CATEGORIES if they clicked a genre, COLLECTIONS if they clicked
    // a collection, etc.) instead of always jumping to ALL_ITEMS.
    LibraryViewMode m_modeBeforeFilter = LibraryViewMode::ALL_ITEMS;
    std::string m_filterTitle;  // Title of current filter (collection/genre name)
    bool m_loaded = false;
    bool m_collectionsLoaded = false;
    bool m_genresLoaded = false;
    bool m_playlistsLoaded = false;

    // Shared pointer to track if this object is still alive
    // Used by async callbacks to check validity before updating UI
    std::shared_ptr<bool> m_alive;
};

} // namespace vitaplex
