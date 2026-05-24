/**
 * VitaPlex - Media Item Cell implementation
 */

#include "view/media_item_cell.hpp"
#include "app/plex_client.hpp"
#include "app/application.hpp"
#include "utils/image_loader.hpp"
#include "platform/platform.hpp"

namespace vitaplex {

MediaItemCell::MediaItemCell()
    : m_alive(std::make_shared<std::atomic<bool>>(true)) {
    const auto& ic = platform::getImageConstraints();

    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::CENTER);
    this->setPadding(5);
    this->setFocusable(true);
    this->setCornerRadius(8);
    // Intentionally NOT calling setBackgroundColor(). Borealis's View::frame()
    // calls drawBackground() for every cell every frame as long as a background
    // colour is set — that's 4 NVG calls (beginPath, roundedRect, fillColor,
    // fill) per visible cell. With 24+ visible cells at 60 fps that becomes a
    // measurable cost on Vita, and the cover image paints right over the gray
    // anyway. Instead the cell's draw() override paints a placeholder rect
    // only for cells whose thumbnail hasn't loaded yet (see draw() below).
    // The focus highlight is still applied via setBackgroundColor() in
    // onFocusGained() — that only affects the single focused cell.

    // Thumbnail image - hidden until texture loads to prevent null texture crash on Vita
    m_thumbnailImage = new brls::Image();
    m_thumbnailImage->setWidth(ic.posterWidth);
    m_thumbnailImage->setHeight(ic.posterHeight);
    m_thumbnailImage->setScalingType(brls::ImageScalingType::FIT);
    m_thumbnailImage->setCornerRadius(4);
    m_thumbnailImage->setVisibility(brls::Visibility::INVISIBLE);
    this->addView(m_thumbnailImage);

    // Title label
    m_titleLabel = new brls::Label();
    m_titleLabel->setFontSize(ic.titleFontSize);
    m_titleLabel->setMarginTop(5);
    m_titleLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    this->addView(m_titleLabel);

    // Subtitle label (for episodes: S01E01)
    m_subtitleLabel = new brls::Label();
    m_subtitleLabel->setFontSize(ic.subtitleFontSize);
    m_subtitleLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    m_subtitleLabel->setVisibility(brls::Visibility::GONE);
    this->addView(m_subtitleLabel);

    // Description label (shows on focus for episodes)
    m_descriptionLabel = new brls::Label();
    m_descriptionLabel->setFontSize(ic.descriptionFontSize);
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

    // The start-button hint overlay used to be three child views per cell
    // (brls::Box + brls::Image + brls::Label) that borealis walked every
    // frame even when GONE. It now lives in RecyclingGrid::draw() as a
    // single NVG image painted on whichever cell currently has focus —
    // see wantsStartHint() / getThumbnailBounds() below.
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

    const auto& ic = platform::getImageConstraints();
    if (isMusic) {
        // Square album art
        m_thumbnailImage->setWidth(ic.squareCoverSize);
        m_thumbnailImage->setHeight(ic.squareCoverSize);
        // Box: cover + small padding for the title row beneath it
        this->setWidth(ic.squareCoverSize + 10);
        this->setHeight(ic.squareCoverSize + 40);
    } else if (isEpisode || isClip) {
        // Landscape episode still / extras clip
        m_thumbnailImage->setWidth(ic.landscapeWidth);
        m_thumbnailImage->setHeight(ic.landscapeHeight);
        this->setWidth(ic.landscapeWidth + 10);
        this->setHeight(ic.landscapeHeight + 45);
    } else {
        // Portrait poster
        m_thumbnailImage->setWidth(ic.posterWidth);
        m_thumbnailImage->setHeight(ic.posterHeight);
        this->setWidth(ic.posterWidth + 10);
        this->setHeight(ic.posterHeight + 35);
    }

