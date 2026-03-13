/**
 * VitaPlex - Media Item Cell implementation
 */

#include "view/media_item_cell.hpp"
#include "app/plex_client.hpp"
#include "app/application.hpp"
#include "utils/image_loader.hpp"

namespace vitaplex {

MediaItemCell::MediaItemCell()
    : m_alive(std::make_shared<std::atomic<bool>>(true)) {
    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::CENTER);
    this->setPadding(5);
    this->setFocusable(true);
    this->setCornerRadius(8);
    this->setBackgroundColor(nvgRGBA(50, 50, 50, 255));

    // Thumbnail image - hidden until texture loads to prevent null texture crash on Vita
    m_thumbnailImage = new brls::Image();
    m_thumbnailImage->setWidth(110);
    m_thumbnailImage->setHeight(165);
    m_thumbnailImage->setScalingType(brls::ImageScalingType::FIT);
    m_thumbnailImage->setCornerRadius(4);
    m_thumbnailImage->setVisibility(brls::Visibility::INVISIBLE);
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

    // Button hint box (shown on focus for album items)
    // Small pill badge in top-right corner of album art
    m_buttonHintBox = new brls::Box();
    m_buttonHintBox->setAxis(brls::Axis::ROW);
    m_buttonHintBox->setJustifyContent(brls::JustifyContent::CENTER);
    m_buttonHintBox->setAlignItems(brls::AlignItems::CENTER);
    m_buttonHintBox->setPositionType(brls::PositionType::ABSOLUTE);
    m_buttonHintBox->setPositionTop(7);    // Small offset from top edge
    m_buttonHintBox->setPositionRight(7);  // Anchor to top-right corner
    m_buttonHintBox->setWidth(40);
    m_buttonHintBox->setHeight(16);
    m_buttonHintBox->setVisibility(brls::Visibility::GONE);

    m_buttonHintIcon = new brls::Image();
    m_buttonHintIcon->setWidth(40);
    m_buttonHintIcon->setHeight(16);
    m_buttonHintIcon->setScalingType(brls::ImageScalingType::FIT);
    m_buttonHintBox->addView(m_buttonHintIcon);

    m_buttonHintLabel = new brls::Label();
    m_buttonHintLabel->setFontSize(8);
    m_buttonHintLabel->setTextColor(nvgRGBA(255, 255, 255, 220));
    m_buttonHintBox->addView(m_buttonHintLabel);

    this->addView(m_buttonHintBox);
}

MediaItemCell::~MediaItemCell() {
    // Signal to any in-flight async image loads that this cell is destroyed
    if (m_alive) {
        m_alive->store(false);
    }
}

