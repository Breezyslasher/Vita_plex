// VitaPlex - Player Activity: video/audio playback with controls and queue support.

#pragma once

#include <borealis.hpp>
#include <borealis/core/timer.hpp>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <unordered_map>
#include <map>
#include <chrono>
#include "app/plex_client.hpp"
#include "app/music_queue.hpp"

// Forward declarations
namespace vitaplex {
    class VideoView;
    struct MediaItem;
}

namespace vitaplex {

class PlayerActivity : public brls::Activity {
public:
    // Play from Plex server
    PlayerActivity(const std::string& mediaKey);

    // Play local downloaded file
    PlayerActivity(const std::string& mediaKey, bool isLocalFile);

    // Play direct file path (for debug/testing)
    static PlayerActivity* createForDirectFile(const std::string& filePath);

    // Play a direct stream URL (Live TV HLS). With liveSessionUuid set, timeline keep-alives hold the grab open.
    static PlayerActivity* createForStream(const std::string& streamUrl, const std::string& title,
                                           const std::string& liveSessionUuid = "");

    // Play from a queue: a server play queue when online, else client-side. userPickedTrack and playlistId matter only for shuffle and for naming a playlist source (see DESIGN_NOTES).
    static PlayerActivity* createWithQueue(const std::vector<MediaItem>& tracks, int startIndex = 0,
                                           bool userPickedTrack = true,
                                           const std::string& playlistId = "");

    // Resume existing queue (return to player without resetting queue)
    static PlayerActivity* createResumeQueue();

    // True while a player is on screen; the SyncLounge prompt skips prompting when we are already watching.
    static bool isActive();

    brls::View* createContentView() override;

    void onContentAvailable() override;

    void willDisappear(bool resetState) override;

private:
    // Android direct-surface: unpaint the frame so MpvSurface composites through during video. No-op elsewhere.
    static void setBackgroundTransparent(bool transparent);

    void loadMedia();
    void loadFromQueue();           // Load current track from queue
    void updateProgress();
    void togglePlayPause();
    void seek(int seconds);
    // Transcode-aware seeking: skips debounce into one absolute target, then seek locally or restart the transcode (see DESIGN_NOTES).
    void requestTranscodeSeek(double absMs);   // arm/refresh the debounce
    void commitTranscodeSeek();                 // fired by m_seekCommitTimer
    void showSeekPreview(double absMs, double totalMs);
    // Authoritative media length: Plex's duration when known, else baseOffset + mpv's. Scales and clamps the seek bar.
    double knownDurationMs() const;
    // Restart the transcode at offsetMs, for far seeks and corrupt streams. False if the new URL could not be fetched.
    bool restartTranscodeAtMs(int offsetMs);

    // Announce a manual play/pause/seek so the party follows. User-driven paths only; the follow loop drives mpv directly.
    void syncLoungeReportUserAction(const std::string& state, double absTimeMs);

    // Controls visibility toggle (like Suwayomi reader settings)
    void toggleControls();
    void showControls();
    void hideControls();
    void resetControlsIdleTimer();  // Reset inactivity timer on user input
    bool m_controlsVisible = true;
    int m_controlsIdleSeconds = 0;  // Seconds since last user interaction

    // Re-size the album cover for the viewport: the XML's 220x220 floats lost on a portrait phone.
    void applyMusicLayoutForViewport();

    // Queue controls
    void playNext();
    void playPrevious();
    void toggleShuffle();
    void toggleRepeat();
    void setShuffleFromOs(bool on);     // OS media controls set an explicit shuffle state
    void setRepeatFromOs(RepeatMode mode); // OS media controls set an explicit repeat mode
    void updateShuffleIcon();       // Update shuffle button icon based on state
    void updateRepeatIcon();        // Update repeat button icon based on state
    void onTrackEnded(const QueueItem* nextTrack);  // Called when track ends
    void updateQueueDisplay();      // Update UI with queue info
    void playNextEpisode();         // Auto-play next episode in season/show

