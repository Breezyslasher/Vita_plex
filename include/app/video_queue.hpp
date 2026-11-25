/**
 * VitaPlex - Video Queue
 * The videos (episodes, music videos) waiting to play after the one playing.
 *
 * Apart from MusicQueue, which the music player plays through and which
 * loads every item as audio: a video plays in the video player
 * (PlayerActivity's single-item path), which takes the next one from here as
 * each ends. The two are never both in use: music starting empties this, and
 * a video starting empties the music queue (PlayerActivity).
 * Not kept between runs.
 */

#pragma once

#include <mutex>
#include <string>
#include <vector>

namespace vitaplex {

struct VideoQueueItem {
    std::string ratingKey;
    std::string title;
};

class VideoQueue {
public:
    static VideoQueue& getInstance();

    // At the end, or to play next.
    void add(const VideoQueueItem& item);
    void addNext(const VideoQueueItem& item);
    // The next video, taken off the queue; false when there is none.
    bool popNext(VideoQueueItem& out);
    void clear();
    size_t size();

private:
    VideoQueue() = default;
    std::mutex m_mutex;
    std::vector<VideoQueueItem> m_items;
};

}  // namespace vitaplex
