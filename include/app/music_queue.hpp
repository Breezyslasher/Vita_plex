/**
 * VitaPlex - Music Queue Manager
 * Handles playlist/queue management for music playback
 */

#pragma once

#include "app/plex_client.hpp"
#include <string>
#include <vector>
#include <functional>
#include <random>

namespace vitaplex {

// Playback modes
enum class RepeatMode {
    OFF,        // No repeat
    ONE,        // Repeat current track
    ALL         // Repeat entire queue
};

// Queue item with essential track info
struct QueueItem {
    std::string ratingKey;
    std::string title;
    std::string artist;       // grandparentTitle for tracks
    std::string album;        // parentTitle for tracks
    std::string thumb;
    int duration = 0;         // Duration in seconds
    int index = 0;            // Position in original queue (for unshuffle)
    int playQueueItemID = 0;  // Server-side play queue item ID (0 = offline/unsynced)
};

/**
 * Music Queue Manager singleton
 * Manages the playback queue, shuffle, repeat modes
 */
class MusicQueue {
public:
    static MusicQueue& getInstance();

    // Queue management
    void clear();
    void addTrack(const MediaItem& item);           // Add to end of queue
    void addTracks(const std::vector<MediaItem>& items);
    void insertTrackAfterCurrent(const MediaItem& item);  // Insert after current track (play next)
    void removeTrack(int index);
    void moveTrack(int fromIndex, int toIndex);

    // Set queue from album/playlist (clears existing queue)
    void setQueue(const std::vector<MediaItem>& items, int startIndex = 0);

    // Playback control
    bool playTrack(int index);          // Play specific track in queue
    bool playNext();                    // Play next track (respects repeat/shuffle)
    bool playPrevious();                // Play previous track
    bool hasNext() const;               // Check if there's a next track
    bool hasPrevious() const;           // Check if there's a previous track

    // Current state
    int getCurrentIndex() const { return m_currentIndex; }
    const QueueItem* getCurrentTrack() const;
    const std::vector<QueueItem>& getQueue() const { return m_queue; }
    int getQueueSize() const { return (int)m_queue.size(); }
    bool isEmpty() const { return m_queue.empty(); }

    // Shuffle mode
    void setShuffle(bool enabled);
    bool isShuffleEnabled() const { return m_shuffleEnabled; }
    void reshuffle();  // Re-randomize shuffle order
    const std::vector<int>& getShuffleOrder() const { return m_shuffleOrder; }
    int getShufflePosition() const { return m_shufflePosition; }

    // Repeat mode
    void setRepeatMode(RepeatMode mode);
    RepeatMode getRepeatMode() const { return m_repeatMode; }
    void cycleRepeatMode();  // Cycle through OFF -> ALL -> ONE -> OFF

    // Track ended callback (for auto-advance)
    using TrackEndedCallback = std::function<void(const QueueItem* nextTrack)>;
    void setTrackEndedCallback(TrackEndedCallback callback) { m_trackEndedCallback = callback; }

    // Notify that current track ended (called by player)
    void onTrackEnded();

    // Queue changed callback (for UI updates)
    using QueueChangedCallback = std::function<void()>;
    void setQueueChangedCallback(QueueChangedCallback callback) { m_queueChangedCallback = callback; }

    // Save/load queue state (for persistence across sessions)
    void saveState();
    void loadState();

    // Version counter - incremented on every queue mutation (add/remove/reorder/set/clear)
    uint32_t getVersion() const { return m_version; }

    // Server-side play queue sync
    // Creates a server play queue from the current queue state.
    // When synced, shuffle/move/add/remove operations are mirrored to server.
    void syncToServer(int playQueueID);
    void clearServerSync();
    bool isServerSynced() const { return m_playQueueID > 0; }
    int getPlayQueueID() const { return m_playQueueID; }
    // Get the playQueueItemID for the current track (for timeline reports)
    int getCurrentPlayQueueItemID() const;

    // Set queue from server play queue response (replaces local state)
    void setFromPlayQueue(const PlexClient::PlayQueueContainer& pq, bool isShuffled);

private:
    MusicQueue();
    ~MusicQueue() = default;

    void notifyQueueChanged();
    QueueItem mediaItemToQueueItem(const MediaItem& item, int index);
    void generateShuffleOrder();
    int getShuffledIndex(int logicalIndex) const;
    int getLogicalIndex(int shuffledIndex) const;

    std::vector<QueueItem> m_queue;           // Actual queue items
    std::vector<int> m_shuffleOrder;          // Shuffle indices mapping
    int m_currentIndex = -1;                  // Current playing index (-1 = nothing)
    int m_shufflePosition = -1;               // Position in shuffle order

    bool m_shuffleEnabled = false;
    RepeatMode m_repeatMode = RepeatMode::OFF;

    TrackEndedCallback m_trackEndedCallback;
    QueueChangedCallback m_queueChangedCallback;

    uint32_t m_version = 0;  // Incremented on every queue mutation
    std::mt19937 m_rng;  // Random number generator for shuffle

    // Server-side play queue state
    int m_playQueueID = 0;  // 0 = offline/not synced
};

} // namespace vitaplex
