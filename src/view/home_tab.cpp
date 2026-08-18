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
#include "utils/air_time.hpp"

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

    // The "On Now" rails sit at the bottom: they turn over constantly and
    // are the longest rails on the page, so they read better below the
    // library content than wedged between it. Each starts hidden and is
    // revealed only if the provider's rail comes back with something.
    m_showsOnNowHeader = makeSectionHeader("Shows On Now");
    m_showsOnNowHeader->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_showsOnNowHeader);
    m_showsOnNowRow = createMediaRow();
    m_showsOnNowRow->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_showsOnNowRow);

    m_moviesOnNowHeader = makeSectionHeader("Movies On Now");
    m_moviesOnNowHeader->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_moviesOnNowHeader);
    m_moviesOnNowRow = createMediaRow();
    m_moviesOnNowRow->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_moviesOnNowRow);

    m_sportsOnNowHeader = makeSectionHeader("Sports On Now");
    m_sportsOnNowHeader->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_sportsOnNowHeader);
    m_sportsOnNowRow = createMediaRow();
    m_sportsOnNowRow->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_sportsOnNowRow);

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

// Rails finish loading well after the tab is on screen — often while a
// modal still sits in front of it, the Plex Home user picker at launch
// being the usual case. Populating a row clears and rebuilds its views,
// and borealis then hands focus to whatever it can find, which yanks the
// user out of the picker and into the page behind it.
//
// Guard the mutation: if focus wasn't ours to begin with, put it back
// where it was. Capture and restore happen inside one brls::sync callback
// with no suspension between them, so the captured view cannot go away
// underneath us.
namespace {
class ForeignFocusGuard {
public:
    explicit ForeignFocusGuard(brls::View* self) {
        m_keep = brls::Application::getCurrentFocus();
        m_restore = m_keep && self && !homeIsDescendantOf(m_keep, self);
    }
    ~ForeignFocusGuard() {
        if (m_restore && m_keep && brls::Application::getCurrentFocus() != m_keep)
            brls::Application::giveFocus(m_keep);
    }
private:
    brls::View* m_keep = nullptr;
    bool m_restore = false;
};
}  // namespace

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

