/**
 * VitaPlex - Media Item Cell implementation
 */

#include "view/media_item_cell.hpp"
#include "app/plex_client.hpp"
#include "app/application.hpp"
#include "app/plex_palette.hpp"
#include "app/hint_icons.hpp"
#include "utils/image_loader.hpp"
#include "platform/platform.hpp"
#include <algorithm>
#include <cmath>

namespace vitaplex {

// Shared start-button hint texture. Loaded lazily on the first focused
// cell that wants it (see draw()). One tiny PNG shared by every grid
// cell. On desktop/android the icon set tracks the live input source
// (Steam Deck / Keyboard / Touch), so we also track the loaded path and
// reload when it flips. Older handles are deleted on the next draw via
// nvgDeleteImage — leaking a handle per source-flip would be tiny but
// unnecessary.
static int         s_startHintNvg  = 0;
static int         s_startHintW    = 0;
static int         s_startHintH    = 0;
static std::string s_startHintPath;

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

    // Transparent layout slot for the cover. The actual cover is painted
    // by RecyclingGrid::draw() in a single batched nvgImagePattern pass,
    // which removes one brls::Image per cell from the borealis view tree
    // (one fewer frame()/drawBackground()/transform stack per cell every
    // frame). The slot still participates in flex layout so titles and
    // progress bars stay positioned correctly underneath.
    m_coverSlot = new brls::Box();
    m_coverSlot->setWidth(ic.posterWidth);
    m_coverSlot->setHeight(ic.posterHeight);
    this->addView(m_coverSlot);

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
    // see wantsStartHint() / getCoverBounds() below.
}

MediaItemCell::~MediaItemCell() {
    // Signal to any in-flight async image loads that this cell is destroyed
    if (m_alive) {
        m_alive->store(false);
    }
    // Release the GPU texture for the cover. ImageLoader's alive-flag
    // check handles the case where the callback hasn't fired yet — if it
    // does, the loader notices !alive and deletes the handle for us.
    if (m_nvgCover != 0) {
        NVGcontext* vg = brls::Application::getNVGContext();
        if (vg) nvgDeleteImage(vg, m_nvgCover);
        m_nvgCover = 0;
    }
}

