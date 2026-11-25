/**
 * VitaPlex - Video Queue: app/video_queue.hpp.
 */

#include "app/video_queue.hpp"

namespace vitaplex {

VideoQueue& VideoQueue::getInstance() {
    static VideoQueue instance;
    return instance;
}

void VideoQueue::add(const VideoQueueItem& item) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_items.push_back(item);
}

void VideoQueue::addNext(const VideoQueueItem& item) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_items.insert(m_items.begin(), item);
}

bool VideoQueue::popNext(VideoQueueItem& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_items.empty()) return false;
    out = m_items.front();
    m_items.erase(m_items.begin());
    return true;
}

void VideoQueue::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_items.clear();
}

size_t VideoQueue::size() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_items.size();
}

}  // namespace vitaplex
