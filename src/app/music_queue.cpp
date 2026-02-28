/**
 * VitaPlex - Music Queue Manager Implementation
 */

#include "app/music_queue.hpp"
#include <borealis.hpp>
#include <algorithm>
#include <chrono>
#include <fstream>

#ifdef __vita__
#include <psp2/io/fcntl.h>
#endif

namespace vitaplex {

static const std::string QUEUE_STATE_FILE = "ux0:data/vitaplex/queue_state.txt";

MusicQueue::MusicQueue() {
    // Seed random number generator
    auto seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    m_rng.seed(static_cast<unsigned int>(seed));
}

MusicQueue& MusicQueue::getInstance() {
    static MusicQueue instance;
    return instance;
}

void MusicQueue::clear() {
    m_queue.clear();
    m_shuffleOrder.clear();
    m_currentIndex = -1;
    m_shufflePosition = -1;
    notifyQueueChanged();
}

QueueItem MusicQueue::mediaItemToQueueItem(const MediaItem& item, int index) {
    QueueItem qi;
    qi.ratingKey = item.ratingKey;
    qi.title = item.title;
    qi.artist = item.grandparentTitle;  // Artist for tracks
    qi.album = item.parentTitle;        // Album for tracks
    qi.thumb = item.thumb;
    qi.duration = item.duration / 1000; // Convert ms to seconds
    qi.index = index;
    return qi;
}

void MusicQueue::addTrack(const MediaItem& item) {
    int index = (int)m_queue.size();
    m_queue.push_back(mediaItemToQueueItem(item, index));

    // Update shuffle order if shuffling
    if (m_shuffleEnabled) {
        // Insert new track at random position in remaining shuffle order
        int insertPos = m_shufflePosition + 1 + (m_rng() % (m_shuffleOrder.size() - m_shufflePosition));
        m_shuffleOrder.insert(m_shuffleOrder.begin() + insertPos, index);
    }

    notifyQueueChanged();
}

void MusicQueue::addTracks(const std::vector<MediaItem>& items) {
    int startIndex = (int)m_queue.size();
    for (size_t i = 0; i < items.size(); i++) {
        m_queue.push_back(mediaItemToQueueItem(items[i], startIndex + (int)i));
    }

    // Regenerate shuffle order if shuffling
    if (m_shuffleEnabled && !m_queue.empty()) {
        generateShuffleOrder();
    }

    notifyQueueChanged();
}

void MusicQueue::removeTrack(int index) {
    if (index < 0 || index >= (int)m_queue.size()) return;

    m_queue.erase(m_queue.begin() + index);

    // Update indices for remaining items
    for (int i = index; i < (int)m_queue.size(); i++) {
        m_queue[i].index = i;
    }

    // Adjust current index if needed
    if (m_currentIndex >= (int)m_queue.size()) {
        m_currentIndex = m_queue.empty() ? -1 : (int)m_queue.size() - 1;
    } else if (m_currentIndex > index) {
        m_currentIndex--;
    }

    // Regenerate shuffle order
    if (m_shuffleEnabled) {
        generateShuffleOrder();
    }

    notifyQueueChanged();
}

void MusicQueue::moveTrack(int fromIndex, int toIndex) {
    if (fromIndex < 0 || fromIndex >= (int)m_queue.size()) return;
    if (toIndex < 0 || toIndex >= (int)m_queue.size()) return;
    if (fromIndex == toIndex) return;

    QueueItem item = m_queue[fromIndex];
    m_queue.erase(m_queue.begin() + fromIndex);
    m_queue.insert(m_queue.begin() + toIndex, item);

    // Update indices
    for (int i = 0; i < (int)m_queue.size(); i++) {
        m_queue[i].index = i;
    }

    // Adjust current index
    if (m_currentIndex == fromIndex) {
        m_currentIndex = toIndex;
    } else if (fromIndex < m_currentIndex && toIndex >= m_currentIndex) {
        m_currentIndex--;
    } else if (fromIndex > m_currentIndex && toIndex <= m_currentIndex) {
        m_currentIndex++;
    }

    notifyQueueChanged();
}

void MusicQueue::setQueue(const std::vector<MediaItem>& items, int startIndex) {
    clear();

    for (size_t i = 0; i < items.size(); i++) {
        m_queue.push_back(mediaItemToQueueItem(items[i], (int)i));
    }

    if (m_shuffleEnabled) {
        generateShuffleOrder();
        // Find the start index in shuffle order
        m_shufflePosition = 0;
        for (size_t i = 0; i < m_shuffleOrder.size(); i++) {
            if (m_shuffleOrder[i] == startIndex) {
                // Move this to the front of remaining shuffle
                std::swap(m_shuffleOrder[0], m_shuffleOrder[i]);
                break;
            }
        }
        m_currentIndex = m_shuffleOrder[0];
    } else {
        m_currentIndex = (startIndex >= 0 && startIndex < (int)m_queue.size())
                        ? startIndex : 0;
    }

    notifyQueueChanged();
    brls::Logger::info("MusicQueue: Set queue with {} tracks, starting at {}",
                       m_queue.size(), m_currentIndex);
}

bool MusicQueue::playTrack(int index) {
    if (index < 0 || index >= (int)m_queue.size()) {
        return false;
    }

    m_currentIndex = index;

    // Update shuffle position if shuffling
    if (m_shuffleEnabled) {
        for (size_t i = 0; i < m_shuffleOrder.size(); i++) {
            if (m_shuffleOrder[i] == index) {
                m_shufflePosition = (int)i;
                break;
            }
        }
    }

    brls::Logger::info("MusicQueue: Playing track {} - {}", index, m_queue[index].title);
    return true;
}

bool MusicQueue::playNext() {
    if (m_queue.empty()) return false;

    int nextIndex = -1;

    if (m_repeatMode == RepeatMode::ONE) {
        // Repeat same track
        nextIndex = m_currentIndex;
    } else if (m_shuffleEnabled) {
        // Use shuffle order
        m_shufflePosition++;
        if (m_shufflePosition >= (int)m_shuffleOrder.size()) {
            if (m_repeatMode == RepeatMode::ALL) {
                // Reshuffle and start over
                reshuffle();
                m_shufflePosition = 0;
            } else {
                // End of queue
                m_shufflePosition = (int)m_shuffleOrder.size() - 1;
                return false;
            }
        }
        nextIndex = m_shuffleOrder[m_shufflePosition];
    } else {
        // Normal sequential order
        nextIndex = m_currentIndex + 1;
        if (nextIndex >= (int)m_queue.size()) {
            if (m_repeatMode == RepeatMode::ALL) {
                nextIndex = 0;
            } else {
                return false;
            }
        }
    }

    m_currentIndex = nextIndex;
    brls::Logger::info("MusicQueue: Next track {} - {}", m_currentIndex, m_queue[m_currentIndex].title);
    return true;
}

bool MusicQueue::playPrevious() {
    if (m_queue.empty()) return false;

    int prevIndex = -1;

    if (m_shuffleEnabled) {
        m_shufflePosition--;
        if (m_shufflePosition < 0) {
            if (m_repeatMode == RepeatMode::ALL) {
                m_shufflePosition = (int)m_shuffleOrder.size() - 1;
            } else {
                m_shufflePosition = 0;
                return false;
            }
        }
        prevIndex = m_shuffleOrder[m_shufflePosition];
    } else {
        prevIndex = m_currentIndex - 1;
        if (prevIndex < 0) {
            if (m_repeatMode == RepeatMode::ALL) {
                prevIndex = (int)m_queue.size() - 1;
            } else {
                return false;
            }
        }
    }

    m_currentIndex = prevIndex;
    brls::Logger::info("MusicQueue: Previous track {} - {}", m_currentIndex, m_queue[m_currentIndex].title);
    return true;
}

bool MusicQueue::hasNext() const {
    if (m_queue.empty()) return false;
    if (m_repeatMode == RepeatMode::ONE || m_repeatMode == RepeatMode::ALL) return true;

    if (m_shuffleEnabled) {
        return m_shufflePosition < (int)m_shuffleOrder.size() - 1;
    }
    return m_currentIndex < (int)m_queue.size() - 1;
}

bool MusicQueue::hasPrevious() const {
    if (m_queue.empty()) return false;
    if (m_repeatMode == RepeatMode::ALL) return true;

    if (m_shuffleEnabled) {
        return m_shufflePosition > 0;
    }
    return m_currentIndex > 0;
}

const QueueItem* MusicQueue::getCurrentTrack() const {
    if (m_currentIndex < 0 || m_currentIndex >= (int)m_queue.size()) {
        return nullptr;
    }
    return &m_queue[m_currentIndex];
}

void MusicQueue::setShuffle(bool enabled) {
    if (m_shuffleEnabled == enabled) return;

    m_shuffleEnabled = enabled;

    if (enabled) {
        generateShuffleOrder();
        // Find current track in shuffle order
        for (size_t i = 0; i < m_shuffleOrder.size(); i++) {
            if (m_shuffleOrder[i] == m_currentIndex) {
                // Move current track to front
                std::swap(m_shuffleOrder[0], m_shuffleOrder[i]);
                m_shufflePosition = 0;
                break;
            }
        }
    } else {
        m_shuffleOrder.clear();
        m_shufflePosition = -1;
    }

    brls::Logger::info("MusicQueue: Shuffle {}", enabled ? "enabled" : "disabled");
    notifyQueueChanged();
}

void MusicQueue::reshuffle() {
    if (!m_shuffleEnabled || m_queue.empty()) return;

    generateShuffleOrder();
    m_shufflePosition = -1;

    brls::Logger::debug("MusicQueue: Reshuffled queue");
}

void MusicQueue::generateShuffleOrder() {
    m_shuffleOrder.clear();
    m_shuffleOrder.reserve(m_queue.size());

    for (int i = 0; i < (int)m_queue.size(); i++) {
        m_shuffleOrder.push_back(i);
    }

    // Fisher-Yates shuffle
    for (int i = (int)m_shuffleOrder.size() - 1; i > 0; i--) {
        int j = m_rng() % (i + 1);
        std::swap(m_shuffleOrder[i], m_shuffleOrder[j]);
    }
}

void MusicQueue::setRepeatMode(RepeatMode mode) {
    m_repeatMode = mode;

    const char* modeStr = "OFF";
    if (mode == RepeatMode::ONE) modeStr = "ONE";
    else if (mode == RepeatMode::ALL) modeStr = "ALL";

    brls::Logger::info("MusicQueue: Repeat mode set to {}", modeStr);
    notifyQueueChanged();
}

void MusicQueue::cycleRepeatMode() {
    switch (m_repeatMode) {
        case RepeatMode::OFF:
            setRepeatMode(RepeatMode::ALL);
            break;
        case RepeatMode::ALL:
            setRepeatMode(RepeatMode::ONE);
            break;
        case RepeatMode::ONE:
            setRepeatMode(RepeatMode::OFF);
            break;
    }
}

void MusicQueue::onTrackEnded() {
    brls::Logger::debug("MusicQueue: Track ended, checking for next");

    const QueueItem* nextTrack = nullptr;

    if (playNext()) {
        nextTrack = getCurrentTrack();
    }

    if (m_trackEndedCallback) {
        m_trackEndedCallback(nextTrack);
    }
}

void MusicQueue::notifyQueueChanged() {
    if (m_queueChangedCallback) {
        m_queueChangedCallback();
    }
}

void MusicQueue::saveState() {
    std::ofstream file(QUEUE_STATE_FILE);
    if (!file.is_open()) {
        brls::Logger::warning("MusicQueue: Could not save queue state");
        return;
    }

    // Save settings
    file << "shuffle=" << (m_shuffleEnabled ? 1 : 0) << "\n";
    file << "repeat=" << (int)m_repeatMode << "\n";
    file << "current=" << m_currentIndex << "\n";
    file << "count=" << m_queue.size() << "\n";

    // Save queue items (just rating keys for now)
    for (const auto& item : m_queue) {
        file << "track=" << item.ratingKey << "\n";
    }

    file.close();
    brls::Logger::debug("MusicQueue: State saved");
}

void MusicQueue::loadState() {
    std::ifstream file(QUEUE_STATE_FILE);
    if (!file.is_open()) {
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        if (key == "shuffle") {
            m_shuffleEnabled = (value == "1");
        } else if (key == "repeat") {
            m_repeatMode = static_cast<RepeatMode>(std::stoi(value));
        } else if (key == "current") {
            m_currentIndex = std::stoi(value);
        }
        // Note: We don't restore the actual tracks here - that would require
        // fetching metadata from Plex. The queue is typically rebuilt when
        // playing an album or playlist.
    }

    file.close();
    brls::Logger::debug("MusicQueue: State loaded (shuffle={}, repeat={})",
                       m_shuffleEnabled, (int)m_repeatMode);
}

} // namespace vitaplex