    // Queue list overlay (Direction-A side sheet)
    void showQueueOverlay();
    void hideQueueOverlay();
    void populateQueueList();       // Build queue list with cover art and titles
    void playFromQueue(int index);  // Play a specific track from queue list
    // Lyrics are drawn by the app: music plays with vo=null, so a subtitle handed to mpv has no surface to land on.
    const PlexStream* findSideloadableStream(int trackId) const;
    void loadAndShowLyrics(const PlexStream& stream);
    void showLyricsMessage(const std::string& text);   // sheet with a reason, not a song
    void showLyricsOverlay();
    void hideLyricsOverlay();
    void buildLyricsRows();
    void syncLyricsToPosition();   // highlight + scroll the line that is playing

    std::vector<LyricLine> m_lyrics;
    std::vector<brls::Label*> m_lyricRows;
    int  m_lyricsIndex = -1;            // row currently highlighted, -1 = none
    bool m_lyricsOverlayVisible = false;
    bool m_lyricsLoading = false;
    bool m_lyricsFailed = false;    // sheet is showing a reason, not a song
    // Ticks only while the sheet is open; the 1s player timer visibly lags the vocal.
    brls::RepeatingTimer m_lyricsTimer;
    // End-of-track watcher at 4Hz: mpv's events are pumped once a second, so ENDED arrived up to a second late.
    brls::RepeatingTimer m_endWatchTimer;

    // Which layout was built. Two flags, never both true: m_mobileLayout is music's portrait screen, m_videoOsd the landscape OSD.
    bool m_mobileLayout = false;
    bool m_videoOsd     = false;
    // Both read m_isQueueMode, so they can only be asked once the activity exists.
    bool useMobileLayout() const;   // reads the Player layout setting
    bool useVideoOsd() const;       // reads the Video player layout setting
    bool controlsCanHide() const;   // false for photos and for music
    // The collapsed queue sheet; its views exist only in player_mobile.xml and are found by id, so no stubs are needed.
    void updateMobileSheet();        // refresh "up next" from the queue
    void wireMobileSheet();          // tap-to-open, once at build time
    bool mobileSheetFits() const;    // false on a screen too short to spare it
    // The video OSD's own controls, likewise present in one layout only and found by id.
    void wireVideoOsd();             // once at build time
    void updateVideoOsd();           // pill text and icon state, per tick
    void setVideoOsdChromeVisible(bool visible);   // top bar + bottom scrim
    // The OSD is drawn for a landscape phone, so it scales on a screen with far more height per unit of width.
    float videoOsdScale() const;
    void  applyVideoOsdForViewport();
    float m_videoOsdScale = 0.0f;    // last factor applied; 0 = not yet
    void cycleSpeed();               // 1.0 -> 1.25 -> 1.5 -> 2.0 -> 0.75 -> ...
    // Set together on the top bar: the show over the episode, not the single line the other layouts print.
    void setVideoOsdTitle(const std::string& title, const std::string& subtitle);
    // Scale factor for code-built rows shared with the mobile XML; the two mobile designs use different frames (see DESIGN_NOTES).
    float ui(float v) const {
        if (m_mobileLayout) return v * kMobileUiScale;
        if (m_videoOsd)     return v * kVideoUiScale;
        return v;
    }
    // Row geometry wants a gentler factor than type: at full scale a queue row is ~161 units and only two or three fit.
    float uiRow(float v) const { return m_mobileLayout ? v * kMobileRowScale : v; }
    static constexpr float kMobileUiScale  = 1280.f / 412.f;
    static constexpr float kMobileRowScale = 2.15f;
    static constexpr float kVideoUiScale   = 1280.f / 915.f;
    // The handoff's 915x412 landscape frame is 1280x576 here, so 576 is the short edge its sizes were drawn against.
    static constexpr float kVideoOsdBaseHeight = 412.f * kVideoUiScale;
    // Queue row height and gap. Every scroll clamp and reorder index must agree, so go through queueRowPitch().
    static constexpr float kQueueRowH   = 52.0f;
    static constexpr float kQueueRowGap = 2.0f;
    float queueRowPitch() const { return uiRow(kQueueRowH) + uiRow(kQueueRowGap); }
    // How far the queue can scroll, read from the measured content height so it matches ScrollingFrame's own limit.
    float queueMaxScroll();
    // Heights the mobile layout budgets against, matching player_mobile.xml.
    static constexpr float kMobileChrome      = 910.f;
    static constexpr float kMobileSheetHeight = 348.f;
    static constexpr float kMobileMinCover    = 450.f;
    std::string m_sheetThumbKey;     // cover already loaded, to avoid refetching

