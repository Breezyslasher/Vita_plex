/**
 * VitaPlex - Player Activity
 * Video/audio playback screen with controls and queue support
 */

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

    // Play a direct stream URL (for Live TV HLS streams).
    // liveSessionUuid (optional) is the Plex Live TV session id; when set, the
    // activity sends periodic /:/timeline keep-alives so the server's 300-sec
    // rolling-subscription stop-timer doesn't kill playback after ~5 min.
    static PlayerActivity* createForStream(const std::string& streamUrl, const std::string& title,
                                           const std::string& liveSessionUuid = "");

    // Play from queue (album, playlist, etc.)
    // Automatically creates a server-side play queue when online,
    // falls back to client-side queue when offline
    // userPickedTrack says whether startIndex is a track the user chose or just
    // the top of a list they pressed Play on. It only matters when "Shuffle New
    // Queues" is on: a chosen track still plays first, while a container is
    // opened on a random one instead of always its first track.
    static PlayerActivity* createWithQueue(const std::vector<MediaItem>& tracks, int startIndex = 0,
                                           bool userPickedTrack = true);

    // Resume existing queue (return to player without resetting queue)
    static PlayerActivity* createResumeQueue();

    // True while a player activity is on screen. Used by the SyncLounge
    // auto-join prompt to skip prompting when we're already watching (the
    // in-player auto-load handles content changes instead).
    static bool isActive();

    brls::View* createContentView() override;

    void onContentAvailable() override;

    void willDisappear(bool resetState) override;

