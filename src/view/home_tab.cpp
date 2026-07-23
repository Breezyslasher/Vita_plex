/**
 * VitaPlex - Home Tab implementation
 * A "Home" title over horizontal rails. Rails, async loads, alive-flag pattern,
 * context menus and direct-play behaviour are unchanged; the Recent Channels
 * rail is sourced from the Live TV path and tunes on click.
 */

#include "view/home_tab.hpp"
#include "view/media_item_cell.hpp"
#include "view/media_detail_view.hpp"
#include "view/long_press_gesture.hpp"
#include "app/application.hpp"
#include "utils/image_loader.hpp"
#include "utils/async.hpp"
#include "platform/platform.hpp"

#include <ctime>
#include <mutex>

namespace vitaplex {

// ── Palette (app tokens; literals per the palette spec) ──
namespace hpal {
    inline NVGcolor surface2()   { return nvgRGB(0x40, 0x40, 0x40); }
    inline NVGcolor text()       { return nvgRGB(0xFF, 0xFF, 0xFF); }
    inline NVGcolor muted()      { return nvgRGB(0xB4, 0xB4, 0xBA); }
    inline NVGcolor dim()        { return nvgRGB(0x8A, 0x8A, 0x90); }
    inline NVGcolor gold()       { return nvgRGB(0xE5, 0xA0, 0x0D); }
}

HomeTab::HomeTab() {
    const auto& ic = platform::getImageConstraints();

    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setGrow(1.0f);

    // Create vertical scrolling container for the entire tab
    m_scrollView = new brls::ScrollingFrame();
    m_scrollView->setGrow(1.0f);
    m_scrollView->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

    m_scrollContent = new brls::Box();
    m_scrollContent->setAxis(brls::Axis::COLUMN);
    m_scrollContent->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_scrollContent->setAlignItems(brls::AlignItems::STRETCH);
    m_scrollContent->setPadding(20);

    // Title
    m_titleLabel = new brls::Label();
    m_titleLabel->setText("Home");
    m_titleLabel->setFontSize(ic.homeTitleFontSize);
    m_titleLabel->setTextColor(hpal::text());
    m_titleLabel->setMarginBottom(10);
    m_scrollContent->addView(m_titleLabel);

    // Continue Watching section
    m_scrollContent->addView(makeSectionHeader("Continue Watching"));
    m_continueWatchingRow = createMediaRow();
    m_scrollContent->addView(m_continueWatchingRow);

    // Recent Channels (Live TV) — header + row start hidden; revealed only if the
    // live-tv fetch returns channels (otherwise the whole rail stays gone).
    m_recentChannelsHeader = makeSectionHeader("Recent Channels");
    m_recentChannelsHeader->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_recentChannelsHeader);
    m_recentChannelsRow = createMediaRow();
    m_recentChannelsRow->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_recentChannelsRow);

    // Recently Added Movies section
    m_scrollContent->addView(makeSectionHeader("Recently Added Movies"));
    m_moviesRow = createMediaRow();
    m_scrollContent->addView(m_moviesRow);

    // Recently Added TV Shows section
    m_scrollContent->addView(makeSectionHeader("Recently Added TV Shows"));
    m_showsRow = createMediaRow();
    m_scrollContent->addView(m_showsRow);

    // Recently Added Music section
    m_scrollContent->addView(makeSectionHeader("Recently Added Music"));
    m_musicRow = createMediaRow();
    m_scrollContent->addView(m_musicRow);

    m_scrollView->setContentView(m_scrollContent);
    this->addView(m_scrollView);

    // Load content immediately
    brls::Logger::debug("HomeTab: Loading content...");
    loadContent();
}

static bool homeIsDescendantOf(brls::View* view, brls::View* ancestor) {
    for (brls::View* v = view; v; v = v->getParent())
        if (v == ancestor) return true;
    return false;
}