    // Next-track prefetch, keyed on (ratingKey, queue version); an empty URL records a failed attempt (see DESIGN_NOTES).
    void prefetchNextTrack();
    std::string m_prefetchKey;       // ratingKey the cached URL belongs to
    std::string m_prefetchUrl;       // empty = resolve was attempted and failed
    std::string m_prefetchSession;   // transcode session negotiated for that URL
    uint32_t m_prefetchVersion = 0;  // MusicQueue version the entry was built at
    int64_t  m_prefetchAtMs = 0;     // when the entry was resolved
    // Plex reaps a transcode session nobody streams, and a stale one is answered 400, so re-resolve past this age.
    static constexpr int64_t kPrefetchMaxAgeMs = 60000;
    bool m_prefetchInFlight = false;

    void updateNowPlayingBlock();   // Refresh the "Now Playing" header from the current track
    void clearUpcoming();           // Remove every track after the current one
    void removeFocusedQueueTrack(); // Remove the track for the focused up-next row
    void removeQueueTrackByIndex(int trackIdx);  // Shared remove (server sync + rebuild)
    void moveFocusedQueueTrack(int direction);  // -1 = up, +1 = down (LB/RB)
    // Controller reorder: START grabs the focused track, up/down move it, A drops it — for remotes with no bumpers.
    void toggleQueueGrab();         // pick up the focused track / drop it
    void setQueueGrab(bool on);     // enter/leave move mode + update the row cue
    // Pickup cue: slide the row out with a small overshoot and a shadow, then settle it back on drop.
    void animateGrabLift(bool lifted);
    void linkFirstRowToClear();     // route UP off the first up-next row to the Clear button
    // Keep UP-escape routes correct after an in-place swap; borealis has no route-erase, so re-point explicitly.
    void refixQueueUpRoutes(int lo);
    // Scroll a row into view after an in-place move, where giveFocus is a no-op because focus never left.
    void scrollQueueToChild(int idx);
    bool m_queueOverlayVisible = false;
    bool m_queueGrabActive = false;     // a track is "picked up" for Up/Down reorder
    bool m_queuePopulating = false;     // Guard against re-entrant populateQueueList
    uint32_t m_cachedQueueVersion = 0; // Queue version when rows were last built (0 = never)
    // Current-track index the list was built for; playTrack/playNext do not bump the version, so this is watched too.
    int m_lastRenderedCurrentIndex = -2;
    // Row that owns focus in the up-next list, for its remove affordance and restoring the previous row's styling.
    brls::Box* m_focusedQueueRow = nullptr;
    // Drives the pickup lift (0 seated, 1 lifted), mapped to translationX on the focused row each tick.
    brls::Animatable m_grabLift;
    static constexpr float kGrabLiftPx = 14.0f;  // how far the held row slides out
    // Child index to focus after the next rebuild, so a reorder keeps the hover on the moved track. -1 once consumed.
    int m_queueFocusTargetChild = -1;

    // Windowed rendering: only build rows around the current track, so a huge queue cannot spawn thousands of views.
    static constexpr int QUEUE_RENDER_LIMIT = 60;  // Max rows to create at once
    static constexpr int QUEUE_EXPAND_CHUNK = 20;   // Rows to add when expanding window
    static constexpr int QUEUE_EXPAND_TRIGGER = 5;   // Expand when focus is within this many rows of edge
    static constexpr int QUEUE_EXPAND_BATCH = 4;     // Rows to create per frame during async expansion
    int m_queueWindowStart = 0;     // First queue display index in the rendered window
    int m_queueWindowEnd = 0;       // One past last queue display index in the window
    int m_queueTotalCount = 0;      // Total queue items
    void expandQueueWindow(int direction);  // +1 = expand down, -1 = expand up
    // Async expansion state - creates rows across frames to avoid freezing
    bool m_expandActive = false;
    int m_expandNext = 0;           // Next queue display index to create
    int m_expandEnd = 0;            // One past last index to create
    void expandQueueBatch();        // Create next batch of expansion rows