private:
    // Android direct-surface: flip the borealis frame-clear colour so the
    // SurfaceFlinger composites MpvSurface through the unpainted video
    // region during video playback, then restore the opaque dark
    // background when leaving the player or switching to audio-only.
    // No-op on every other platform.
    static void setBackgroundTransparent(bool transparent);

    void loadMedia();
    void loadFromQueue();           // Load current track from queue
    void updateProgress();
    void togglePlayPause();
    void seek(int seconds);
    // Transcode-aware seeking. On an HLS transcode, skips/scrubs are debounced
    // into a single absolute target (m_seekTargetMs): a backward or small-forward
    // jump that's already transcoded seeks locally, while a far-forward jump (or
    // one before the current transcode start) restarts the transcode at the
    // target so Plex re-encodes from there instead of mpv crawling across
    // un-transcoded segments. Direct play / local / music seek locally as before.
    void requestTranscodeSeek(double absMs);   // arm/refresh the debounce
    void commitTranscodeSeek();                 // fired by m_seekCommitTimer
    void showSeekPreview(double absMs, double totalMs);
    // Authoritative full media length in ms: Plex's item.duration when known
    // (stable across transcode restarts), else baseOffset + mpv duration. Used
    // as the seek-bar scale and the clamp bound so seeks can't run past the end.
    double knownDurationMs() const;
    // Stop the current transcode and start a fresh one at offsetMs. Used for far
    // seeks and to escape a corrupt stream that an mpv-local seek can't. Returns
    // false if the new transcode URL couldn't be fetched.
    bool restartTranscodeAtMs(int offsetMs);

    // SyncLounge: announce a manual play/pause/seek (state + absolute ms) so
    // the watch party follows, claiming host under auto-host. No-op when not
    // connected. Called from the user-driven control paths only — the follow
    // loop drives MpvPlayer directly, so this never fires for synced changes.
    void syncLoungeReportUserAction(const std::string& state, double absTimeMs);

    // Controls visibility toggle (like Suwayomi reader settings)
    void toggleControls();
    void showControls();
    void hideControls();
    void resetControlsIdleTimer();  // Reset inactivity timer on user input
    bool m_controlsVisible = true;
    int m_controlsIdleSeconds = 0;  // Seconds since last user interaction

    // Re-size the music-mode UI (album cover) to fit the current
    // viewport. Hard-coded XML cover size is 220×220 which floats lost
    // in a 720-wide portrait phone window; this picks the larger of
    // ~55% viewport width and 220 so the cover scales up gracefully on
    // bigger / portrait screens while staying its designed size on a
    // landscape Vita / Switch. Called when entering music mode and on
    // every viewport-orientation flip.
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
    // Lyrics. Drawn by the app, not by mpv: music plays with vo=null and no
    // render context, so a subtitle handed to mpv has no surface to land on.
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
    // Ticks only while the sheet is open. The 1s player timer is too coarse to
    // follow a lyric line without it visibly lagging the vocal.
    brls::RepeatingTimer m_lyricsTimer;
    // End-of-track watcher. mpv's events are pumped from updateProgress(), which
    // runs once a second, so the ENDED state was not even set until up to a
    // second after the audio stopped — and then handled on that same tick. This
    // pumps four times a second and hands over the moment the file ends.
    brls::RepeatingTimer m_endWatchTimer;

    // Which layout createContentView() built. The two XMLs declare the same
    // view ids, so almost nothing else needs to know — this is for the few
    // places where the geometry genuinely differs.
    bool m_mobileLayout = false;
    static bool useMobileLayout();   // reads the Player layout setting
    // The collapsed queue sheet along the bottom of the mobile layout. Its
    // views exist only in player_mobile.xml and are looked up by id rather than
    // bound, so the classic layout needs no stubs for them.
    void updateMobileSheet();        // refresh "up next" from the queue
    void wireMobileSheet();          // tap-to-open, once at build time
    bool mobileSheetFits() const;    // false on a screen too short to spare it
    // The queue / lyrics rows are built in code at sizes written for the
    // classic layout. The mobile layout's XML is scaled to a phone frame
    // (1280 logical units standing in for the handoff's 412), so anything it
    // shares has to be scaled the same way or it renders a third of the size.
    float ui(float v) const { return m_mobileLayout ? v * kMobileUiScale : v; }
    static constexpr float kMobileUiScale = 1280.f / 412.f;
    // Heights the mobile layout budgets against, in borealis logical units and
    // matching player_mobile.xml: everything below the cover, the collapsed
    // sheet, and the smallest cover worth calling a cover.
    static constexpr float kMobileChrome      = 910.f;
    static constexpr float kMobileSheetHeight = 348.f;
    static constexpr float kMobileMinCover    = 450.f;
    std::string m_sheetThumbKey;     // cover already loaded, to avoid refetching

    // Next-track prefetch. Resolving a Plex stream URL costs two blocking HTTP
    // round-trips (/library/metadata, then /decision). Doing them when the track
    // ends put both of them inside the silence between songs, and froze the UI
    // thread for their duration. This resolves the next track while the current
    // one is still playing, so auto-advance only has to hand mpv a URL.
    //
    // Invalidation is the pair (ratingKey, queue version): every queue mutation
    // bumps MusicQueue's version, so a reorder, add or remove simply makes the
    // cache stop matching and the blocking path runs as before. A cached entry
    // with an empty URL records a failed attempt, so a track whose resolve fails
    // is not retried every second.
    void prefetchNextTrack();
    std::string m_prefetchKey;       // ratingKey the cached URL belongs to
    std::string m_prefetchUrl;       // empty = resolve was attempted and failed
    std::string m_prefetchSession;   // transcode session negotiated for that URL
    uint32_t m_prefetchVersion = 0;  // MusicQueue version the entry was built at
    bool m_prefetchInFlight = false;

    void updateNowPlayingBlock();   // Refresh the "Now Playing" header from the current track
    void clearUpcoming();           // Remove every track after the current one
    void removeFocusedQueueTrack(); // Remove the track for the focused up-next row
    void removeQueueTrackByIndex(int trackIdx);  // Shared remove (server sync + rebuild)
    void moveFocusedQueueTrack(int direction);  // -1 = up, +1 = down (LB/RB)
    // Controller/remote reorder, mirroring the sidebar editor's grab model:
    // START (or Android TV hold-center) picks up the focused track, then
    // Up/Down move it and A/OK drops it. Gives bare D-pad remotes a way to
    // reorder without the L/R bumpers (which Android TV remotes lack).
    void toggleQueueGrab();         // pick up the focused track / drop it
    void setQueueGrab(bool on);     // enter/leave move mode + update the row cue
    // Animated pickup cue: slides the grabbed row out to the right with a small
    // overshoot "pop" and casts a shadow, then settles it back on drop. Makes
    // picking up a track read as a physical lift, not just a colour change.
    void animateGrabLift(bool lifted);
    void linkFirstRowToClear();     // route UP off the first up-next row to the Clear button
    // After an in-place reorder swaps rows around index `lo` (and lo+1), keep
    // the UP-escape routes correct: row 0 -> Clear, others -> the row above.
    // borealis has no route-erase, so we re-point explicitly rather than clear.
    void refixQueueUpRoutes(int lo);
    // Scroll the up-next list so the row at child index `idx` is visible. Used
    // after an in-place move, where giveFocus is a no-op (the row never lost
    // focus) and so wouldn't scroll the moved row into view on its own.
    void scrollQueueToChild(int idx);
    bool m_queueOverlayVisible = false;
    bool m_queueGrabActive = false;     // a track is "picked up" for Up/Down reorder
    bool m_queuePopulating = false;     // Guard against re-entrant populateQueueList
    uint32_t m_cachedQueueVersion = 0; // Queue version when rows were last built (0 = never)
    // Current-track index the up-next list was last built for. playTrack/playNext
    // don't bump the queue version, so the refresh loop watches this to rebuild
    // the sheet (Now Playing + Up Next) when the song advances.
    int m_lastRenderedCurrentIndex = -2;
    // Row that currently owns focus inside the up-next list — used to reveal its
    // remove (✕) affordance and restore the previous row's styling on focus move.
    brls::Box* m_focusedQueueRow = nullptr;
    // Drives the pickup "lift" animation (0 = seated in the list, 1 = lifted
    // out). Mapped to translationX on m_focusedQueueRow each tick.
    brls::Animatable m_grabLift;
    static constexpr float kGrabLiftPx = 14.0f;  // how far the held row slides out
    // When >= 0, the child index populateQueueList / the batched build should land
    // focus on after the next rebuild, instead of the default (first / current
    // row). Set by moveFocusedQueueTrack so a reorder keeps the hover on the moved
    // track even on large queues (which build asynchronously and would otherwise
    // re-focus the top row on completion). Reset to -1 once consumed.
    int m_queueFocusTargetChild = -1;

    // Windowed queue rendering - only create rows for a window around the current track
    // to avoid creating thousands of views for large queues
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

    // Queue row management - maps row views to their track indices
    // so gesture handlers can look up current position at interaction time
    // instead of relying on stale captured values
    struct QueueRowData {
        int trackIdx;
        std::string title;
        brls::Box* removeBtn = nullptr;  // the ✕ affordance, shown only while focused
    };
    std::unordered_map<brls::View*, QueueRowData> m_queueRowData;

    // Lazy thumbnail loading for queue rows - only loads covers for
    // visible rows instead of all tracks at once
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
    // Metadata for the OS media session while playing video. Music reads the
    // queue for this; video has no queue, so the details fetch stashes it here.
    // Only consumed on Android — the desktop MPRIS/SMTC publishes are unchanged.
    // Display-mode matching: applied once per file, from the first frame rate
    // mpv reports. Restored when the player leaves.
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
    // True when the server chose direct play and we're streaming the original
    // file (not an HLS transcode): mpv owns the timeline, so baseOffset is 0,
    // resume is an mpv seek, and seeks are local/instant rather than transcode
    // restarts. Set per-load from the URL getTranscodeUrl returns.
    bool m_directPlay = false;
    bool m_isQueueMode = false;    // Playing from queue
    bool m_isResuming = false;     // Resuming existing playback (don't restart track)
    // lyrics support removed
    bool m_destroying = false;     // Flag to prevent timer callbacks during destruction
    bool m_loadingMedia = false;   // Flag to prevent rapid re-entry of loadMedia
    bool m_wasForeground = true;   // Tracks app foreground state to detect a
                                   // background->foreground return (mobile): a
                                   // cover loaded while hidden fails to upload to
                                   // GL, so we re-load it once we're visible again.
    bool m_videoOsActive = false;      // OS media session wired up for video playback
    bool m_lastVideoOsPlaying = false; // last play-state pushed to the video session

    // OS media controls (media keys / overlay) for VIDEO playback. Music uses the
    // MusicController path; these mirror it for the non-queue (video) player so the
    // same keys control video. No-ops where the platform has no media session.
    void setupVideoMediaSession();
    void publishVideoNowPlaying();
    double m_pendingSeek = 0.0;    // Pending seek position (set when resuming)
    int m_transcodeBaseOffsetMs = 0;  // Base offset (ms) used to start current transcode
    int m_mediaDurationMs = 0;        // Full media length (ms) from Plex metadata; 0 = unknown
    bool m_updatingSlider = false;  // Guard to prevent slider update from triggering seek
    brls::RepeatingTimer m_updateTimer;
    // Debounce for transcode seeks: each skip/scrub rewinds it, and its end
    // callback commits one seek ~350 ms after the last input. m_seekTargetMs is
    // the pending absolute position in ms (< 0 means no seek pending).
    brls::Timer m_seekCommitTimer;
    double m_seekTargetMs = -1.0;
    int m_timelineCounter = 0;           // Seconds since last timeline report
    std::string m_lastTimelineState;     // Last reported state to detect changes

    // SyncLounge follow: last time we issued a drift-correcting seek to match
    // the room host. Seeking an HLS transcode restarts it and the position
    // takes several seconds to settle, so we rate-limit seeks to avoid a
    // per-second re-seek storm. Default-constructed (epoch) lets the first
    // correction fire immediately.
    std::chrono::steady_clock::time_point m_lastSyncSeek{};
    // Consecutive transcode restarts triggered by an insane (corrupt-stream)
    // local position while following. Capped so a stream that won't recover
    // doesn't restart forever; reset once the position reads sane again.
    int m_syncRecoverAttempts = 0;
    // ratingKey we last auto-loaded to follow the host's content, so the
    // updateProgress loop doesn't re-trigger a reload while it's loading.
    std::string m_syncLoungeContentKey;
    // Whether we've announced the current media to the SyncLounge room yet
    // (reset on each loadMedia). Announce-once so the party shows our title.
    bool m_syncLoungeAnnounced = false;
    // Whether announcing the current item should claim host (true only for a
    // user-initiated new video; set false when we auto-load to follow the
    // host, so following never steals host). Default true = user opened it.
    bool m_syncLoungeClaimHostOnAnnounce = true;
    // Last SyncLounge party-pause action sequence we applied, so each inbound
    // partyPause pauses/resumes the local player exactly once.
    int m_lastPartyPauseSeq = 0;

    // Diagnostic overlay panel — created lazily on first
    // updateMpvStatsOverlay() call when AppSettings::showMpvStats is on.
    // Lives at the top-left of playerContainer, refreshed once per
    // second from inside m_updateTimer's callback.
    brls::Box*   m_mpvStatsBox   = nullptr;
    brls::Label* m_mpvStatsLabel = nullptr;
    void updateMpvStatsOverlay();

    // Deferred MPV init: URL and title are stored here during onContentAvailable()
    // and loaded in the first updateProgress() call. This prevents GXM context
    // conflicts between MPV's render context creation / decoder threads and
    // NanoVG during the borealis activity show phase.
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
    // Keep buttons inside hidden containers out of the focus order. borealis'
    // View::isFocusable() only tests the view's own visibility, so a button in a
    // GONE box stays focusable and hit-testable, and the highlight lands on a
    // zero-sized view. Called whenever those containers change visibility.
    void syncHiddenFocus();
    // Every icon in the player and the resource behind it. Two problems need
    // this, both of them Android backgrounding:
    //
    //   - Image::setImageFromRes() uploads there and then, so a swap made with
    //     no GL surface (an OS notification toggling play/pause, shuffle or
    //     repeat) is silently dropped and the icon stays blank.
    //   - Icons the XML loaded once already have textures, and losing the EGL
    //     context invalidates them. Image keeps no copy of its path, so nothing
    //     can ask it to reload; the path has to be remembered here.
    //
    // registerIcons() seeds the map with what the XML set, setIconRes() keeps
    // it current, and reapplyIcons() re-issues the lot when uploads become
    // possible again.
    std::map<brls::Image*, std::string> m_iconRes;
    bool m_uploadsWereSafe = true;
    void registerIcons();
    void setIconRes(brls::Image* img, const std::string& res);
    void reapplyIcons();

    // Raw keyboard hook, for keys borealis has no gamepad equivalent for:
    // Space toggles playback. Subscribed in onContentAvailable, dropped in
    // willDisappear — the event outlives the activity.
    brls::InputManager* m_inputManager = nullptr;
    brls::Event<brls::KeyState>::Subscription m_kbSub;
    bool m_kbSubscribed = false;

    // Inert until onContentAvailable has wired the D-pad routes. See
    // syncHiddenFocus() for why running it before that point aborts the process.
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
    // Bound only so their resource can be re-applied after a lost GL context;
    // neither is ever swapped at runtime.
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