void HomeTab::draw(NVGcontext* vg, float x, float y, float width, float height,
                   brls::Style style, brls::FrameContext* ctx) {
    // Vertical culling: mark rails/headers outside the page viewport
    // INVISIBLE so their whole subtree skips draw (an off-screen rail's
    // cards each cost a cover pattern + badge paths per frame otherwise).
    // INVISIBLE<->VISIBLE never touches layout; GONE children (hidden
    // Recent Channels rail) are left alone. The margin keeps the adjacent
    // rail drawable so vertical focus navigation can reach it.
    if (m_scrollView && m_scrollContent) {
        const auto& ic = platform::getImageConstraints();
        const float margin = (float)ic.homeRowHeight + 80.0f;
        const float vpTop = m_scrollView->getY();
        const float vpBottom = vpTop + m_scrollView->getHeight();
        brls::View* focus = brls::Application::getCurrentFocus();

        for (brls::View* child : m_scrollContent->getChildren()) {
            const brls::Visibility v = child->getVisibility();
            if (v == brls::Visibility::GONE) continue;

            const float cTop = child->getY();
            const float cBottom = cTop + child->getHeight();
            bool visible = (cBottom >= vpTop - margin) && (cTop <= vpBottom + margin);
            if (!visible && focus && homeIsDescendantOf(focus, child))
                visible = true;

            const brls::Visibility want = visible ? brls::Visibility::VISIBLE
                                                  : brls::Visibility::INVISIBLE;
            if (v != want) child->setVisibility(want);
        }
    }

    brls::Box::draw(vg, x, y, width, height, style, ctx);
}

brls::Box* HomeTab::makeSectionHeader(const std::string& title) {
    const auto& ic = platform::getImageConstraints();

    auto* header = new brls::Box();
    header->setAxis(brls::Axis::ROW);
    header->setAlignItems(brls::AlignItems::CENTER);
    header->setMarginTop(15);
    header->setMarginBottom(10);

    auto* accent = new brls::Rectangle();
    accent->setColor(hpal::gold());
    accent->setWidth(5);
    accent->setHeight((float)ic.homeSectionFontSize + 2.0f);
    accent->setCornerRadius(2);
    accent->setMarginRight(10);
    header->addView(accent);

    auto* label = new brls::Label();
    label->setText(title);
    label->setFontSize(ic.homeSectionFontSize);
    label->setTextColor(hpal::text());
    header->addView(label);

    return header;
}

HorizontalScrollRow* HomeTab::createMediaRow() {
    // Row height comes from the platform layer so each device picks a
    // height that comfortably fits its poster dimensions. Previously this
    // was hard-coded to 210px (Vita's value), which clipped the top and
    // bottom of taller posters on PS4 / Desktop / Android / Switch.
    const auto& ic = platform::getImageConstraints();
    auto* row = new HorizontalScrollRow();
    row->setHeight(ic.homeRowHeight);
    row->setMarginBottom(10);
    return row;
}