void HomeTab::populateRow(HorizontalScrollRow* row, const std::vector<MediaItem>& items,
                          bool directPlay, bool preferPoster) {
    if (!row) return;

    row->clearViews();

    for (const auto& item : items) {
        auto* cell = new MediaItemCell();
        cell->setPreferPoster(preferPoster);   // before setItem: it picks the shape
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

        // Register START button context menus for movies, shows, and seasons.
        // Live programmes are skipped: every one of these menus acts on a
        // library ratingKey, which an EPG item does not have.
        if (!capturedItem.liveChannelKey.empty()) {
            // no context menu for live programmes
        } else if (capturedItem.mediaType == MediaType::MOVIE) {
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
            // Station logos are PNGs with transparent backgrounds, so the
            // call-sign placeholder read straight through them. Retire it
            // once the artwork lands — INVISIBLE rather than GONE, since it
            // is centred in the tile and dropping it would relayout.
            ImageLoader::loadAsync(url, [placeholder](brls::Image* img) {
                if (img) img->setVisibility(brls::Visibility::VISIBLE);
                if (placeholder) placeholder->setVisibility(brls::Visibility::INVISIBLE);
            }, preview, m_channelImgAlive);
        }

        // How much of the programme on this channel has aired, along the
        // bottom edge of the tile.
        const float aired = airProgress(ch.programStart, ch.programEnd, (int64_t)now);
        if (aired >= 0.0f) {
            auto* bar = new brls::Rectangle();
            bar->setPositionType(brls::PositionType::ABSOLUTE);
            bar->setPositionBottom(0);
            bar->setPositionLeft(0);
            bar->setHeight(3);
            bar->setWidth(std::min(std::max((float)ic.landscapeWidth * aired, 2.0f),
                                   (float)ic.landscapeWidth));
            bar->setColor(hpal::gold());
            tile->addView(bar);
        }
        cell->addView(tile);

        // Channel name.
        auto* name = new brls::Label();
        name->setText(ch.title.empty() ? ch.callSign : ch.title);
        name->setFontSize(ic.subtitleFontSize > 0 ? ic.subtitleFontSize : 14);
        name->setTextColor(hpal::text());
        name->setMarginTop(6);
        cell->addView(name);

        // "{time} · {now-playing}" subtitle, if available. The airing
        // window leads because it is short and fixed-width, so the
        // programme title is the part that truncates on a narrow cell.
        std::string sub = airWindowLabel(ch.programStart, ch.programEnd);
        if (sub.empty() && ch.channelNumber > 0) sub = std::to_string(ch.channelNumber);
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
                ForeignFocusGuard focusGuard(this);
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
            ForeignFocusGuard focusGuard(this);

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

// The Live TV rails outlive the tab instance (HomeTab is recreated on
// every tab switch). Without this, each visit to Home re-ran the whole
// fetch and competed for the single HTTPS pipe with the Live TV tab.
static std::vector<MediaItem>     s_recentItemsCache;
static std::vector<MediaItem>     s_onNowCache;
static std::vector<MediaItem>     s_moviesOnNowCache;
static std::vector<MediaItem>     s_sportsOnNowCache;
static std::vector<LiveTVChannel> s_recentChannelsCache;   // fallback path only
static time_t s_liveTVCacheAt = 0;
static std::mutex s_liveTVCacheMutex;
static constexpr time_t kLiveTVCacheTTL = 300;  // 5 minutes

void HomeTab::loadRecentChannels() {
    asyncRun([this, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        std::vector<MediaItem>     recentItems;
        std::vector<MediaItem>     onNow;
        std::vector<MediaItem>     moviesOnNow;
        std::vector<MediaItem>     sportsOnNow;
        std::vector<LiveTVChannel> channels;

        bool cached = false;
        {
            std::lock_guard<std::mutex> lock(s_liveTVCacheMutex);
            if (s_liveTVCacheAt != 0 && time(nullptr) - s_liveTVCacheAt < kLiveTVCacheTTL) {
                recentItems = s_recentItemsCache;
                onNow       = s_onNowCache;
                moviesOnNow = s_moviesOnNowCache;
                sportsOnNow = s_sportsOnNowCache;
                channels    = s_recentChannelsCache;
                cached      = true;
            }
        }

        if (!cached) {
            PlexClient& client = PlexClient::getInstance();

            // Nothing below is reachable without a DVR, and every call would
            // otherwise force the availability probe first. Skip the whole
            // rail load on servers that have no Live TV.
            if (!client.hasLiveTV() &&
                !Application::getInstance().getSettings().lastHadLiveTV) {
                return;
            }

            // One request fills every rail: the provider's discover
            // response carries Recent Channels and both "… On Now" rails
            // inline, with their items. These are the server's own rails —
            // genuinely the channels this account watched, not a guess
            // from the lineup.
            PlexClient::LiveTVHomeRails rails;
            client.fetchLiveTVHomeRails(rails);
            channels    = std::move(rails.recentChannels);
            onNow       = std::move(rails.showsOnNow);
            moviesOnNow = std::move(rails.moviesOnNow);
            sportsOnNow = std::move(rails.sportsOnNow);

            // Deliberately no EPG-grid fallback. Filling the rail with the
            // first channels of the lineup made it look like viewing
            // history to a user who had never watched a channel, and it
            // showed the same unchanging list forever. If the provider has
            // no Recent Channels hub, or the account has watched nothing,
            // the rail stays hidden — same as the official client.
            if (channels.empty())
                brls::Logger::debug("HomeTab: no recent channels — rail stays hidden");

            if (recentItems.empty() && onNow.empty() && moviesOnNow.empty() &&
                sportsOnNow.empty() && channels.empty()) {
                brls::Logger::debug("HomeTab: no live TV content (rails stay hidden)");
                return;
            }

            std::lock_guard<std::mutex> lock(s_liveTVCacheMutex);
            s_recentItemsCache    = recentItems;
            s_onNowCache          = onNow;
            s_moviesOnNowCache    = moviesOnNow;
            s_sportsOnNowCache    = sportsOnNow;
            s_recentChannelsCache = channels;
            s_liveTVCacheAt       = time(nullptr);
        } else {
            brls::Logger::debug("HomeTab: live TV rails served from cache");
        }

        brls::sync([this, recentItems, onNow, moviesOnNow, sportsOnNow, channels, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !*alive) return;
            // Revealing a rail (GONE -> VISIBLE) relayouts and repopulates,
            // so this is the most likely of the three to pull focus.
            ForeignFocusGuard focusGuard(this);

            m_recentChannelItems = recentItems;
            m_showsOnNow         = onNow;
            m_moviesOnNow        = moviesOnNow;
            m_sportsOnNow        = sportsOnNow;
            m_recentChannels     = channels;

            const bool showRecent = !m_recentChannelItems.empty() || !m_recentChannels.empty();
            if (m_recentChannelsHeader)
                m_recentChannelsHeader->setVisibility(showRecent ? brls::Visibility::VISIBLE
                                                                 : brls::Visibility::GONE);
            if (m_recentChannelsRow)
                m_recentChannelsRow->setVisibility(showRecent ? brls::Visibility::VISIBLE
                                                              : brls::Visibility::GONE);
            if (showRecent) {
                if (!m_recentChannelItems.empty()) populateRow(m_recentChannelsRow, m_recentChannelItems);
                else                               populateChannelRow();
            }

            const bool showOnNow = !m_showsOnNow.empty();
            if (m_showsOnNowHeader)
                m_showsOnNowHeader->setVisibility(showOnNow ? brls::Visibility::VISIBLE
                                                            : brls::Visibility::GONE);
            if (m_showsOnNowRow)
                m_showsOnNowRow->setVisibility(showOnNow ? brls::Visibility::VISIBLE
                                                         : brls::Visibility::GONE);
            // Shows On Now lists programmes, but reads as a row of shows —
            // poster art, like the Recently Added rails above it.
            if (showOnNow) populateRow(m_showsOnNowRow, m_showsOnNow, false, true);

            // Movies On Now items are movie-typed, so they take the poster
            // shape on their own; the flag is set anyway for the odd entry
            // the provider types as an episode.
            const bool showMovies = !m_moviesOnNow.empty();
            if (m_moviesOnNowHeader)
                m_moviesOnNowHeader->setVisibility(showMovies ? brls::Visibility::VISIBLE
                                                              : brls::Visibility::GONE);
            if (m_moviesOnNowRow)
                m_moviesOnNowRow->setVisibility(showMovies ? brls::Visibility::VISIBLE
                                                           : brls::Visibility::GONE);
            if (showMovies) populateRow(m_moviesOnNowRow, m_moviesOnNow, false, true);

            const bool showSports = !m_sportsOnNow.empty();
            if (m_sportsOnNowHeader)
                m_sportsOnNowHeader->setVisibility(showSports ? brls::Visibility::VISIBLE
                                                              : brls::Visibility::GONE);
            if (m_sportsOnNowRow)
                m_sportsOnNowRow->setVisibility(showSports ? brls::Visibility::VISIBLE
                                                           : brls::Visibility::GONE);
            // Fixtures without show art keep the landscape cell — see
            // MediaItemCell::setPreferPoster().
            if (showSports) populateRow(m_sportsOnNowRow, m_sportsOnNow, false, true);
        });
    });
}

void HomeTab::onItemSelected(const MediaItem& item) {
    // A Live TV programme's ratingKey is an EPG key, not a library one, so
    // /library/metadata 404s on it and both the detail view and the player
    // come up empty. What the user wants is the channel it is on, which is
    // the same thing the Recent Channels rail tunes.
    if (!item.liveChannelKey.empty()) {
        LiveTVChannel ch;
        ch.key           = item.liveChannelKey;
        ch.title         = item.liveChannelTitle.empty() ? item.title : item.liveChannelTitle;
        ch.currentProgram = item.title;
        // tuneChannel() looks for the programme airing now to pass its
        // metadata key through to the tune; this item is that programme.
        if (!item.key.empty() && item.airStartAt > 0 && item.airEndAt > item.airStartAt) {
            ChannelProgram prog;
            prog.title       = item.title;
            prog.metadataKey = item.key;
            prog.startTime   = item.airStartAt;
            prog.endTime     = item.airEndAt;
            ch.programs.push_back(std::move(prog));
        }
        tuneChannel(ch);
        return;
    }

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