void MediaItemCell::setItem(const MediaItem& item) {
    m_item = item;

    // Adjust thumbnail size based on media type
    // Music (albums, artists, tracks) use square covers
    // Episodes use landscape stills (episode thumbnail)
    // Movies, TV shows use portrait posters
    bool isMusic = (item.mediaType == MediaType::MUSIC_ARTIST ||
                    item.mediaType == MediaType::MUSIC_ALBUM ||
                    item.mediaType == MediaType::MUSIC_TRACK ||
                    item.type == "playlist");
    bool isEpisode = (item.mediaType == MediaType::EPISODE);
    bool isClip = (item.mediaType == MediaType::CLIP);

    if (isMusic) {
        // Square album art
        m_thumbnailImage->setWidth(110);
        m_thumbnailImage->setHeight(110);
        // Adjust box to fit square art + text
        this->setWidth(120);
        this->setHeight(150);
    } else if (isEpisode || isClip) {
        // Landscape episode still / extras clip
        m_thumbnailImage->setWidth(140);
        m_thumbnailImage->setHeight(80);
        // Adjust box to fit landscape image + text
        this->setWidth(150);
        this->setHeight(125);
    } else {
        // Portrait poster
        m_thumbnailImage->setWidth(110);
        m_thumbnailImage->setHeight(165);
        // Adjust box to fit portrait poster + text
        this->setWidth(120);
        this->setHeight(200);
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

        // Hide titles for movies and shows if setting is enabled
        bool hideTitle = Application::getInstance().getSettings().hideTitlesInGrid &&
            (item.mediaType == MediaType::MOVIE || item.mediaType == MediaType::SHOW);
        m_titleLabel->setVisibility(hideTitle ? brls::Visibility::GONE : brls::Visibility::VISIBLE);

        // Shrink box height when title is hidden to remove blank space
        if (hideTitle && !isMusic && !isEpisode) {
            this->setHeight(178);
        }
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

    // Show progress bar only for items with meaningful watch progress
    // Require at least 1% watched and at least 30 seconds of viewOffset
    // to avoid showing bars for items that were barely started or have stale data
    if (m_progressBar) {
        if (item.viewOffset > 30000 && item.duration > 0) {
            float progress = (float)item.viewOffset / (float)item.duration;
            // Only show if between 1% and 95% (fully watched items shouldn't show bar)
            if (progress > 0.01f && progress < 0.95f) {
                float barWidth = isEpisode ? 140.0f : 110.0f;
                m_progressBar->setWidth(std::min(barWidth * progress, barWidth));
                m_progressBar->setVisibility(brls::Visibility::VISIBLE);
            } else {
                m_progressBar->setVisibility(brls::Visibility::GONE);
            }
        } else {
            m_progressBar->setVisibility(brls::Visibility::GONE);
        }
    }

    // Load thumbnail
    loadThumbnail();
}

void MediaItemCell::loadThumbnail() {
    if (!m_thumbnailImage) return;

    PlexClient& client = PlexClient::getInstance();

    // Use square dimensions for music/playlists, landscape for episodes, portrait for movies/TV
    bool isMusic = (m_item.mediaType == MediaType::MUSIC_ARTIST ||
                    m_item.mediaType == MediaType::MUSIC_ALBUM ||
                    m_item.mediaType == MediaType::MUSIC_TRACK ||
                    m_item.type == "playlist");
    bool isEpisode = (m_item.mediaType == MediaType::EPISODE);
    bool isClip = (m_item.mediaType == MediaType::CLIP);

    int width, height;
    if (isMusic) {
        width = 110; height = 110;
    } else if (isEpisode || isClip) {
        width = 280; height = 160;  // Landscape still (2x display for quality)
    } else {
        width = 110; height = 165;
    }

    // For episodes, use episode's own thumb (episode still) - landscape format
    // Fall back to grandparentThumb (show poster) only if episode thumb is missing
    std::string thumbPath = m_item.thumb;
    if (isEpisode && thumbPath.empty() && !m_item.grandparentThumb.empty()) {
        thumbPath = m_item.grandparentThumb;
    }

    if (thumbPath.empty()) return;

    std::string url = client.getThumbnailUrl(thumbPath, width, height);

    ImageLoader::loadAsync(url, [](brls::Image* image) {
        // Show thumbnail once texture is loaded successfully
        image->setVisibility(brls::Visibility::VISIBLE);
    }, m_thumbnailImage, m_alive);
}

brls::View* MediaItemCell::create() {
    return new MediaItemCell();
}

void MediaItemCell::draw(NVGcontext* vg, float x, float y, float width, float height,
                          brls::Style style, brls::FrameContext* ctx) {
    brls::Box::draw(vg, x, y, width, height, style, ctx);

    // Touch press feedback overlay (like Suwayomi MangaItemCell)
    if (m_pressed) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, 8);
        nvgFillColor(vg, nvgRGBA(0, 0, 0, 80));
        nvgFill(vg);
    }
}

void MediaItemCell::onFocusGained() {
    brls::Box::onFocusGained();
    // Focused background color (like Suwayomi's getFocusedRowBg)
    this->setBackgroundColor(nvgRGBA(60, 60, 80, 255));
    // Selection border
    this->setBorderColor(nvgRGBA(229, 160, 13, 255));  // Plex orange
    this->setBorderThickness(2.0f);
    updateFocusInfo(true);
}