void HomeTab::populateRow(HorizontalScrollRow* row, const std::vector<MediaItem>& items, bool directPlay) {
    if (!row) return;

    row->clearViews();

    for (const auto& item : items) {
        auto* cell = new MediaItemCell();
        cell->setItem(item);
        cell->setMarginRight(10);

        MediaItem capturedItem = item;
        cell->registerClickAction([this, capturedItem, directPlay](brls::View* view) {
            if (directPlay) {
                // Play directly for continue watching items (movies, episodes, tracks)
                Application::getInstance().pushPlayerActivity(capturedItem.ratingKey);
            } else {
                onItemSelected(capturedItem);
            }
            return true;
        });
        cell->addGestureRecognizer(new brls::TapGestureRecognizer(cell));

        // Register START button context menus for movies, shows, and seasons
        if (capturedItem.mediaType == MediaType::MOVIE) {
            cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
                [capturedItem](brls::View* view) {
                    MediaDetailView::showMovieContextMenuStatic(capturedItem);
                    return true;
                });
        } else if (capturedItem.mediaType == MediaType::SHOW) {
            cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
                [capturedItem](brls::View* view) {
                    MediaDetailView::showShowContextMenuStatic(capturedItem);
                    return true;
                });
        } else if (capturedItem.mediaType == MediaType::SEASON) {
            cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
                [capturedItem](brls::View* view) {
                    MediaDetailView::showSeasonContextMenuStatic(capturedItem);
                    return true;
                });
        } else if (capturedItem.mediaType == MediaType::EPISODE) {
            cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
                [capturedItem](brls::View* view) {
                    MediaDetailView::showEpisodeContextMenu(capturedItem);
                    return true;
                });
        } else if (capturedItem.mediaType == MediaType::MUSIC_ARTIST) {
            cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
                [capturedItem](brls::View* view) {
                    MediaDetailView::showArtistContextMenuStatic(capturedItem);
                    return true;
                });
        } else if (capturedItem.mediaType == MediaType::MUSIC_ALBUM) {
            cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
                [capturedItem](brls::View* view) {
                    MediaDetailView::showAlbumContextMenuStatic(capturedItem);
                    return true;
                });
        }

        // Long press on touch = same as START button options
        cell->addGestureRecognizer(new LongPressGestureRecognizer(
            cell, [capturedItem](LongPressGestureStatus status) {
                if (status.state != brls::GestureState::START) {
                    return;
                }

                if (capturedItem.mediaType == MediaType::MOVIE) {
                    MediaDetailView::showMovieContextMenuStatic(capturedItem);
                } else if (capturedItem.mediaType == MediaType::SHOW) {
                    MediaDetailView::showShowContextMenuStatic(capturedItem);
                } else if (capturedItem.mediaType == MediaType::SEASON) {
                    MediaDetailView::showSeasonContextMenuStatic(capturedItem);
                } else if (capturedItem.mediaType == MediaType::EPISODE) {
                    MediaDetailView::showEpisodeContextMenu(capturedItem);
                } else if (capturedItem.mediaType == MediaType::MUSIC_ARTIST) {
                    MediaDetailView::showArtistContextMenuStatic(capturedItem);
                } else if (capturedItem.mediaType == MediaType::MUSIC_ALBUM) {
                    MediaDetailView::showAlbumContextMenuStatic(capturedItem);
                }
            }));

        row->addView(cell);
    }

    // Add placeholder if empty
    if (items.empty()) {
        auto* placeholder = new brls::Label();
        placeholder->setText("No items");
        placeholder->setFontSize(16);
        placeholder->setMarginLeft(10);
        row->addView(placeholder);
    }
}

