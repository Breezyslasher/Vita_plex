/**
 * VitaPlex - Library Section Tab
 * Shows content for a single library section (for sidebar mode)
 * Collections, categories (genres) appear as browsable content within the tab
 */

#pragma once

#include <borealis.hpp>
#include <memory>
#include <vector>
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

    // Check if this tab is still valid (not destroyed)
    bool isValid() const { return m_alive && *m_alive; }

    std::string m_sectionKey;
    std::string m_title;
    std::string m_sectionType;  // "movie", "show", "artist"

    brls::Label* m_titleLabel = nullptr;

    // View mode selector buttons
    brls::Box* m_viewModeBox = nullptr;
    brls::Button* m_allBtn = nullptr;
    brls::Button* m_collectionsBtn = nullptr;
    brls::Button* m_categoriesBtn = nullptr;
    brls::Button* m_playlistsBtn = nullptr;
    brls::Button* m_backBtn = nullptr;  // Back button when in filtered view

    // Main content grid
    RecyclingGrid* m_contentGrid = nullptr;

    // Track list view (for playlist contents)
    brls::ScrollingFrame* m_trackListScroll = nullptr;
    brls::Box* m_trackListBox = nullptr;

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

    static constexpr size_t TRACK_LIST_PAGE_SIZE = 50;

    LibraryViewMode m_viewMode = LibraryViewMode::ALL_ITEMS;
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