    // Set title
    if (m_titleLabel) {
        std::string title = item.title;
        // Truncate long titles — per-platform budget from the platform layer
        // so desktop cells (which are wider) show more of the title.
        size_t maxChars = (size_t)platform::getImageConstraints().maxCellTitleChars;
        if (maxChars > 3 && title.length() > maxChars) {
            title = title.substr(0, maxChars - 2) + "...";
        }
        m_originalTitle = title;  // Store truncated title for focus restore
        m_titleLabel->setText(title);

        // Hide titles for movies and shows if setting is enabled
        bool hideTitle = Application::getInstance().getSettings().hideTitlesInGrid &&
            (item.mediaType == MediaType::MOVIE || item.mediaType == MediaType::SHOW);
        m_titleLabel->setVisibility(hideTitle ? brls::Visibility::GONE : brls::Visibility::VISIBLE);

        // Shrink box height when title is hidden to remove blank space
        if (hideTitle && !isMusic && !isEpisode) {
            this->setHeight(ic.posterHeight + 13);
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
                float barWidth = isEpisode ? (float)ic.landscapeWidth
                                           : (float)ic.posterWidth;
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

    // Request thumbnails sized for the current platform. PSV pulls down
    // tiny 110x165 covers; PS4/desktop pull down 220x330+ posters from the
    // Plex server. The constraints come from the platform layer so we never
    // hard-code per-device sizes here.
    const auto& ic = platform::getImageConstraints();
    int width, height;
    if (isMusic) {
        width = ic.squareCoverSize;
        height = ic.squareCoverSize;
    } else if (isEpisode || isClip) {
        width = ic.landscapeWidth;
        height = ic.landscapeHeight;
    } else {
        width = ic.posterWidth;
        height = ic.posterHeight;
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
    // Placeholder background — only painted while the cover is still loading.
    // Once ImageLoader flips the brls::Image to VISIBLE, the cover overlays
    // this region and we skip the draw entirely. This is what replaces the
    // ctor-level setBackgroundColor() that used to fire 4 NVG calls per cell
    // per frame.
    if (m_thumbnailImage &&
        m_thumbnailImage->getVisibility() != brls::Visibility::VISIBLE) {
        float tw = m_thumbnailImage->getWidth();
        float th = m_thumbnailImage->getHeight();
        if (tw > 0 && th > 0) {
            float tx = m_thumbnailImage->getX();
            float ty = m_thumbnailImage->getY();
            nvgBeginPath(vg);
            nvgRoundedRect(vg, tx, ty, tw, th, 4.0f);
            nvgFillColor(vg, nvgRGB(40, 40, 48));
            nvgFill(vg);
        }
    }

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
    // Reset to transparent rather than the prior solid gray. The cell-wide
    // gray was redrawn every frame by borealis as long as alpha > 0; clearing
    // it here is what makes the perf win from the ctor stick once the user
    // moves focus away. Loaded covers still occlude the cell area; the
    // placeholder in draw() handles cells that haven't loaded yet.
    this->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
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
        } else {
            m_titleLabel->setText(m_originalTitle);
            m_descriptionLabel->setVisibility(brls::Visibility::GONE);
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
        } else {
            m_titleLabel->setText(m_originalTitle);
            m_descriptionLabel->setVisibility(brls::Visibility::GONE);
        }
    } else if (m_item.type == "playlist") {
        // Show full title for playlists on focus
        if (focused) {
            m_titleLabel->setText(m_item.title);
        } else {
            m_titleLabel->setText(m_originalTitle);
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

bool MediaItemCell::wantsStartHint() const {
    // Match the prior per-cell logic in updateFocusInfo(): movies, shows,
    // seasons, albums, artists, and playlists all surfaced the start
    // button hint when focused. Episodes/tracks/clips intentionally did
    // not — keep that behaviour identical here.
    if (m_item.type == "playlist") return true;
    switch (m_item.mediaType) {
        case MediaType::MOVIE:
        case MediaType::SHOW:
        case MediaType::SEASON:
        case MediaType::MUSIC_ALBUM:
        case MediaType::MUSIC_ARTIST:
            return true;
        default:
            return false;
    }
}

void MediaItemCell::getThumbnailBounds(float& tx, float& ty, float& tw, float& th) const {
    if (m_thumbnailImage) {
        tx = m_thumbnailImage->getX();
        ty = m_thumbnailImage->getY();
        tw = m_thumbnailImage->getWidth();
        th = m_thumbnailImage->getHeight();
    } else {
        tx = ty = tw = th = 0.0f;
    }
}

} // namespace vitaplex