void HomeTab::populateChannelRow() {
    if (!m_recentChannelsRow) return;
    m_recentChannelsRow->clearViews();

    // Fresh image-alive token for this batch of channel previews so any in-flight
    // load from a previous build bails before touching a freed Image.
    if (m_channelImgAlive) *m_channelImgAlive = false;
    m_channelImgAlive = std::make_shared<std::atomic<bool>>(true);

    const auto& ic = platform::getImageConstraints();
    PlexClient& client = PlexClient::getInstance();
    const time_t now = time(nullptr);

    for (const auto& ch : m_recentChannels) {
        // Dedicated 16:9 channel cell (channels don't map cleanly onto the
        // mediaType-driven MediaItemCell). Focusable Box → cyan focus ring.
        auto* cell = new brls::Box();
        cell->setAxis(brls::Axis::COLUMN);
        cell->setAlignItems(brls::AlignItems::FLEX_START);
        cell->setWidth((float)ic.landscapeWidth);
        cell->setMarginRight(14);
        cell->setCornerRadius(8);
        cell->setFocusable(true);

        // Now-playing program → episode preview art + title. Falls back to the
        // station logo only when the current program has no artwork.
        std::string previewThumb;
        std::string nowTitle = ch.currentProgram;
        for (const auto& prog : ch.programs) {
            if (prog.startTime <= (int64_t)now && prog.endTime > (int64_t)now) {
                if (!prog.thumb.empty()) previewThumb = prog.thumb;
                if (nowTitle.empty()) nowTitle = prog.title;
                break;
            }
        }
        const std::string tileSrc = !previewThumb.empty() ? previewThumb : ch.thumb;

        // 16:9 tile showing the episode preview (or a call-sign placeholder).
        auto* tile = new brls::Box();
        tile->setWidth((float)ic.landscapeWidth);
        tile->setHeight((float)ic.landscapeHeight);
        tile->setCornerRadius(8);
        tile->setBackgroundColor(hpal::surface2());
        tile->setJustifyContent(brls::JustifyContent::CENTER);
        tile->setAlignItems(brls::AlignItems::CENTER);

        auto* placeholder = new brls::Label();
        std::string ph = !ch.callSign.empty() ? ch.callSign
                         : (ch.channelNumber > 0 ? std::to_string(ch.channelNumber) : ch.title);
        placeholder->setText(ph);
        placeholder->setFontSize(18);
        placeholder->setTextColor(hpal::muted());
        tile->addView(placeholder);

        auto* preview = new brls::Image();
        preview->setPositionType(brls::PositionType::ABSOLUTE);
        preview->setPositionTop(0);
        preview->setPositionLeft(0);
        preview->setPositionRight(0);
        preview->setHeight((float)ic.landscapeHeight);
        preview->setCornerRadius(8);
        // Episode stills are ~16:9 like the tile → FILL covers without letterbox.
        preview->setScalingType(brls::ImageScalingType::FILL);
        preview->setVisibility(brls::Visibility::INVISIBLE);
        tile->addView(preview);
        if (!tileSrc.empty()) {
            std::string url = client.getThumbnailUrl(tileSrc, ic.landscapeWidth * 2, ic.landscapeHeight * 2);
            ImageLoader::loadAsync(url, [](brls::Image* img) {
                if (img) img->setVisibility(brls::Visibility::VISIBLE);
            }, preview, m_channelImgAlive);
        }
        cell->addView(tile);

        // Channel name.
        auto* name = new brls::Label();
        name->setText(ch.title.empty() ? ch.callSign : ch.title);
        name->setFontSize(ic.subtitleFontSize > 0 ? ic.subtitleFontSize : 14);
        name->setTextColor(hpal::text());
        name->setMarginTop(6);
        cell->addView(name);

        // "{number} · {now-playing}" subtitle, if available.
        std::string sub;
        if (ch.channelNumber > 0) sub = std::to_string(ch.channelNumber);
        if (!nowTitle.empty())
            sub += (sub.empty() ? "" : "  \xC2\xB7  ") + nowTitle;
        if (!sub.empty()) {
            auto* subLbl = new brls::Label();
            subLbl->setText(sub);
            subLbl->setFontSize(12);
            subLbl->setTextColor(hpal::dim());
            cell->addView(subLbl);
        }

        // Tune on click — same path the Live TV tab uses.
        LiveTVChannel captured = ch;
        cell->registerClickAction([this, captured](brls::View*) {
            tuneChannel(captured);
            return true;
        });
        cell->addGestureRecognizer(new brls::TapGestureRecognizer(cell));

        m_recentChannelsRow->addView(cell);
    }
}

void HomeTab::tuneChannel(const LiveTVChannel& channel) {
    // Mirrors LiveTVTab::onChannelSelected: resolve the tune key, find the
    // current program's metadata key, tune via PlexClient, then push the live
    // player. Nothing here touches `this` after the async resolves.
    std::string tuneCh = channel.key;
    if (tuneCh.empty()) tuneCh = channel.channelIdentifier;
    if (tuneCh.empty()) tuneCh = std::to_string(channel.channelNumber);

    std::string programMetadataKey;
    time_t now = time(nullptr);
    for (const auto& prog : channel.programs) {
        if (prog.startTime <= (int64_t)now && prog.endTime > (int64_t)now && !prog.metadataKey.empty()) {
            programMetadataKey = prog.metadataKey;
            break;
        }
    }

    asyncRun([channel, tuneCh, programMetadataKey]() {
        PlexClient& client = PlexClient::getInstance();
        std::string streamUrl;
        std::string liveSessionUuid;

        if (client.tuneLiveTVChannel(tuneCh, streamUrl, liveSessionUuid, programMetadataKey)) {
            brls::sync([streamUrl, liveSessionUuid, channel]() {
                std::string title = channel.title;
                if (!channel.currentProgram.empty()) title += " - " + channel.currentProgram;
                Application::getInstance().pushLiveTVPlayerActivity(streamUrl, title, liveSessionUuid);
            });
        } else {
            brls::Logger::error("HomeTab: Failed to tune channel {}", channel.title);
            brls::sync([channel]() {
                brls::Dialog* dialog = new brls::Dialog("Failed to tune channel: " + channel.title);
                dialog->addButton("OK", []() {});
                dialog->open();
            });
        }
    });
}

