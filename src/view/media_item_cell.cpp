/**
 * VitaPlex - Media Item Cell implementation
 */

#include "view/media_item_cell.hpp"
#include "app/plex_client.hpp"
#include "utils/image_loader.hpp"

namespace vitaplex {

MediaItemCell::MediaItemCell() {
    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::CENTER);
    this->setPadding(5);
    this->setFocusable(true);
    this->setCornerRadius(8);
    this->setBackgroundColor(nvgRGBA(50, 50, 50, 255));

    // Thumbnail image
    m_thumbnailImage = new brls::Image();
    m_thumbnailImage->setWidth(110);
    m_thumbnailImage->setHeight(165);
    m_thumbnailImage->setScalingType(brls::ImageScalingType::FIT);
    m_thumbnailImage->setCornerRadius(4);
    this->addView(m_thumbnailImage);

    // Title label
    m_titleLabel = new brls::Label();
    m_titleLabel->setFontSize(12);
    m_titleLabel->setMarginTop(5);
    m_titleLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    this->addView(m_titleLabel);

    // Subtitle label (for episodes: S01E01)
    m_subtitleLabel = new brls::Label();
    m_subtitleLabel->setFontSize(10);
    m_subtitleLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    m_subtitleLabel->setVisibility(brls::Visibility::GONE);
    this->addView(m_subtitleLabel);

    // Description label (shows on focus for episodes)
    m_descriptionLabel = new brls::Label();
    m_descriptionLabel->setFontSize(9);
    m_descriptionLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    m_descriptionLabel->setVisibility(brls::Visibility::GONE);
    this->addView(m_descriptionLabel);

    // Progress bar (for continue watching)
    m_progressBar = new brls::Rectangle();
    m_progressBar->setHeight(3);
    m_progressBar->setWidth(0);
    m_progressBar->setColor(nvgRGBA(229, 160, 13, 255)); // Plex orange
    m_progressBar->setVisibility(brls::Visibility::GONE);
    this->addView(m_progressBar);
}

void MediaItemCell::setItem(const MediaItem& item) {
    m_item = item;

    // Adjust thumbnail size based on media type
    // Music (albums, artists, tracks) use square covers
    // Movies, TV shows use portrait posters
    bool isMusic = (item.mediaType == MediaType::MUSIC_ARTIST ||
                    item.mediaType == MediaType::MUSIC_ALBUM ||
                    item.mediaType == MediaType::MUSIC_TRACK);

    if (isMusic) {
        // Square album art
        m_thumbnailImage->setWidth(110);
        m_thumbnailImage->setHeight(110);
    } else {
        // Portrait poster
        m_thumbnailImage->setWidth(110);
        m_thumbnailImage->setHeight(165);
    }

    // Set title
    if (m_titleLabel) {
        std::string title = item.title;
        // Truncate long titles
        if (title.length() > 15) {
            title = title.substr(0, 13) + "...";
        }
        m_originalTitle = title;  // Store truncated title for focus restore
        m_titleLabel->setText(title);
    }

    // Set subtitle for episodes
    if (m_subtitleLabel) {
        if (item.mediaType == MediaType::EPISODE) {
            char subtitle[32];
            snprintf(subtitle, sizeof(subtitle), "S%02dE%02d",
                     item.parentIndex, item.index);
            m_subtitleLabel->setText(subtitle);
            m_subtitleLabel->setVisibility(brls::Visibility::VISIBLE);
        } else if (item.mediaType == MediaType::MUSIC_TRACK) {
            // Show track number for music
            if (item.index > 0) {
                m_subtitleLabel->setText("Track " + std::to_string(item.index));
                m_subtitleLabel->setVisibility(brls::Visibility::VISIBLE);
            } else {
                m_subtitleLabel->setVisibility(brls::Visibility::GONE);
            }
        } else {
            m_subtitleLabel->setVisibility(brls::Visibility::GONE);
        }
    }

    // Show progress bar for items with view offset
    if (m_progressBar && item.viewOffset > 0 && item.duration > 0) {
        float progress = (float)item.viewOffset / (float)item.duration;
        m_progressBar->setWidth(110 * progress);
        m_progressBar->setVisibility(brls::Visibility::VISIBLE);
    }

    // Load thumbnail
    loadThumbnail();
}