    // Batched queue population - creates rows across multiple frames to avoid UI freeze
    static constexpr int QUEUE_BATCH_SIZE = 12;  // Rows to create per frame (keep low for Vita perf)
    int m_queueBatchNext = 0;                    // Next row index to create
    int m_queueBatchTotal = 0;                   // Total rows to create
    bool m_queueBatchActive = false;             // Whether batched creation is in progress
    std::vector<QueueItem> m_queueBatchTracks;   // Snapshot of tracks for batched creation
    std::vector<int> m_queueBatchShuffleOrder;   // Snapshot of shuffle order
    int m_queueBatchCurrentIndex = 0;            // Current track index snapshot
    bool m_queueBatchShuffled = false;           // Shuffle state snapshot
    void populateQueueBatch();                   // Create next batch of rows
    void createQueueRow(int displayIdx, int trackIdx, const QueueItem& track, bool isCurrent);

    // Maps row views to track indices, so gesture handlers look up the current position rather than a stale capture.
    struct QueueRowData {
        int trackIdx;
        std::string title;
        brls::Box* removeBtn = nullptr;  // the ✕ affordance, shown only while focused
    };
    std::unordered_map<brls::View*, QueueRowData> m_queueRowData;

    // Lazy thumbnail loading for queue rows - only loads covers for visible rows instead of all tracks at once
    struct DeferredThumb {
        brls::Image* image;
        std::string thumbPath;      // Raw Plex thumb path (resolved lazily)
        std::string ratingKey;      // For checking local downloads lazily
        bool loaded;
    };
    std::vector<DeferredThumb> m_deferredThumbs;
    void loadQueueThumbsAroundIndex(int displayIndex);
    static constexpr int QUEUE_THUMB_BUFFER = 6;  // Load this many rows above/below visible

    // Helper to find a row's current display position in the queue list
    int findQueueRowDisplayIndex(brls::View* row);
    // Swap two adjacent queue rows visually (no rebuild)
    void swapQueueRows(int displayIdxA, int displayIdxB, bool skipThumbReload = false);
    // Bulk-reassign all rows in [origIdx..targetIdx] from queue data (O(range) not O(n) swaps)
    void reassignQueueRange(int origIdx, int targetIdx);
    // Renumber all queue row labels after a reorder
    void renumberQueueRows();
    // Remove a single queue row from the display (no rebuild)
    void removeQueueRow(int displayIdx);
    // Update queue overlay title with current track count/duration
    void updateQueueTitle();

    // Drag-to-reorder state: hold delay + live row movement
    struct DragState {
        bool active = false;                 // Whether a drag is in progress
        brls::View* draggedRow = nullptr;     // The row being dragged
        int originalDisplayIdx = -1;         // Display index where drag started
        int targetDisplayIdx = -1;           // Current target drop position
        int draggedTrackIdx = -1;            // Queue index of the track being dragged
        std::chrono::steady_clock::time_point holdStart;  // When touch began
        bool holdMet = false;                 // Whether hold threshold was met
        bool justEnded = false;              // Suppress tap/click right after drag ends
        bool scrollPassthrough = false;      // True when forwarding touch as scroll (hold not met)
        float initialScrollY = 0.0f;         // ScrollingFrame offset when touch began
        float dragStartY = 0.0f;             // Finger Y when drag mode activated (for row translation)
        float dragStartScrollY = 0.0f;       // Scroll offset when drag mode activated
        float scrollViewTop = 0.0f;          // Scroll view's absolute Y on screen (computed at drag start)
    };
    DragState m_dragState;
    static constexpr int HOLD_THRESHOLD_MS = 200;  // ms to hold before drag starts
    static constexpr float ROW_HEIGHT_PX = 62.0f;  // Approx row height for swap threshold

    std::string m_mediaKey;
    std::string m_directFilePath;  // For direct file playback (debug) or stream URL
    std::string m_streamTitle;     // Title for stream playback (Live TV)
    std::string m_liveSessionUuid; // Set for Live TV streams; triggers periodic
                                   // /:/timeline keep-alive pings in updateProgress
    int m_liveKeepaliveCounter = 0;  // Seconds since last live-TV keep-alive
    MediaType m_mediaType = MediaType::UNKNOWN;  // Type of media being played
    // OS media session metadata for video, which has no queue to read it from. Android only; MPRIS and SMTC are unchanged.
    bool m_refreshRateApplied = false;
    void applyContentRefreshRate();