HomeTab::~HomeTab() {
    if (m_alive) { *m_alive = false; }
    if (m_channelImgAlive) { *m_channelImgAlive = false; }
}

void HomeTab::willDisappear(bool resetState) {
    brls::Box::willDisappear(resetState);
    // Invalidate alive flag so pending async callbacks bail out
    if (m_alive) *m_alive = false;
    if (m_channelImgAlive) *m_channelImgAlive = false;
    ImageLoader::cancelAll();
    // Free image cache when leaving home tab to reclaim memory
    ImageLoader::clearCache();

    // Free stored item data to reduce baseline memory
    m_continueWatching.clear();
    m_continueWatching.shrink_to_fit();
    m_recentMovies.clear();
    m_recentMovies.shrink_to_fit();
    m_recentShows.clear();
    m_recentShows.shrink_to_fit();
    m_recentMusic.clear();
    m_recentMusic.shrink_to_fit();
    m_recentChannels.clear();
    m_recentChannels.shrink_to_fit();

    // Mark as not loaded so data is re-fetched when returning
    m_loaded = false;
}

void HomeTab::onFocusGained() {
    brls::Box::onFocusGained();
    // Re-create alive flag so new async callbacks work (old ones still bail out)
    m_alive = std::make_shared<bool>(true);

    if (!m_loaded) {
        loadContent();
    }
}

void HomeTab::loadContent() {
    brls::Logger::debug("HomeTab::loadContent - Starting async load");

    // Load continue watching asynchronously
    asyncRun([this, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        brls::Logger::debug("HomeTab: Fetching continue watching (async)...");
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;

        if (client.fetchContinueWatching(items)) {
            brls::Logger::info("HomeTab: Got {} continue watching items", items.size());

            // Trim heavy fields to reduce memory
            for (auto& item : items) item.trimForGrid();

            brls::sync([this, items, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                m_continueWatching = items;
                populateRow(m_continueWatchingRow, m_continueWatching, true);
            });
        } else {
            brls::Logger::error("HomeTab: Failed to fetch continue watching");
        }
    });

    // Load the Live TV "Recent Channels" rail (hides itself if empty).
    loadRecentChannels();

    // Load recently added by fetching from library sections
    asyncRun([this, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        brls::Logger::debug("HomeTab: Fetching library sections for recently added...");
        PlexClient& client = PlexClient::getInstance();

        // First get all library sections
        std::vector<LibrarySection> sections;
        if (!client.fetchLibrarySections(sections)) {
            brls::Logger::error("HomeTab: Failed to fetch library sections");
            return;
        }

        // Get hidden libraries setting
        std::string hiddenLibraries = Application::getInstance().getSettings().hiddenLibraries;

        std::vector<MediaItem> movies;
        std::vector<MediaItem> shows;
        std::vector<MediaItem> music;

        // Helper to check if library is hidden
        auto isHidden = [&hiddenLibraries](const std::string& key) -> bool {
            if (hiddenLibraries.empty()) return false;
            std::string hidden = hiddenLibraries;
            size_t pos = 0;
            while ((pos = hidden.find(',')) != std::string::npos) {
                if (hidden.substr(0, pos) == key) return true;
                hidden.erase(0, pos + 1);
            }
            return (hidden == key);
        };

        // Fetch recently added from each section by type
        for (const auto& section : sections) {
            // Skip hidden libraries
            if (isHidden(section.key)) {
                brls::Logger::debug("HomeTab: Skipping hidden library: {}", section.title);
                continue;
            }

            std::vector<MediaItem> sectionItems;

            // Fetch recently added using the correct API endpoint
            if (client.fetchSectionRecentlyAdded(section.key, sectionItems)) {
                // Sort items by type
                for (auto& item : sectionItems) {
                    if (section.type == "movie") {
                        if (movies.size() < 8) movies.push_back(item);
                    } else if (section.type == "show") {
                        if (shows.size() < 8) shows.push_back(item);
                    } else if (section.type == "artist") {
                        if (music.size() < 8) music.push_back(item);
                    }
                }
            }
        }

        brls::Logger::info("HomeTab: Got {} movies, {} shows, {} music items",
                           movies.size(), shows.size(), music.size());

        // Trim heavy fields to reduce memory for grid display
        for (auto& item : movies) item.trimForGrid();
        for (auto& item : shows) item.trimForGrid();
        for (auto& item : music) item.trimForGrid();

        // Update UI on main thread
        brls::sync([this, movies, shows, music, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !*alive) return;

            m_recentMovies = movies;
            m_recentShows = shows;
            m_recentMusic = music;

            populateRow(m_moviesRow, m_recentMovies);
            populateRow(m_showsRow, m_recentShows);
            populateRow(m_musicRow, m_recentMusic);
        });
    });

    m_loaded = true;
    brls::Logger::debug("HomeTab: Async content loading started");
}