void MediaItemCell::loadThumbnail() {
    if (!m_thumbnailImage) return;

    PlexClient& client = PlexClient::getInstance();

    // Use square dimensions for music, portrait for movies/TV
    bool isMusic = (m_item.mediaType == MediaType::MUSIC_ARTIST ||
                    m_item.mediaType == MediaType::MUSIC_ALBUM ||
                    m_item.mediaType == MediaType::MUSIC_TRACK);

    int width = isMusic ? 220 : 220;
    int height = isMusic ? 220 : 330;

    // For episodes, prefer grandparentThumb (show poster) if available
    std::string thumbPath = m_item.thumb;
    if (m_item.mediaType == MediaType::EPISODE && !m_item.grandparentThumb.empty()) {
        thumbPath = m_item.grandparentThumb;
    }

    if (thumbPath.empty()) return;

    std::string url = client.getThumbnailUrl(thumbPath, width, height);

    ImageLoader::loadAsync(url, [this](brls::Image* image) {
        // Image loaded callback
    }, m_thumbnailImage);
}

brls::View* MediaItemCell::create() {
    return new MediaItemCell();
}

void MediaItemCell::onFocusGained() {
    brls::Box::onFocusGained();
    updateFocusInfo(true);
}

void MediaItemCell::onFocusLost() {
    brls::Box::onFocusLost();
    updateFocusInfo(false);
}

void MediaItemCell::updateFocusInfo(bool focused) {
    if (!m_titleLabel || !m_descriptionLabel) return;

    // For episodes, show extended info on focus
    if (m_item.mediaType == MediaType::EPISODE) {
        if (focused) {
            // Show full title
            m_titleLabel->setText(m_item.title);

            // Show duration and other info
            std::string info;
            if (m_item.duration > 0) {
                int minutes = m_item.duration / 60000;
                info = std::to_string(minutes) + " min";
            }
            if (!m_item.summary.empty()) {
                // Show first 50 chars of summary
                std::string summary = m_item.summary;
                if (summary.length() > 50) {
                    summary = summary.substr(0, 47) + "...";
                }
                if (!info.empty()) info += " - ";
                info += summary;
            }
            if (!info.empty()) {
                m_descriptionLabel->setText(info);
                m_descriptionLabel->setVisibility(brls::Visibility::VISIBLE);
            }
        } else {
            // Restore truncated title
            m_titleLabel->setText(m_originalTitle);
            m_descriptionLabel->setVisibility(brls::Visibility::GONE);
        }
    } else if (m_item.mediaType == MediaType::MOVIE) {
        // Show runtime for movies on focus
        if (focused && m_item.duration > 0) {
            int minutes = m_item.duration / 60000;
            std::string info = std::to_string(minutes) + " min";
            if (m_item.year > 0) {
                info = std::to_string(m_item.year) + " - " + info;
            }
            m_descriptionLabel->setText(info);
            m_descriptionLabel->setVisibility(brls::Visibility::VISIBLE);
            // Show full title
            m_titleLabel->setText(m_item.title);
        } else {
            m_titleLabel->setText(m_originalTitle);
            m_descriptionLabel->setVisibility(brls::Visibility::GONE);
        }
    } else if (m_item.mediaType == MediaType::SHOW) {
        // Show year for shows on focus
        if (focused) {
            std::string info;
            if (m_item.year > 0) {
                info = std::to_string(m_item.year);
            }
            if (m_item.leafCount > 0) {
                if (!info.empty()) info += " - ";
                info += std::to_string(m_item.leafCount) + " seasons";
            }
            if (!info.empty()) {
                m_descriptionLabel->setText(info);
                m_descriptionLabel->setVisibility(brls::Visibility::VISIBLE);
            }
            m_titleLabel->setText(m_item.title);
        } else {
            m_titleLabel->setText(m_originalTitle);
            m_descriptionLabel->setVisibility(brls::Visibility::GONE);
        }
    }
}

} // namespace vitaplex