    std::string m_osArtUrl;    // poster / episode still, already a full URL or file path
    std::string m_osArtist;    // show title for episodes, year for movies
    std::string m_osAlbum;     // "S2 - E4" for episodes, empty otherwise

    std::string m_parentRatingKey;  // Season/album ratingKey for auto-play-next
    std::string m_grandparentRatingKey;  // Show ratingKey for cross-season auto-play-next
    int m_episodeIndex = 0;         // Episode index within season for auto-play-next
    bool m_endHandled = false;      // Prevent multiple triggers when playback ends
    bool m_isPlaying = false;
    bool m_isPhoto = false;
    bool m_isLocalFile = false;    // Playing from local download
    bool m_isDirectFile = false;   // Playing direct file path (debug)
    // True when the server chose direct play: mpv owns the timeline, so baseOffset is 0 and seeks are local.
    bool m_directPlay = false;
    bool m_isQueueMode = false;    // Playing from queue
    bool m_isResuming = false;     // Resuming existing playback (don't restart track)
    // lyrics support removed
    bool m_destroying = false;     // Flag to prevent timer callbacks during destruction
    bool m_loadingMedia = false;   // Flag to prevent rapid re-entry of loadMedia
    bool m_wasForeground = true;   // Tracks app foreground state to detect a
                                   // Background to foreground on mobile: a cover loaded while hidden failed to upload, so re-load it once visible.
    bool m_videoOsActive = false;      // OS media session wired up for video playback
    bool m_lastVideoOsPlaying = false; // last play-state pushed to the video session

    // OS media controls for video; music goes through MusicController. No-ops where the platform has no session.
    void setupVideoMediaSession();
    void publishVideoNowPlaying();
    double m_pendingSeek = 0.0;    // Pending seek position (set when resuming)
    int m_transcodeBaseOffsetMs = 0;  // Base offset (ms) used to start current transcode
    int m_mediaDurationMs = 0;        // Full media length (ms) from Plex metadata; 0 = unknown
    // Track length handed to loadUrl so an early end-of-stream is told from a real one; 0 means unknown.
    int64_t m_pendingDurationMs = 0;
    bool m_updatingSlider = false;  // Guard to prevent slider update from triggering seek
    brls::RepeatingTimer m_updateTimer;
    // Seek debounce: each input rewinds it, and one seek commits ~350ms after the last. Target < 0 means none pending.
    brls::Timer m_seekCommitTimer;
    double m_seekTargetMs = -1.0;
    int m_timelineCounter = 0;           // Seconds since last timeline report
    std::string m_lastTimelineState;     // Last reported state to detect changes

    // Last drift-correcting seek. An HLS transcode restarts on seek and takes seconds to settle, so these are rate-limited.
    std::chrono::steady_clock::time_point m_lastSyncSeek{};
    // Consecutive restarts from a corrupt position, capped so an unrecoverable stream cannot restart forever.
    int m_syncRecoverAttempts = 0;
    // ratingKey we last auto-loaded to follow the host, so the update loop cannot re-trigger a reload mid-load.
    std::string m_syncLoungeContentKey;
    // Whether the current media has been announced to the room; reset per loadMedia so the party shows our title.
    bool m_syncLoungeAnnounced = false;
    // Whether announcing claims host: true for a user-opened item, false when auto-loading to follow.
    bool m_syncLoungeClaimHostOnAnnounce = true;
    // Last party-pause sequence applied, so each inbound action pauses or resumes exactly once.
    int m_lastPartyPauseSeq = 0;

    // Diagnostic panel, created lazily when showMpvStats is on and refreshed once a second.
    brls::Box*   m_mpvStatsBox   = nullptr;
    brls::Label* m_mpvStatsLabel = nullptr;
    void updateMpvStatsOverlay();

    // Deferred MPV init: stored during onContentAvailable, loaded on the first tick, to avoid the GXM/NanoVG conflict.
    std::string m_pendingPlayUrl;
    std::string m_pendingPlayTitle;
    bool m_pendingIsAudio = false;

    // Alive flag for async image loads - prevents use-after-free when activity is destroyed
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);

    // Set while a player activity is on screen (see isActive()).
    static std::atomic<bool> s_active;

    // Track cycling
    void cycleAudioTrack();
    void cycleSubtitleTrack();
    void updatePlayPauseLabel();

