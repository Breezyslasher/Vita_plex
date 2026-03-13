/**
 * VitaPlex - Media Detail View
 * Shows detailed information about a media item
 */

#pragma once

#include <borealis.hpp>
#include <memory>
#include <atomic>
#include "app/plex_client.hpp"

namespace vitaplex {

class MediaDetailView : public brls::Box {
public:
    MediaDetailView(const MediaItem& item);
    ~MediaDetailView();

    static brls::View* create();

private:
    void loadDetails();
    void loadChildren();
    void loadMusicCategories();
    void loadTrackList();              // Load tracks in vertical list (like Suwayomi chapters)
    void loadExtras();                 // Load extras (trailers, featurettes, etc.)
    void onPlay(bool resume = false);
    void onDownload();
    void showDownloadOptions();
    void downloadAll();
    void downloadUnwatched(int maxCount = -1);
    void toggleDescription();          // Collapse/expand description
    void showAlbumContextMenu(const MediaItem& album);  // Context menu for albums
    void showMovieContextMenu(const MediaItem& movie);  // Context menu for movies
    void showShowContextMenu(const MediaItem& show);    // Context menu for TV shows

public:
    // Static context menus callable from any view (home, search, library grid, etc.)
    static void showMovieContextMenuStatic(const MediaItem& movie);
    static void showShowContextMenuStatic(const MediaItem& show);
    static void showSeasonContextMenuStatic(const MediaItem& season);
    static void showArtistContextMenuStatic(const MediaItem& artist);
    static void showAlbumContextMenuStatic(const MediaItem& album);
    static void performTrackActionStatic(const MediaItem& track);
    void performTrackAction(const MediaItem& track, size_t trackIndex);  // Handle track default action
    void showTrackActionDialog(const MediaItem& track, size_t trackIndex);  // Ask user what to do

    brls::HScrollingFrame* createMediaRow(const std::string& title, brls::Box** contentOut);

    MediaItem m_item;
    std::vector<MediaItem> m_children;

    // Main layout
    brls::ScrollingFrame* m_scrollView = nullptr;
    brls::Box* m_mainContent = nullptr;

    brls::Label* m_titleLabel = nullptr;
    brls::Label* m_yearLabel = nullptr;
    brls::Label* m_ratingLabel = nullptr;
    brls::Label* m_durationLabel = nullptr;
    brls::Label* m_summaryLabel = nullptr;
    brls::Image* m_posterImage = nullptr;
    brls::Button* m_playButton = nullptr;
    brls::Button* m_resumeButton = nullptr;
    brls::Button* m_downloadButton = nullptr;
    brls::Box* m_childrenBox = nullptr;

    // Track list for albums (vertical list with its own nested scroll)
    brls::Box* m_trackListBox = nullptr;

    // Collapsible description
    bool m_descriptionExpanded = false;
    std::string m_fullDescription;

    // Music category rows for artists
    brls::Box* m_musicCategoriesBox = nullptr;
    brls::Box* m_albumsContent = nullptr;
    brls::Box* m_singlesContent = nullptr;
    brls::Box* m_epsContent = nullptr;
    brls::Box* m_compilationsContent = nullptr;
    brls::Box* m_soundtracksContent = nullptr;

    // Extras (trailers, featurettes, deleted scenes)
    brls::Box* m_extrasBox = nullptr;

    // Track currently focused hint icon (like Suwayomi's m_currentFocusedIcon)
    brls::Image* m_currentFocusedHint = nullptr;
    brls::Label* m_currentFocusedHintLabel = nullptr;

    // Shared alive flag to prevent async callbacks from accessing destroyed view
    std::shared_ptr<std::atomic<bool>> m_alive;
};

} // namespace vitaplex
