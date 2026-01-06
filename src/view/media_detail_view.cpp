/**
 * VitaPlex - Media Detail View implementation
 */

#include "view/media_detail_view.hpp"
#include "view/media_item_cell.hpp"
#include "app/application.hpp"
#include "utils/image_loader.hpp"

namespace vitaplex {

MediaDetailView::MediaDetailView(const MediaItem& item)
    : m_item(item) {

    this->setAxis(brls::Axis::ROW);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::FLEX_START);
    this->setPadding(30);
    this->setGrow(1.0f);

    // Register back button (B/Circle) to pop this activity
    this->registerAction("Back", brls::ControllerButton::BUTTON_B, [](brls::View* view) {
        brls::Application::popActivity();
        return true;
    }, false, false, brls::Sound::SOUND_BACK);

    // Left side - poster
    auto* leftBox = new brls::Box();
    leftBox->setAxis(brls::Axis::COLUMN);
    leftBox->setWidth(200);
    leftBox->setMarginRight(30);

    m_posterImage = new brls::Image();
    m_posterImage->setWidth(200);
    m_posterImage->setHeight(300);
    m_posterImage->setScalingType(brls::ImageScalingType::FIT);
    leftBox->addView(m_posterImage);

    // Play buttons
    m_playButton = new brls::Button();
    m_playButton->setText("Play");
    m_playButton->setWidth(200);
    m_playButton->setMarginTop(20);
    m_playButton->registerClickAction([this](brls::View* view) {
        onPlay(false);
        return true;
    });
    leftBox->addView(m_playButton);

    if (m_item.viewOffset > 0) {
        m_resumeButton = new brls::Button();
        m_resumeButton->setText("Resume");
        m_resumeButton->setWidth(200);
        m_resumeButton->setMarginTop(10);
        m_resumeButton->registerClickAction([this](brls::View* view) {
            onPlay(true);
            return true;
        });
        leftBox->addView(m_resumeButton);
    }

    this->addView(leftBox);

    // Right side - details
    auto* rightBox = new brls::Box();
    rightBox->setAxis(brls::Axis::COLUMN);
    rightBox->setGrow(1.0f);

    // Title
    m_titleLabel = new brls::Label();
    m_titleLabel->setText(m_item.title);
    m_titleLabel->setFontSize(26);
    m_titleLabel->setMarginBottom(10);
    rightBox->addView(m_titleLabel);

    // Metadata row
    auto* metaBox = new brls::Box();
    metaBox->setAxis(brls::Axis::ROW);
    metaBox->setMarginBottom(15);

    if (m_item.year > 0) {
        m_yearLabel = new brls::Label();
        m_yearLabel->setText(std::to_string(m_item.year));
        m_yearLabel->setFontSize(16);
        m_yearLabel->setMarginRight(15);
        metaBox->addView(m_yearLabel);
    }

    if (!m_item.contentRating.empty()) {
        m_ratingLabel = new brls::Label();
        m_ratingLabel->setText(m_item.contentRating);
        m_ratingLabel->setFontSize(16);
        m_ratingLabel->setMarginRight(15);
        metaBox->addView(m_ratingLabel);
    }

    if (m_item.duration > 0) {
        m_durationLabel = new brls::Label();
        int minutes = m_item.duration / 60000;
        m_durationLabel->setText(std::to_string(minutes) + " min");
        m_durationLabel->setFontSize(16);
        metaBox->addView(m_durationLabel);
    }

    rightBox->addView(metaBox);

    // Summary
    if (!m_item.summary.empty()) {
        m_summaryLabel = new brls::Label();
        m_summaryLabel->setText(m_item.summary);
        m_summaryLabel->setFontSize(16);
        m_summaryLabel->setMarginBottom(20);
        rightBox->addView(m_summaryLabel);
    }

    // Children container (for shows/seasons)
    if (m_item.mediaType == MediaType::SHOW ||
        m_item.mediaType == MediaType::SEASON ||
        m_item.mediaType == MediaType::MUSIC_ALBUM) {

        auto* childrenLabel = new brls::Label();
        if (m_item.mediaType == MediaType::SHOW) {
            childrenLabel->setText("Seasons");
        } else if (m_item.mediaType == MediaType::SEASON) {
            childrenLabel->setText("Episodes");
        } else {
            childrenLabel->setText("Tracks");
        }
        childrenLabel->setFontSize(20);
        childrenLabel->setMarginBottom(10);
        rightBox->addView(childrenLabel);

        m_childrenBox = new brls::Box();
        m_childrenBox->setAxis(brls::Axis::ROW);
        m_childrenBox->setHeight(180);
        rightBox->addView(m_childrenBox);
    }

    this->addView(rightBox);

    // Load full details
    loadDetails();
}

brls::View* MediaDetailView::create() {
    return nullptr; // Factory not used
}

void MediaDetailView::loadDetails() {
    PlexClient& client = PlexClient::getInstance();

    // Load full details
    MediaItem fullItem;
    if (client.fetchMediaDetails(m_item.ratingKey, fullItem)) {
        m_item = fullItem;

        // Update UI with full details
        if (m_titleLabel && !m_item.title.empty()) {
            m_titleLabel->setText(m_item.title);
        }

        if (m_summaryLabel && !m_item.summary.empty()) {
            m_summaryLabel->setText(m_item.summary);
        }
    }

    // Load thumbnail
    if (m_posterImage && !m_item.thumb.empty()) {
        std::string url = client.getThumbnailUrl(m_item.thumb, 400, 600);
        ImageLoader::loadAsync(url, [this](brls::Image* image) {
            // Image loaded
        }, m_posterImage);
    }

    // Load children if applicable
    loadChildren();
}

void MediaDetailView::loadChildren() {
    if (!m_childrenBox) return;

    PlexClient& client = PlexClient::getInstance();

    if (client.fetchChildren(m_item.ratingKey, m_children)) {
        m_childrenBox->clearViews();

        for (const auto& child : m_children) {
            auto* cell = new MediaItemCell();
            cell->setItem(child);
            cell->setWidth(120);
            cell->setHeight(170);
            cell->setMarginRight(10);

            cell->registerClickAction([this, child](brls::View* view) {
                // Navigate to child detail
                auto* detailView = new MediaDetailView(child);
                brls::Application::pushActivity(new brls::Activity(detailView));
                return true;
            });

            m_childrenBox->addView(cell);
        }
    }
}

void MediaDetailView::onPlay(bool resume) {
    // Start playback
    if (m_item.mediaType == MediaType::MOVIE ||
        m_item.mediaType == MediaType::EPISODE) {

        Application::getInstance().pushPlayerActivity(m_item.ratingKey);
    }
}

} // namespace vitaplex