    // Track selection overlay
    enum class TrackSelectMode { NONE, AUDIO, SUBTITLE, VIDEO };
    TrackSelectMode m_trackSelectMode = TrackSelectMode::NONE;
    std::vector<PlexStream> m_plexStreams;  // Cached streams from Plex
    int m_partId = 0;                       // Plex part ID for stream selection
    bool m_streamsLoaded = false;
    int m_selectedTrackIndex = 0;  // Index of selected item in track list for focus

    void showTrackOverlay(TrackSelectMode mode);
    void hideTrackOverlay();
    // What the OSD's audio and subtitle pills say after the name — the selected track, hence pills and not glyphs.
    std::string trackSummary(TrackSelectMode mode) const;
    void populateTrackList(TrackSelectMode mode);
    void populateSubtitleSearchResults();
    void selectTrack(TrackSelectMode mode, int index);  // index into filtered list, -1 = off for subs
    void fetchPlexStreams();
    std::vector<PlexClient::SubtitleResult> m_subtitleSearchResults;

    // Intro/credits skip
    std::vector<MediaItem::Marker> m_markers;
    void updateSkipButton(double positionMs);
    void skipToMarkerEnd();
    std::string m_activeMarkerType;        // Currently active marker type ("intro"/"credits"), empty if none
    int m_activeMarkerEndMs = 0;           // End time of the active marker
    int m_skipButtonShowSeconds = 0;       // Seconds the skip button has been visible
    bool m_skipButtonVisible = false;      // Whether skip button is currently shown
    bool m_introSkipped = false;           // Whether intro was already auto-skipped this playback
    bool m_creditsSkipped = false;         // Whether credits was already auto-skipped this playback

    BRLS_BIND(brls::Box, playerContainer, "player/container");
    BRLS_BIND(brls::Label, titleLabel, "player/title");
    BRLS_BIND(brls::Label, artistLabel, "player/artist");
    BRLS_BIND(brls::Label, timeLabel, "player/time");
    BRLS_BIND(brls::Label, timeElapsedLabel, "player/time_elapsed");
    BRLS_BIND(brls::Label, timeRemainingLabel, "player/time_remaining");
    BRLS_BIND(brls::Label, queueLabel, "player/queue_info");
    BRLS_BIND(brls::Slider, progressSlider, "player/progress");
    BRLS_BIND(brls::Box, controlsBox, "player/controls");
    BRLS_BIND(brls::Box, centerControls, "player/center_controls");
    BRLS_BIND(brls::Image, photoImage, "player/photo");
    BRLS_BIND(brls::Box, albumArtContainer, "player/album_art_container");
    BRLS_BIND(brls::Image, albumArt, "player/album_art");
    BRLS_BIND(VideoView, videoView, "player/video");
    BRLS_BIND(brls::Image, playPauseIcon, "player/play_pause_icon");
    BRLS_BIND(brls::Image, audioIcon, "player/audio_icon");
    BRLS_BIND(brls::Image, subtitleIcon, "player/sub_icon");
    BRLS_BIND(brls::Image, rewindIcon, "player/rewind_icon");
    BRLS_BIND(brls::Image, forwardIcon, "player/forward_icon");
    BRLS_BIND(brls::Box, playBtn, "player/play_btn");
    BRLS_BIND(brls::Box, rewindBtn, "player/rewind_btn");
    BRLS_BIND(brls::Box, forwardBtn, "player/forward_btn");
    BRLS_BIND(brls::Box, audioBtn, "player/audio_btn");
    BRLS_BIND(brls::Box, subBtn, "player/sub_btn");
    BRLS_BIND(brls::Box, videoBtn, "player/video_btn");
    BRLS_BIND(brls::Image, videoIcon, "player/video_icon");
    BRLS_BIND(brls::Box, pipBtn, "player/pip_btn");
    BRLS_BIND(brls::Image, pipIcon, "player/pip_icon");
    BRLS_BIND(brls::Box, trackOverlay, "player/track_overlay");
    BRLS_BIND(brls::Label, trackOverlayTitle, "player/track_overlay_title");
    BRLS_BIND(brls::Box, trackList, "player/track_list");
    BRLS_BIND(brls::Box, skipBtn, "player/skip_btn");
    BRLS_BIND(brls::Label, skipLabel, "player/skip_label");
    BRLS_BIND(brls::Box, lyricsBtn, "player/lyrics_btn");
    BRLS_BIND(brls::Image, lyricsIcon, "player/lyrics_icon");
    BRLS_BIND(brls::Box, queueBtn, "player/queue_btn");
    BRLS_BIND(brls::Image, queueIcon, "player/queue_icon");
    // Keep buttons in hidden containers out of the focus order: isFocusable() tests only the view's own visibility.
    void syncHiddenFocus();
    // Every icon and the resource behind it, so a swap dropped with no GL surface or a lost texture can be re-issued (see DESIGN_NOTES).
    std::map<brls::Image*, std::string> m_iconRes;
    bool m_uploadsWereSafe = true;
    void registerIcons();
    void setIconRes(brls::Image* img, const std::string& res);
    void reapplyIcons();

