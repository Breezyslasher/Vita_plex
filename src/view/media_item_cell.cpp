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

    // Set title
    if (m_titleLabel) {
        std::string title = item.title;
        // Truncate long titles
        if (title.length() > 15) {
            title = title.substr(0, 13) + "...";
        }
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
    if (!m_thumbnailImage || m_item.thumb.empty()) return;

    PlexClient& client = PlexClient::getInstance();
    std::string url = client.getThumbnailUrl(m_item.thumb, 220, 330);

    ImageLoader::loadAsync(url, [this](brls::Image* image) {
        // Image loaded callback
    }, m_thumbnailImage);
}

brls::View* MediaItemCell::create() {
    return new MediaItemCell();
}

} // namespace vitaplex