// The channel rail's EPG snapshot outlives the tab instance (HomeTab is
// recreated on every tab switch). Without this, each visit to Home kicks
// off a full multi-second fetchEPGGrid that competes for the single HTTPS
// pipe with the Live TV tab's own guide fetch.
static std::vector<LiveTVChannel> s_recentChannelsCache;
static time_t s_recentChannelsCacheAt = 0;
static std::mutex s_recentChannelsCacheMutex;
static constexpr time_t kRecentChannelsCacheTTL = 300;  // 5 minutes

void HomeTab::loadRecentChannels() {
    asyncRun([this, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        std::vector<LiveTVChannel> channels;
        {
            std::lock_guard<std::mutex> lock(s_recentChannelsCacheMutex);
            if (!s_recentChannelsCache.empty() &&
                time(nullptr) - s_recentChannelsCacheAt < kRecentChannelsCacheTTL) {
                channels = s_recentChannelsCache;
            }
        }

        if (channels.empty()) {
            brls::Logger::debug("HomeTab: Fetching live channels (async)...");
            PlexClient& client = PlexClient::getInstance();

            // Same data path the Live TV tab uses (fetchEPGGrid). A small window is
            // enough to surface the now-playing episode preview + tune metadata.
            if (!client.fetchEPGGrid(channels, 2)) {
                brls::Logger::debug("HomeTab: no live channels (rail stays hidden)");
                return;
            }
            if (channels.size() > 10) channels.resize(10);

            std::lock_guard<std::mutex> lock(s_recentChannelsCacheMutex);
            s_recentChannelsCache = channels;
            s_recentChannelsCacheAt = time(nullptr);
        } else {
            brls::Logger::debug("HomeTab: live channels served from cache");
        }

        brls::sync([this, channels, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !*alive) return;

            m_recentChannels = channels;
            const bool show = !m_recentChannels.empty();
            if (m_recentChannelsHeader)
                m_recentChannelsHeader->setVisibility(show ? brls::Visibility::VISIBLE
                                                           : brls::Visibility::GONE);
            if (m_recentChannelsRow)
                m_recentChannelsRow->setVisibility(show ? brls::Visibility::VISIBLE
                                                        : brls::Visibility::GONE);
            if (show) populateChannelRow();
        });
    });
}

void HomeTab::onItemSelected(const MediaItem& item) {
    // For tracks, play directly instead of showing detail view
    if (item.mediaType == MediaType::MUSIC_TRACK) {
        Application::getInstance().pushPlayerActivity(item.ratingKey);
        return;
    }

    // Show media detail view for other types
    auto* detailView = new MediaDetailView(item);
    brls::Application::pushActivity(new brls::Activity(detailView));
}

} // namespace vitaplex