    // Raw keyboard hook for keys with no gamepad equivalent; dropped in willDisappear, as the event outlives us.
    brls::InputManager* m_inputManager = nullptr;
    brls::Event<brls::KeyState>::Subscription m_kbSub;
    bool m_kbSubscribed = false;

    // Inert until onContentAvailable has wired the D-pad routes; running earlier aborts the process.
    bool m_focusWiringDone = false;

    BRLS_BIND(brls::Box, lyricsOverlay, "player/lyrics_overlay");
    BRLS_BIND(brls::Box, lyricsScrim, "player/lyrics_scrim");
    BRLS_BIND(brls::ScrollingFrame, lyricsScroll, "player/lyrics_scroll");
    BRLS_BIND(brls::Box, lyricsList, "player/lyrics_list");
    BRLS_BIND(brls::Label, lyricsOverlayTitle, "player/lyrics_overlay_title");
    BRLS_BIND(brls::Box, queueOverlay, "player/queue_overlay");
    BRLS_BIND(brls::Box, queueScrim, "player/queue_scrim");
    BRLS_BIND(brls::Label, queueOverlayTitle, "player/queue_overlay_title");
    BRLS_BIND(brls::Box, queueList, "player/queue_list");
    BRLS_BIND(brls::ScrollingFrame, queueScroll, "player/queue_scroll");
    // Now Playing block + Up Next header
    BRLS_BIND(brls::Box, queueNowPlaying, "player/queue_now_playing");
    BRLS_BIND(brls::Image, queueNpThumb, "player/queue_np_thumb");
    BRLS_BIND(brls::Label, queueNpTitle, "player/queue_np_title");
    BRLS_BIND(brls::Label, queueNpArtist, "player/queue_np_artist");
    BRLS_BIND(brls::Label, queueNpLabel, "player/queue_np_label");
    BRLS_BIND(brls::Label, queueUpNextLabel, "player/queue_upnext_label");
    BRLS_BIND(brls::Box, queueClearBtn, "player/queue_clear_btn");

    // Music-specific UI elements
    BRLS_BIND(brls::Box, musicInfo, "player/music_info");
    BRLS_BIND(brls::Label, musicTitleLabel, "player/music_title");
    BRLS_BIND(brls::Label, musicArtistLabel, "player/music_artist");
    BRLS_BIND(brls::Box, musicTransport, "player/music_transport");
    BRLS_BIND(brls::Box, musicPlayBtn, "player/music_play_btn");
    BRLS_BIND(brls::Image, musicPlayIcon, "player/music_play_icon");
    // Bound only so their resource can be re-applied after a lost GL context; neither is ever swapped at runtime.
    BRLS_BIND(brls::Image, musicPrevIcon, "player/music_prev_icon");
    BRLS_BIND(brls::Image, musicNextIcon, "player/music_next_icon");
    BRLS_BIND(brls::Box, musicPrevBtn, "player/music_prev_btn");
    BRLS_BIND(brls::Box, musicNextBtn, "player/music_next_btn");
    // lyrics button removed
    BRLS_BIND(brls::Box, shuffleBtn, "player/shuffle_btn");
    BRLS_BIND(brls::Image, shuffleIcon, "player/shuffle_icon");
    BRLS_BIND(brls::Box, repeatBtn, "player/repeat_btn");
    BRLS_BIND(brls::Image, repeatIcon, "player/repeat_icon");
};

} // namespace vitaplex
