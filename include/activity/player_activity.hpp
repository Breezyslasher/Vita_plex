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

    // Play a direct stream URL (for Live TV HLS streams)
    static PlayerActivity* createForStream(const std::string& streamUrl, const std::string& title);

    // Play from queue (album, playlist, etc.)
    static PlayerActivity* createWithQueue(const std::vector<MediaItem>& tracks, int startIndex = 0);

    // Resume existing queue (return to player without resetting queue)
    static PlayerActivity* createResumeQueue();

    brls::View* createContentView() override;

    void onContentAvailable() override;

    void willDisappear(bool resetState) override;

private:
    void loadMedia();
    void loadFromQueue();           // Load current track from queue
    void updateProgress();
    void togglePlayPause();
    void seek(int seconds);

    // Controls visibility toggle (like Suwayomi reader settings)
    void toggleControls();
    void showControls();
    void hideControls();
    void resetControlsIdleTimer();  // Reset inactivity timer on user input
    bool m_controlsVisible = true;
    int m_controlsIdleSeconds = 0;  // Seconds since last user interaction

    // Queue controls
    void playNext();
    void playPrevious();
    void toggleShuffle();
    void toggleRepeat();
    void updateShuffleIcon();       // Update shuffle button icon based on state
    void updateRepeatIcon();        // Update repeat button icon based on state
    void onTrackEnded(const QueueItem* nextTrack);  // Called when track ends
    void updateQueueDisplay();      // Update UI with queue info

    // Queue list overlay
    void showQueueOverlay();
    void hideQueueOverlay();
    void populateQueueList();       // Build queue list with cover art and titles
    void playFromQueue(int index);  // Play a specific track from queue list
    bool m_queueOverlayVisible = false;
    bool m_queuePopulating = false;     // Guard against re-entrant populateQueueList
    uint32_t m_cachedQueueVersion = 0; // Queue version when rows were last built (0 = never)

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
    void swapQueueRows(int displayIdxA, int displayIdxB);
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
    MediaType m_mediaType = MediaType::UNKNOWN;  // Type of media being played
    std::string m_parentRatingKey;  // Season/album ratingKey for auto-play-next
    int m_episodeIndex = 0;         // Episode index within season for auto-play-next
    bool m_isPlaying = false;
    bool m_isPhoto = false;
    bool m_isLocalFile = false;    // Playing from local download
    bool m_isDirectFile = false;   // Playing direct file path (debug)
    bool m_isQueueMode = false;    // Playing from queue
    bool m_isResuming = false;     // Resuming existing playback (don't restart track)
    // lyrics support removed
    bool m_destroying = false;     // Flag to prevent timer callbacks during destruction
    bool m_loadingMedia = false;   // Flag to prevent rapid re-entry of loadMedia
    double m_pendingSeek = 0.0;    // Pending seek position (set when resuming)
    int m_transcodeBaseOffsetMs = 0;  // Base offset (ms) used to start current transcode
    bool m_updatingSlider = false;  // Guard to prevent slider update from triggering seek
    brls::RepeatingTimer m_updateTimer;
    int m_timelineCounter = 0;           // Seconds since last timeline report
    std::string m_lastTimelineState;     // Last reported state to detect changes

    // Deferred MPV init: URL and title are stored here during onContentAvailable()
    // and loaded in the first updateProgress() call. This prevents GXM context
    // conflicts between MPV's render context creation / decoder threads and
    // NanoVG during the borealis activity show phase.
    std::string m_pendingPlayUrl;
    std::string m_pendingPlayTitle;
    bool m_pendingIsAudio = false;

    // Alive flag for async image loads - prevents use-after-free when activity is destroyed
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);

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
    BRLS_BIND(brls::Box, trackOverlay, "player/track_overlay");
    BRLS_BIND(brls::Label, trackOverlayTitle, "player/track_overlay_title");
    BRLS_BIND(brls::Box, trackList, "player/track_list");
    BRLS_BIND(brls::Box, skipBtn, "player/skip_btn");
    BRLS_BIND(brls::Label, skipLabel, "player/skip_label");
    BRLS_BIND(brls::Box, queueBtn, "player/queue_btn");
    BRLS_BIND(brls::Image, queueIcon, "player/queue_icon");
    BRLS_BIND(brls::Box, queueOverlay, "player/queue_overlay");
    BRLS_BIND(brls::Label, queueOverlayTitle, "player/queue_overlay_title");
    BRLS_BIND(brls::Box, queueList, "player/queue_list");
    BRLS_BIND(brls::ScrollingFrame, queueScroll, "player/queue_scroll");

    // Music-specific UI elements
    BRLS_BIND(brls::Box, musicInfo, "player/music_info");
    BRLS_BIND(brls::Label, musicTitleLabel, "player/music_title");
    BRLS_BIND(brls::Label, musicArtistLabel, "player/music_artist");
    BRLS_BIND(brls::Box, musicTransport, "player/music_transport");
    BRLS_BIND(brls::Box, musicPlayBtn, "player/music_play_btn");
    BRLS_BIND(brls::Image, musicPlayIcon, "player/music_play_icon");
    BRLS_BIND(brls::Box, musicPrevBtn, "player/music_prev_btn");
    BRLS_BIND(brls::Box, musicNextBtn, "player/music_next_btn");
    // lyrics button removed
    BRLS_BIND(brls::Box, shuffleBtn, "player/shuffle_btn");
    BRLS_BIND(brls::Image, shuffleIcon, "player/shuffle_icon");
    BRLS_BIND(brls::Box, repeatBtn, "player/repeat_btn");
    BRLS_BIND(brls::Image, repeatIcon, "player/repeat_icon");
};

} // namespace vitaplex
