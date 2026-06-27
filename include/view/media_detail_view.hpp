/**
 * VitaPlex - Media Detail View
 * Shows detailed information about a media item
 */

#pragma once

#include <borealis.hpp>
#include <memory>
#include <atomic>
#include <string>
#include <vector>
#include <functional>
#include "app/plex_client.hpp"

namespace vitaplex {

// One row in the anchored options popover (artboard "D4a"). Every menu that
// used to build a centered brls::Dialog + vertical brls::Button stack now
// translates each of its buttons into an OptionRow and feeds the whole vector
// to MediaDetailView::showOptionsPopover(). Presentation only — the `action`
// is the verbatim former button body (sans the leading dialog->dismiss()).
struct OptionRow {
    std::string icon;     // resources/icons asset key (e.g. "play.png")
    std::string label;
    std::string sub;      // optional trailing mono value ("", "from 12m", "00:00", "420 MB")
    bool primary = false; // gold fill (exactly one: the play/resume action)
    bool danger  = false; // muted (Cancel)
    std::function<bool(brls::View*)> action;
};

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
    void loadRecommendations();        // Load related / recommended titles (movies + shows)
    void loadPeople();                 // Load cast & crew row (movies + shows)
    // Build the (initially hidden) Cast & Crew + Recommended rows into `parent`.
    // Shared by movies (page scroll) and shows (inner scroll).
    void buildPeopleAndRecommendedRows(brls::Box* parent);
    // Direction B (poster-left) two-column layout for the movie / standalone-item
    // path. Creates the poster, stream rows, title/meta/actions/summary and the
    // cast / recommended rows, reusing every existing handler and async loader.
    void buildMovieLayout();
    // Direction A artist layout: fixed header (square art + star, ARTIST eyebrow,
    // name, genre/album/track meta, Play/Shuffle/Download/Add action row, bio)
    // above the type-grouped category rails (Albums, Singles & EPs, …). Reuses
    // loadDetails / loadMusicCategories and the existing member widgets.
    void buildArtistLayout();
    // Rebuild the artist meta line ("Anime · J-Pop · 12 albums · 140 tracks")
    // from m_item.genres + the cached album / track counts. Called as each piece
    // of data arrives (genres from loadDetails, counts from loadMusicCategories).
    void refreshArtistMeta();
    // Push a hero-header + poster-grid screen of a person's other titles in the
    // same library. excludeRatingKey is the title we came from, dropped from the
    // results so a person credited only on the current title shows a notification
    // instead of a one-item grid of that same title. personThumb (optional) is
    // the headshot shown in the hero; the credit kind (Actor/Director/Writer) is
    // derived from `filter`.
    static void showPersonResults(const std::string& personName,
                                  const std::string& sectionKey,
                                  const std::string& filter,
                                  const std::string& excludeRatingKey,
                                  const std::string& personThumb = "");
    void onPlay(bool resume = false);
    void onDownload();
    void showDownloadOptions();
    void downloadAll();
    void downloadUnwatched(int maxCount = -1);
    void setupChildrenFocusTransfer();  // Set up focus navigation for children items
    void showAlbumContextMenu(const MediaItem& album);  // Context menu for albums
    void showMovieContextMenu(const MediaItem& movie);  // Context menu for movies
    void showShowContextMenu(const MediaItem& show);    // Context menu for TV shows
    

    // Toggle the server-side watched / unwatched flag for this item.
    // Uses Plex's /:/scrobble and /:/unscrobble endpoints; the button
    // label flips immediately on success, no full reload required.
    void onToggleWatched();

    // Fetch the audio + subtitle streams Plex knows about for this part
    // and populate the AUDIO / SUBTITLES picker rows. Plex persists the
    // user's selection server-side per Part, so changes here carry over
    // automatically when the user finally hits Play.
    void loadStreams();
    void updateStreamRowLabels();
    void showAudioPicker();
    void showSubtitlePicker();
    // Merged audio + subtitle picker (one dialog, two tabs). defaultTab:
    // 0 = Audio, 1 = Subtitles. Both rows above open this same dialog.
    void showStreamDialog(int defaultTab);

public:
    // Static context menus callable from any view (home, search, library grid, etc.)
    static void showMovieContextMenuStatic(const MediaItem& movie);
    static void showShowContextMenuStatic(const MediaItem& show);
    static void showEpisodeContextMenu(const MediaItem& episode);
    static void showSeasonContextMenuStatic(const MediaItem& season);
    static void showArtistContextMenuStatic(const MediaItem& artist);
    static void showAlbumContextMenuStatic(const MediaItem& album);
    static void performTrackActionStatic(const MediaItem& track);
    static void showTrackContextMenuStatic(const MediaItem& track);

