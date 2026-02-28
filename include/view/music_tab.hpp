/**
 * VitaPlex - Music Tab
 * Displays music libraries with playlists and collections
 */

#pragma once

#include <borealis.hpp>
#include <memory>
#include "app/plex_client.hpp"
#include "view/recycling_grid.hpp"

namespace vitaplex {

class MusicTab : public brls::Box {
public:
    MusicTab();
    ~MusicTab() override;

    void onFocusGained() override;

private:
    void loadSections();
    void loadContent(const std::string& sectionKey);
    void loadPlaylists();
    void loadCollections(const std::string& sectionKey);
    void onItemSelected(const MediaItem& item);
    void onPlaylistSelected(const Playlist& playlist);
    void onCollectionSelected(const MediaItem& collection);
    brls::Box* createHorizontalRow(const std::string& title);

    // Playlist management
    void showCreatePlaylistDialog();
    void showPlaylistOptionsDialog(const Playlist& playlist);
    void playPlaylistWithQueue(const std::string& playlistId, int startIndex = 0);
    void refreshPlaylists();

    brls::ScrollingFrame* m_scrollView = nullptr;
    brls::Box* m_mainContainer = nullptr;
    brls::Label* m_titleLabel = nullptr;

    // Section selector
    brls::HScrollingFrame* m_sectionsScroll = nullptr;
    brls::Box* m_sectionsBox = nullptr;

    // Playlists row
    brls::Box* m_playlistsRow = nullptr;
    brls::Box* m_playlistsContainer = nullptr;

    // Collections row
    brls::Box* m_collectionsRow = nullptr;
    brls::Box* m_collectionsContainer = nullptr;

    // Main content grid
    RecyclingGrid* m_contentGrid = nullptr;

    std::vector<LibrarySection> m_sections;  // Music sections only
    std::vector<MediaItem> m_items;
    std::vector<Playlist> m_playlists;        // Using new Playlist struct
    std::vector<MediaItem> m_collections;
    std::string m_currentSection;
    std::string m_currentPlaylistId;          // Currently viewing playlist
    bool m_loaded = false;
    bool m_viewingPlaylist = false;           // True if viewing playlist contents

    // Shared pointer to track if this object is still alive
    std::shared_ptr<bool> m_alive;
};

} // namespace vitaplex