void MediaItemCell::onFocusLost() {
    brls::Box::onFocusLost();
    // Restore default background
    this->setBackgroundColor(nvgRGBA(50, 50, 50, 255));
    this->setBorderColor(nvgRGBA(0, 0, 0, 0));
    this->setBorderThickness(0.0f);
    m_pressed = false;
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
                // Show first 30 chars of summary to avoid overflow
                std::string summary = m_item.summary;
                if (summary.length() > 30) {
                    summary = summary.substr(0, 27) + "...";
                }
                if (!info.empty()) info += "\n";
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
        if (focused) {
            if (m_item.duration > 0) {
                int minutes = m_item.duration / 60000;
                std::string info = std::to_string(minutes) + " min";
                if (m_item.year > 0) {
                    info = std::to_string(m_item.year) + " - " + info;
                }
                m_descriptionLabel->setText(info);
                m_descriptionLabel->setVisibility(brls::Visibility::VISIBLE);
            }
            // Show full title
            m_titleLabel->setText(m_item.title);
            // Show start button hint overlay
            if (m_buttonHintBox) {
                if (m_buttonHintIcon) {
                    m_buttonHintIcon->setImageFromRes("images/start_button.png");
                }
                if (m_buttonHintLabel) m_buttonHintLabel->setVisibility(brls::Visibility::GONE);
                m_buttonHintBox->setVisibility(brls::Visibility::VISIBLE);
            }
        } else {
            m_titleLabel->setText(m_originalTitle);
            m_descriptionLabel->setVisibility(brls::Visibility::GONE);
            if (m_buttonHintBox) m_buttonHintBox->setVisibility(brls::Visibility::GONE);
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
            // Show start button hint overlay
            if (m_buttonHintBox) {
                if (m_buttonHintIcon) {
                    m_buttonHintIcon->setImageFromRes("images/start_button.png");
                }
                if (m_buttonHintLabel) m_buttonHintLabel->setVisibility(brls::Visibility::GONE);
                m_buttonHintBox->setVisibility(brls::Visibility::VISIBLE);
            }
        } else {
            m_titleLabel->setText(m_originalTitle);
            m_descriptionLabel->setVisibility(brls::Visibility::GONE);
            if (m_buttonHintBox) m_buttonHintBox->setVisibility(brls::Visibility::GONE);
        }
    } else if (m_item.mediaType == MediaType::SEASON) {
        // Show season info on focus
        if (focused) {
            m_titleLabel->setText(m_item.title);
            std::string info;
            if (m_item.leafCount > 0) {
                info = std::to_string(m_item.leafCount) + " episodes";
            }
            if (!info.empty()) {
                m_descriptionLabel->setText(info);
                m_descriptionLabel->setVisibility(brls::Visibility::VISIBLE);
            }
            // Show start button hint overlay
            if (m_buttonHintBox) {
                if (m_buttonHintIcon) {
                    m_buttonHintIcon->setImageFromRes("images/start_button.png");
                }
                if (m_buttonHintLabel) m_buttonHintLabel->setVisibility(brls::Visibility::GONE);
                m_buttonHintBox->setVisibility(brls::Visibility::VISIBLE);
            }
        } else {
            m_titleLabel->setText(m_originalTitle);
            m_descriptionLabel->setVisibility(brls::Visibility::GONE);
            if (m_buttonHintBox) m_buttonHintBox->setVisibility(brls::Visibility::GONE);
        }
    } else if (m_item.mediaType == MediaType::MUSIC_ALBUM ||
               m_item.mediaType == MediaType::MUSIC_ARTIST) {
        // Show full title and year for music items on focus
        if (focused) {
            m_titleLabel->setText(m_item.title);
            std::string info;
            if (m_item.year > 0) {
                info = std::to_string(m_item.year);
            }
            if (!info.empty()) {
                m_descriptionLabel->setText(info);
                m_descriptionLabel->setVisibility(brls::Visibility::VISIBLE);
            }
            // Show button hint overlay for albums and artists (icon only, no text)
            if (m_buttonHintBox && (m_item.mediaType == MediaType::MUSIC_ALBUM ||
                                    m_item.mediaType == MediaType::MUSIC_ARTIST)) {
                if (m_buttonHintIcon) {
                    m_buttonHintIcon->setImageFromRes("images/start_button.png");
                }
                if (m_buttonHintLabel) m_buttonHintLabel->setVisibility(brls::Visibility::GONE);
                m_buttonHintBox->setVisibility(brls::Visibility::VISIBLE);
            }
        } else {
            m_titleLabel->setText(m_originalTitle);
            m_descriptionLabel->setVisibility(brls::Visibility::GONE);
            if (m_buttonHintBox) m_buttonHintBox->setVisibility(brls::Visibility::GONE);
        }
    } else if (m_item.type == "playlist") {
        // Show full title and START button hint for playlists on focus
        if (focused) {
            m_titleLabel->setText(m_item.title);
            if (m_buttonHintBox) {
                if (m_buttonHintIcon) {
                    m_buttonHintIcon->setImageFromRes("images/start_button.png");
                }
                if (m_buttonHintLabel) m_buttonHintLabel->setVisibility(brls::Visibility::GONE);
                m_buttonHintBox->setVisibility(brls::Visibility::VISIBLE);
            }
        } else {
            m_titleLabel->setText(m_originalTitle);
            if (m_buttonHintBox) m_buttonHintBox->setVisibility(brls::Visibility::GONE);
        }
    } else if (m_item.mediaType == MediaType::CLIP) {
        // Show full title and duration for extras on focus
        if (focused) {
            m_titleLabel->setText(m_item.title);
            if (m_item.duration > 0) {
                int minutes = m_item.duration / 60000;
                std::string info;
                if (minutes > 0) {
                    info = std::to_string(minutes) + " min";
                } else {
                    int seconds = m_item.duration / 1000;
                    info = std::to_string(seconds) + " sec";
                }
                m_descriptionLabel->setText(info);
                m_descriptionLabel->setVisibility(brls::Visibility::VISIBLE);
            }
        } else {
            m_titleLabel->setText(m_originalTitle);
            m_descriptionLabel->setVisibility(brls::Visibility::GONE);
        }
    }
}

} // namespace vitaplex