    // Shared builder for the compact options popover (artboard "D4a"). Anchors a
    // 320px panel to `anchor` (the focused cell, usually
    // brls::Application::getCurrentFocus()); falls back to a centered bottom
    // sheet when `anchor` is null or the screen is too narrow. Every show*
    // context menu funnels its rows through here. Static because several callers
    // are static members.
    // `scrollable` caps the panel to the screen height and puts the rows in a
    // ScrollingFrame — for long lists (e.g. the genre filter) that would
    // otherwise run off-screen. Default off, so every existing caller is
    // byte-for-byte unchanged.
    static void showOptionsPopover(brls::View* anchor,
                                   const std::string& contextLine,
                                   const std::string& title,
                                   std::vector<OptionRow> rows,
                                   bool scrollable = false);

    // Centered translucent choice dialog styled like the audio/subtitle picker
    // (dark panel, scrim, rounded rows). Each OptionRow becomes a row; clicking
    // it dismisses the dialog then runs the row's action. Used for the
    // SyncLounge auto-join prompt (and other party notices).
    static void showCenteredChoice(const std::string& title,
                                   const std::string& subtitle,
                                   std::vector<OptionRow> rows);

    void performTrackAction(const MediaItem& track, size_t trackIndex);  // Handle track default action
    void showTrackActionDialog(const MediaItem& track, size_t trackIndex);  // Ask user what to do

    // Build a labeled square-cover rail into m_musicCategoriesBox (artist detail).
    // count >= 0 renders a muted "(count)" beside the title; pass -1 to omit it.
    brls::HScrollingFrame* createMediaRow(const std::string& title, int count,
                                          brls::Box** contentOut);

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
    // Toggleable mark-watched / mark-unwatched action. Label switches
    // depending on m_item.watched, which itself is updated locally after
    // a successful PUT to /:/scrobble or /:/unscrobble.
    brls::Button* m_markWatchedButton = nullptr;
    // Icon overlays positioned absolutely on top of the secondary
    // action buttons. Pointers are kept around so onToggleWatched()
    // can flip the watched glyph (filled / outline) on click.
    brls::Image* m_downloadIcon = nullptr;
    brls::Image* m_markWatchedIcon = nullptr;
    // Audio button's leading icon — swapped to the surround-sound channel-layout
    // glyph (2.0 / 3.1 / 5.1 / 5.1.2 / 7.1) matching the selected audio track so
    // the user can see the format at a glance.
    brls::Image* m_audioIcon = nullptr;
    // AUDIO / SUBTITLES pickers, only created for playable items
    // (movies / episodes / extras). Hidden until loadStreams() resolves.
    brls::Button* m_audioRow = nullptr;
    brls::Button* m_subtitleRow = nullptr;
    int m_partId = 0;                       // Set by loadStreams()
    std::vector<PlexStream> m_streams;      // Populated by loadStreams()
    brls::Box* m_childrenBox = nullptr;
    brls::Label* m_childrenLabel = nullptr;
    brls::HScrollingFrame* m_childrenScroll = nullptr;

    // Track list for albums (vertical list with its own nested scroll)
    brls::Box* m_trackListBox = nullptr;

    // Description
    std::string m_fullDescription;
    brls::ScrollingFrame* m_summaryScroll = nullptr;   // Scroll frame for description

    // Artist detail meta line + cached counts (Direction A header).
    brls::Label* m_artistMetaLabel = nullptr;
    int m_artistAlbumCount = 0;
    int m_artistTrackCount = 0;
    // Clamp the summary to a fixed-height preview (artist detail bio) so a long
    // blurb can't push the rails off-screen; loadDetails honours this when the
    // full summary arrives.
    bool m_truncateSummary = false;

    // Music category rows for artists
    brls::Box* m_musicCategoriesBox = nullptr;
    brls::Box* m_albumsContent = nullptr;
    brls::Box* m_singlesContent = nullptr;
    brls::Box* m_epsContent = nullptr;
    brls::Box* m_compilationsContent = nullptr;
    brls::Box* m_soundtracksContent = nullptr;

    // Scrolling container for seasons+extras (prevents whole page from scrolling)
    brls::ScrollingFrame* m_mediaContentScroll = nullptr;
    brls::Box* m_mediaContentBox = nullptr;

    // Extras (trailers, featurettes, deleted scenes)
    brls::Label* m_extrasLabel = nullptr;
    brls::HScrollingFrame* m_extrasScroll = nullptr;
    brls::Box* m_extrasBox = nullptr;

    // Cast & crew row (movies)
    brls::Label* m_peopleLabel = nullptr;
    brls::HScrollingFrame* m_peopleScroll = nullptr;
    brls::Box* m_peopleBox = nullptr;

    // Recommended / related row (movies)
    brls::Label* m_recommendationsLabel = nullptr;
    brls::HScrollingFrame* m_recommendationsScroll = nullptr;
    brls::Box* m_recommendationsBox = nullptr;

    // Music videos row for artists
    brls::Box* m_musicVideosContent = nullptr;

    // Track currently focused hint icon (like Suwayomi's m_currentFocusedIcon)
    brls::Image* m_currentFocusedHint = nullptr;
    brls::Label* m_currentFocusedHintLabel = nullptr;

    // Shared alive flag to prevent async callbacks from accessing destroyed view
    std::shared_ptr<std::atomic<bool>> m_alive;
};

} // namespace vitaplex
