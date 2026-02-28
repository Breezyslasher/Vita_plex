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

    // Play from queue (album, playlist, etc.)
    static PlayerActivity* createWithQueue(const std::vector<MediaItem>& tracks, int startIndex = 0);

    brls::View* createContentView() override;

    void onContentAvailable() override;

    void willDisappear(bool resetState) override;

private:
    void loadMedia();
    void loadFromQueue();           // Load current track from queue
    void updateProgress();
    void togglePlayPause();
    void seek(int seconds);

    // Queue controls
    void playNext();
    void playPrevious();
    void toggleShuffle();
    void toggleRepeat();
    void onTrackEnded(const QueueItem* nextTrack);  // Called when track ends
    void updateQueueDisplay();      // Update UI with queue info

    std::string m_mediaKey;
    std::string m_directFilePath;  // For direct file playback (debug)
    bool m_isPlaying = false;
    bool m_isPhoto = false;
    bool m_isLocalFile = false;    // Playing from local download
    bool m_isDirectFile = false;   // Playing direct file path (debug)
    bool m_isQueueMode = false;    // Playing from queue
    bool m_destroying = false;     // Flag to prevent timer callbacks during destruction
    bool m_loadingMedia = false;   // Flag to prevent rapid re-entry of loadMedia
    double m_pendingSeek = 0.0;    // Pending seek position (set when resuming)
    brls::RepeatingTimer m_updateTimer;

    // Alive flag for async image loads - prevents use-after-free when activity is destroyed
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);

    BRLS_BIND(brls::Box, playerContainer, "player/container");
    BRLS_BIND(brls::Label, titleLabel, "player/title");
    BRLS_BIND(brls::Label, artistLabel, "player/artist");
    BRLS_BIND(brls::Label, timeLabel, "player/time");
    BRLS_BIND(brls::Label, queueLabel, "player/queue_info");
    BRLS_BIND(brls::Slider, progressSlider, "player/progress");
    BRLS_BIND(brls::Box, controlsBox, "player/controls");
    BRLS_BIND(brls::Image, photoImage, "player/photo");
    BRLS_BIND(brls::Image, albumArt, "player/album_art");
    BRLS_BIND(VideoView, videoView, "player/video");
};

} // namespace vitaplex
