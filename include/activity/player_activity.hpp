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
#include "app/plex_client.hpp"

// Forward declarations
namespace vitaplex {
    class VideoView;
    struct MediaItem;
    struct QueueItem;
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
    void onTrackEnded(const QueueItem* nextTrack);  // Called when track ends
    void updateQueueDisplay();      // Update UI with queue info

    // Queue list overlay
    void showQueueOverlay();
    void hideQueueOverlay();
    void populateQueueList();       // Build queue list with cover art and titles
    void playFromQueue(int index);  // Play a specific track from queue list
    bool m_queueOverlayVisible = false;

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
    bool m_lyricsEnabled = false;  // Lyrics subtitle toggle state for music mode
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
    BRLS_BIND(brls::Label, queueLabel, "player/queue_info");
    BRLS_BIND(brls::Slider, progressSlider, "player/progress");
    BRLS_BIND(brls::Box, controlsBox, "player/controls");
    BRLS_BIND(brls::Box, centerControls, "player/center_controls");
    BRLS_BIND(brls::Image, photoImage, "player/photo");
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
    BRLS_BIND(brls::Box, lyricsBtn, "player/lyrics_btn");
    BRLS_BIND(brls::Image, lyricsIcon, "player/lyrics_icon");
};

} // namespace vitaplex