void MediaItemCell::setItem(const MediaItem& item) {
    m_item = item;
    m_ratingTextW = -1.0f;  // re-measure the cover badges for the new item
    m_charTextW   = -1.0f;

    // Item-change resets any previously-loaded cover. The grid pass will
    // paint a placeholder rect for this cell until loadThumbnail() fires
    // its callback. We delete the old NVG handle eagerly rather than
    // overwriting m_nvgCover; otherwise the old texture leaks on every
    // cell-recycle.
    if (m_nvgCover != 0) {
        NVGcontext* vg = brls::Application::getNVGContext();
        if (vg) nvgDeleteImage(vg, m_nvgCover);
        m_nvgCover = 0;
        m_coverW = 0;
        m_coverH = 0;
    }

    // Adjust cover slot size based on media type
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

    // Label-area padding beneath the cover. Portrait phones / narrow
    // viewports tighten this aggressively so the user doesn't end up
    // staring at empty bands between rows in a music-artist grid (the
    // screenshot bug). Episodes keep slightly more room because their
    // subtitle line (S01E01) is visible.
    const bool portrait     = platform::isPortrait();
    const int  musicLabel   = portrait ? 26 : 40;
    const int  posterLabel  = portrait ? 24 : 35;
    const int  landscLabel  = portrait ? 32 : 45;

    if (isMusic) {
        // Square album art
        m_coverSlot->setWidth(ic.squareCoverSize);
        m_coverSlot->setHeight(ic.squareCoverSize);
        this->setWidth(ic.squareCoverSize + 10);
        this->setHeight(ic.squareCoverSize + musicLabel);
    } else if (isEpisode || isClip) {
        // Landscape episode still / extras clip
        m_coverSlot->setWidth(ic.landscapeWidth);
        m_coverSlot->setHeight(ic.landscapeHeight);
        this->setWidth(ic.landscapeWidth + 10);
        this->setHeight(ic.landscapeHeight + landscLabel);
    } else {
        // Portrait poster
        m_coverSlot->setWidth(ic.posterWidth);
        m_coverSlot->setHeight(ic.posterHeight);
        this->setWidth(ic.posterWidth + 10);
        this->setHeight(ic.posterHeight + posterLabel);
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

        // Hide titles for all media types when the toggle is on. Previously
        // restricted to movies/shows; the user wanted it global so seasons,
        // episodes, albums, etc. also drop their titles.
        bool hideTitle = Application::getInstance().getSettings().hideTitlesInGrid;
        m_titleLabel->setVisibility(hideTitle ? brls::Visibility::GONE : brls::Visibility::VISIBLE);

        // Shrink box height when title is hidden to remove blank space.
        // Per-shape so square / landscape cells don't leak the portrait
        // padding offset when the title row disappears.
        if (hideTitle) {
            if (isMusic) {
                this->setHeight(ic.squareCoverSize + 10);
            } else if (isEpisode || isClip) {
                this->setHeight(ic.landscapeHeight + 13);
            } else {
                this->setHeight(ic.posterHeight + 13);
            }
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

    // The lambda captures `this` so the callback can write into the cell.
    // ImageLoader passes the alive flag through; if the cell has been
    // destroyed by the time the load completes, ImageLoader cleans up
    // the NVG handle on our behalf rather than calling the callback.
    MediaItemCell* self = this;
    ImageLoader::loadCoverAsync(url,
        [self](int nvgImg, int w, int h) {
            // If a newer setItem() ran while we were loading, it already
            // deleted whatever m_nvgCover used to point at. Replace with
            // the freshly-decoded handle.
            if (self->m_nvgCover != 0 && self->m_nvgCover != nvgImg) {
                NVGcontext* vg = brls::Application::getNVGContext();
                if (vg) nvgDeleteImage(vg, self->m_nvgCover);
            }
            self->m_nvgCover = nvgImg;
            self->m_coverW   = w;
            self->m_coverH   = h;
        },
        m_alive);
}

brls::View* MediaItemCell::create() {
    return new MediaItemCell();
}

void MediaItemCell::draw(NVGcontext* vg, float x, float y, float width, float height,
                          brls::Style style, brls::FrameContext* ctx) {
    // Cell background, labels, progress bar, etc. paint first.
    brls::Box::draw(vg, x, y, width, height, style, ctx);

    // Cover paint lives here (not in the parent) so every place that
    // hosts a MediaItemCell gets the picture, not just RecyclingGrid:
    // the home tab's HorizontalScrollRow and the media-detail view's
    // brls::HScrollingFrame containers (seasons / episodes / extras /
    // album tracks) used to render nothing because the prior batched
    // pass was grid-only. Parent containers already set clipsToBounds
    // or a ScrollingFrame scissor, so off-screen cells' paints are
    // clipped without us adding our own scissor here.
    if (m_coverSlot) {
        float cx = m_coverSlot->getX();
        float cy = m_coverSlot->getY();
        float cw = m_coverSlot->getWidth();
        float ch = m_coverSlot->getHeight();
        if (cw > 0.0f && ch > 0.0f) {
            if (m_nvgCover != 0 && m_coverW > 0 && m_coverH > 0) {
                // Letterbox-fit, same as the prior batched pass.
                float scale = std::min(cw / (float)m_coverW,
                                       ch / (float)m_coverH);
                float sw = (float)m_coverW * scale;
                float sh = (float)m_coverH * scale;
                float ox = cx + (cw - sw) * 0.5f;
                float oy = cy + (ch - sh) * 0.5f;
                NVGpaint paint = nvgImagePattern(
                    vg, ox, oy, sw, sh, 0, m_nvgCover, 1.0f);
                nvgBeginPath(vg);
                nvgRoundedRect(vg, ox, oy, sw, sh, 4.0f);
                nvgFillPaint(vg, paint);
                nvgFill(vg);
            } else {
                // Placeholder until the async loader fires its callback.
                nvgBeginPath(vg);
                nvgRoundedRect(vg, cx, cy, cw, ch, 4.0f);
                nvgFillColor(vg, nvgRGB(40, 40, 48));
                nvgFill(vg);
            }

            // ----- overlay badges on the cover -----
            // Rating (top-right), shown anywhere a rating is known so movie /
            // show / home / search poster grids all get it. Movies use a
            // Rotten-Tomatoes-style popcorn percentage (audience score preferred,
            // else the critic rating scaled to %); everything else keeps the
            // ★ out-of-ten.
            {
                const bool isMovie = (m_item.mediaType == MediaType::MOVIE);
                bool showPct = false, showStar = false;
                float pct = 0.0f;
                if (isMovie) {
                    float src = (m_item.audienceRating > 0.0f) ? m_item.audienceRating
                                                              : m_item.rating;
                    if (src > 0.0f) { pct = src * 10.0f; showPct = true; }
                } else if (m_item.rating > 0.0f) {
                    showStar = true;
                }

                if (showPct || showStar) {
                    char rbuf[16];
                    if (showPct) snprintf(rbuf, sizeof(rbuf), "%d%%", (int)(pct + 0.5f));
                    else         snprintf(rbuf, sizeof(rbuf), "%.1f", m_item.rating);

                    nvgFontFace(vg, "regular");
                    nvgFontSize(vg, 12.0f);
                    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    // The badge string is static per item — measure once, not
                    // per frame (nvgTextBounds does fontstash shaping).
                    if (m_ratingTextW < 0.0f) {
                        float tb[4];
                        m_ratingTextW = nvgTextBounds(vg, 0, 0, rbuf, nullptr, tb);
                    }
                    float numW = m_ratingTextW;

                    const float bh = 20.0f, iconR = 6.0f, padH = 6.0f, gap = 4.0f;
                    float bw = padH + iconR * 2.0f + gap + numW + padH;
                    float bx = cx + cw - bw - 6.0f;
                    float by = cy + 6.0f;

                    nvgBeginPath(vg);
                    nvgRoundedRect(vg, bx, by, bw, bh, 6.0f);
                    nvgFillColor(vg, nvgRGBA(0, 0, 0, 170));
                    nvgFill(vg);

                    float icx = bx + padH + iconR, icy = by + bh * 0.5f;
                    if (showPct) {
                        // Popcorn: a red bucket with buttery kernels (all paths,
                        // so it never depends on an emoji / font glyph).
                        float s = iconR * 2.0f;
                        float bkW = s * 0.80f, bkH = s * 0.62f;
                        float bkx = icx - bkW * 0.5f, bky = icy + s * 0.5f - bkH;
                        nvgBeginPath(vg);
                        nvgMoveTo(vg, bkx + bkW * 0.14f, bky);
                        nvgLineTo(vg, bkx + bkW * 0.86f, bky);
                        nvgLineTo(vg, bkx + bkW, bky + bkH);
                        nvgLineTo(vg, bkx, bky + bkH);
                        nvgClosePath(vg);
                        nvgFillColor(vg, nvgRGB(225, 60, 45));
                        nvgFill(vg);
                        nvgFillColor(vg, nvgRGB(245, 214, 130));
                        nvgBeginPath(vg); nvgCircle(vg, icx - s * 0.22f, bky - s * 0.02f, s * 0.16f); nvgFill(vg);
                        nvgBeginPath(vg); nvgCircle(vg, icx + s * 0.02f, bky - s * 0.12f, s * 0.18f); nvgFill(vg);
                        nvgBeginPath(vg); nvgCircle(vg, icx + s * 0.24f, bky - s * 0.02f, s * 0.15f); nvgFill(vg);
                    } else {
                        // Gold star path.
                        nvgBeginPath(vg);
                        for (int i = 0; i < 10; i++) {
                            float ang = -1.5707963f + (float)i * 0.6283185f;  // -90° step 36°
                            float rad = (i % 2 == 0) ? iconR : iconR * 0.42f;
                            float px = icx + cosf(ang) * rad;
                            float py = icy + sinf(ang) * rad;
                            if (i == 0) nvgMoveTo(vg, px, py);
                            else        nvgLineTo(vg, px, py);
                        }
                        nvgClosePath(vg);
                        nvgFillColor(vg, nvgRGB(229, 160, 13));
                        nvgFill(vg);
                    }

                    nvgFillColor(vg, nvgRGB(255, 255, 255));
                    nvgText(vg, bx + padH + iconR * 2.0f + gap, icy, rbuf, nullptr);
                }
            }

            // Role badge (top-left): "as {character}" / "Director" / "Writer".
            // Only set on person-results posters, so it stays scoped there.
            if (!m_item.character.empty()) {
                nvgFontFace(vg, "regular");
                nvgFontSize(vg, 11.0f);
                nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                if (m_charTextW < 0.0f) {
                    float tb[4];
                    m_charTextW = nvgTextBounds(vg, 0, 0, m_item.character.c_str(), nullptr, tb);
                }
                float tw = m_charTextW;

                const float bh = 18.0f, padH = 7.0f;
                float bw = tw + padH * 2.0f;
                float maxW = cw - 12.0f;
                if (bw > maxW) bw = maxW;
                float bx = cx + 6.0f, by = cy + 6.0f;

                nvgBeginPath(vg);
                nvgRoundedRect(vg, bx, by, bw, bh, 5.0f);
                nvgFillColor(vg, nvgRGBA(0, 0, 0, 153));  // ~.6
                nvgFill(vg);

                nvgSave(vg);
                nvgScissor(vg, bx, by, bw, bh);
                nvgFillColor(vg, nvgRGB(232, 232, 236));  // #E8E8EC
                nvgText(vg, bx + padH, by + bh * 0.5f, m_item.character.c_str(), nullptr);
                nvgRestore(vg);
            }
        }
    }

    // Touch-press tint paints AFTER the cover so it dims the picture
    // too (same visual as before — used to be after Box::draw which
    // only had the cell background underneath).
    if (m_pressed) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, 8);
        nvgFillColor(vg, nvgRGBA(0, 0, 0, 80));
        nvgFill(vg);
    }

    // Start-button hint on the focused cell, painted last so it always
    // sits on top of the cover. Shared static texture: loaded once on
    // first paint, never released (one small PNG for the app lifetime).
    if (this->isFocused() && wantsStartHint() && m_coverSlot) {
        std::string path = HintIcons::getResPath(brls::BUTTON_START);
        if (!path.empty() && path != s_startHintPath) {
            if (s_startHintNvg != 0) nvgDeleteImage(vg, s_startHintNvg);
            std::string full = std::string(RESOURCE_PREFIX) + path;
            s_startHintNvg = nvgCreateImage(vg, full.c_str(), 0);
            if (s_startHintNvg != 0) {
                nvgImageSize(vg, s_startHintNvg,
                             &s_startHintW, &s_startHintH);
                s_startHintPath = path;
            }
        }
        if (s_startHintNvg != 0 && s_startHintW > 0 && s_startHintH > 0) {
            float fcx = m_coverSlot->getX();
            float fcy = m_coverSlot->getY();
            float hintW = (float)s_startHintW;
            float hintH = (float)s_startHintH;
            // Top-LEFT corner of the cover with a small inset, so the focus hint
            // no longer sits on top of the top-right rating badge.
            float hx = fcx + 7.0f;
            float hy = fcy + 7.0f;
            NVGpaint hp = nvgImagePattern(
                vg, hx, hy, hintW, hintH, 0, s_startHintNvg, 1.0f);
            nvgBeginPath(vg);
            nvgRect(vg, hx, hy, hintW, hintH);
            nvgFillPaint(vg, hp);
            nvgFill(vg);
        }
    }
}

void MediaItemCell::onFocusGained() {
    brls::Box::onFocusGained();
    // Focus = warm cream halo, never a gold fill (palette rule 1): a raised
    // warm surface behind the cover + a bright gold-white ring. Keeping the
    // ring off-gold means a focused poster never reads like a gold "selected"
    // state. (borealis also paints its native warm highlight on top.)
    this->setBackgroundColor(vitaplex::palette::surface2);
    this->setBorderColor(vitaplex::palette::focusHalo);
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
        case MediaType::EPISODE:
        case MediaType::MUSIC_ALBUM:
        case MediaType::MUSIC_ARTIST:
            return true;
        default:
            return false;
    }
}

void MediaItemCell::getCoverBounds(float& cx, float& cy, float& cw, float& ch) const {
    if (m_coverSlot) {
        cx = m_coverSlot->getX();
        cy = m_coverSlot->getY();
        cw = m_coverSlot->getWidth();
        ch = m_coverSlot->getHeight();
    } else {
        cx = cy = cw = ch = 0.0f;
    }
}

} // namespace vitaplex
