/**
 * VitaPlex - Home Tab implementation
 * Direction A: featured hero + horizontal rails. The first rail overlaps up
 * into the hero's bottom fade. Rails, async loads, alive-flag pattern, context
 * menus and direct-play behaviour are unchanged from the previous layout.
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
#include <cstdio>

namespace vitaplex {

// ── Direction A palette (app tokens; literals per the palette spec) ──
namespace hpal {
    inline NVGcolor surface()    { return nvgRGB(0x38, 0x38, 0x38); }
    inline NVGcolor surface2()   { return nvgRGB(0x40, 0x40, 0x40); }
    inline NVGcolor surface3()   { return nvgRGB(0x49, 0x49, 0x49); }
    inline NVGcolor text()       { return nvgRGB(0xFF, 0xFF, 0xFF); }
    inline NVGcolor muted()      { return nvgRGB(0xB4, 0xB4, 0xBA); }
    inline NVGcolor dim()        { return nvgRGB(0x8A, 0x8A, 0x90); }
    inline NVGcolor gold()       { return nvgRGB(0xE5, 0xA0, 0x0D); }
    inline NVGcolor goldBright() { return nvgRGB(0xFF, 0xC2, 0x3D); }
    inline NVGcolor goldInk()    { return nvgRGB(0x24, 0x1C, 0x08); }
}

// Two stacked scrims over the hero art so the bottom-left text and the rail
// that overlaps the hero stay legible. borealis' built-in gradient background
// exposes no public start/end colour setter, so we paint them by hand.
namespace {
class HeroScrim : public brls::View {
public:
    void draw(NVGcontext* vg, float x, float y, float w, float h,
              brls::Style style, brls::FrameContext* ctx) override {
        if (w <= 0.0f || h <= 0.0f) return;
        // Left → right: dark on the title side, clearing toward the art.
        NVGpaint side = nvgLinearGradient(vg, x, y, x + w * 0.62f, y,
            nvgRGBA(45, 45, 45, 245), nvgRGBA(45, 45, 45, 0));
        nvgBeginPath(vg); nvgRect(vg, x, y, w, h);
        nvgFillPaint(vg, side); nvgFill(vg);
        // Bottom → up: anchors the hero into the page background / rail fade.
        NVGpaint bottom = nvgLinearGradient(vg, x, y + h, x, y + h * 0.42f,
            nvgRGBA(45, 45, 45, 255), nvgRGBA(45, 45, 45, 0));
        nvgBeginPath(vg); nvgRect(vg, x, y, w, h);
        nvgFillPaint(vg, bottom); nvgFill(vg);
    }
};
} // namespace

HomeTab::HomeTab() {
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
    // No outer padding: the hero is full-bleed; rails get their own side padding.

    // Header region (cinematic hero OR plain "Home" title), filled by buildHeader().
    m_heroContainer = new brls::Box();
    m_heroContainer->setAxis(brls::Axis::COLUMN);
    m_heroContainer->setAlignItems(brls::AlignItems::STRETCH);
    m_scrollContent->addView(m_heroContainer);

    // Rails container — side padding so the hero can bleed to the content edges.
    m_railsContainer = new brls::Box();
    m_railsContainer->setAxis(brls::Axis::COLUMN);
    m_railsContainer->setAlignItems(brls::AlignItems::STRETCH);
    m_railsContainer->setPaddingLeft(20);
    m_railsContainer->setPaddingRight(20);
    m_railsContainer->setPaddingBottom(20);

    // Continue Watching section
    m_railsContainer->addView(makeSectionHeader("Continue Watching"));
    m_continueWatchingRow = createMediaRow();
    m_railsContainer->addView(m_continueWatchingRow);

    // Recent Channels (Live TV) — NEW. Header + row start hidden; revealed only
    // if the live-tv fetch returns channels (otherwise the whole rail stays gone).
    m_recentChannelsHeader = makeSectionHeader("Recent Channels");
    m_recentChannelsHeader->setVisibility(brls::Visibility::GONE);
    m_railsContainer->addView(m_recentChannelsHeader);
    m_recentChannelsRow = createMediaRow();
    m_recentChannelsRow->setVisibility(brls::Visibility::GONE);
    m_railsContainer->addView(m_recentChannelsRow);

    // Recently Added Movies section
    m_railsContainer->addView(makeSectionHeader("Recently Added Movies"));
    m_moviesRow = createMediaRow();
    m_railsContainer->addView(m_moviesRow);

    // Recently Added TV Shows section
    m_railsContainer->addView(makeSectionHeader("Recently Added TV Shows"));
    m_showsRow = createMediaRow();
    m_railsContainer->addView(m_showsRow);

    // Recently Added Music section
    m_railsContainer->addView(makeSectionHeader("Recently Added Music"));
    m_musicRow = createMediaRow();
    m_railsContainer->addView(m_musicRow);

    m_scrollContent->addView(m_railsContainer);

    m_scrollView->setContentView(m_scrollContent);
    this->addView(m_scrollView);

    // Build the header for the first time (reads the "Show featured banner" setting).
    buildHeader();

    // React to orientation flips: re-sizes / re-stacks the hero. Uses a
    // dedicated lifetime flag (m_alive is recycled on focus).
    platform::onOrientationChanged([this, aliveWeak = std::weak_ptr<bool>(m_orientationAlive)]() {
        auto alive = aliveWeak.lock();
        if (!alive || !*alive) return;
        rebuildHeader();
    });

    // Load content immediately
    brls::Logger::debug("HomeTab: Loading content...");
    loadContent();
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

void HomeTab::buildHeader() {
    if (!m_heroContainer) return;
    m_heroContainer->clearViews();   // frees the previous hero / title subtree
    m_heroPlayButton = nullptr;      // both handles point into what we just freed
    m_titleLabel = nullptr;

    const bool heroOn = Application::getInstance().getSettings().showFeaturedBanner;
    m_lastShowFeaturedBanner = heroOn;

    if (heroOn) {
        buildHero(m_heroContainer);
        // First rail overlaps up into the hero's bottom fade.
        if (m_railsContainer) m_railsContainer->setMarginTop(-36.0f);
    } else {
        const auto& ic = platform::getImageConstraints();
        m_titleLabel = new brls::Label();
        m_titleLabel->setText("Home");
        m_titleLabel->setFontSize(ic.homeTitleFontSize);
        m_titleLabel->setTextColor(hpal::text());
        m_titleLabel->setMarginLeft(20);
        m_titleLabel->setMarginTop(20);
        m_titleLabel->setMarginBottom(6);
        m_heroContainer->addView(m_titleLabel);
        if (m_railsContainer) m_railsContainer->setMarginTop(0.0f);
    }
}

void HomeTab::buildHero(brls::Box* parent) {
    const bool portrait = platform::isPortrait();
    const float heroH = portrait ? 300.0f : 380.0f;

    auto* hero = new brls::Box();
    hero->setHeight(heroH);
    hero->setAlignItems(brls::AlignItems::STRETCH);
    hero->setBackgroundColor(hpal::surface());   // placeholder behind the art

    // 1) Background art (backdrop, falling back to the poster thumb).
    auto* art = new brls::Image();
    art->setPositionType(brls::PositionType::ABSOLUTE);
    art->setPositionTop(0);
    art->setPositionLeft(0);
    art->setPositionRight(0);                            // left+right → stretch to hero width
    art->setHeight(heroH);
    art->setScalingType(brls::ImageScalingType::FILL);   // cover the hero box
    art->setVisibility(brls::Visibility::INVISIBLE);
    hero->addView(art);

    if (m_heroResolved) {
        std::string src = !m_heroItem.art.empty() ? m_heroItem.art : m_heroItem.thumb;
        if (!src.empty()) {
            if (m_heroImgAlive) *m_heroImgAlive = false;
            m_heroImgAlive = std::make_shared<std::atomic<bool>>(true);
            std::string url = PlexClient::getInstance().getThumbnailUrl(src, 960, 540);
            ImageLoader::loadAsync(url, [](brls::Image* img) {
                if (img) img->setVisibility(brls::Visibility::VISIBLE);
            }, art, m_heroImgAlive);
        }
    }

    // 2) Scrims.
    auto* scrim = new HeroScrim();
    scrim->setPositionType(brls::PositionType::ABSOLUTE);
    scrim->setPositionTop(0);
    scrim->setPositionLeft(0);
    scrim->setPositionRight(0);
    scrim->setHeight(heroH);
    hero->addView(scrim);

    // 3) Bottom-left content stack (above the scrims).
    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setAlignItems(brls::AlignItems::FLEX_START);
    content->setPositionType(brls::PositionType::ABSOLUTE);
    content->setPositionLeft(portrait ? 20.0f : 40.0f);
    content->setPositionRight(portrait ? 20.0f : 60.0f);   // bound width so the title wraps
    content->setPositionBottom(portrait ? 40.0f : 52.0f);

    // Gold kicker chip.
    auto* kicker = new brls::Box();
    kicker->setAxis(brls::Axis::ROW);
    kicker->setAlignItems(brls::AlignItems::CENTER);
    kicker->setJustifyContent(brls::JustifyContent::CENTER);
    kicker->setHeight(22);
    kicker->setPadding(0, 10, 0, 10);
    kicker->setCornerRadius(4);
    kicker->setBackgroundColor(hpal::gold());
    kicker->setMarginBottom(10);
    auto* kickerLbl = new brls::Label();
    kickerLbl->setText(m_heroFromContinue ? "CONTINUE WATCHING" : "FEATURED");
    kickerLbl->setFontSize(13);
    kickerLbl->setTextColor(hpal::goldInk());
    kicker->addView(kickerLbl);
    content->addView(kicker);

    // Title.
    auto* title = new brls::Label();
    title->setText(m_heroResolved ? m_heroItem.title : "Home");
    title->setFontSize(portrait ? 32 : 46);
    title->setTextColor(hpal::text());
    title->setMarginBottom(8);
    content->addView(title);

    // Metadata line (★ rating · year · rating pill · runtime) — resolved only.
    if (m_heroResolved) {
        auto* meta = new brls::Box();
        meta->setAxis(brls::Axis::ROW);
        meta->setAlignItems(brls::AlignItems::CENTER);
        meta->setMarginBottom(14);
        bool any = false;

        if (m_heroItem.rating > 0.0f) {
            auto* star = new brls::Image();
            star->setImageFromRes("icons/mini_star.png");
            star->setWidth(15);
            star->setHeight(15);
            star->setMarginRight(5);
            meta->addView(star);
            auto* r = new brls::Label();
            char buf[16];
            snprintf(buf, sizeof(buf), "%.1f", m_heroItem.rating);
            r->setText(buf);
            r->setFontSize(15);
            r->setTextColor(hpal::goldBright());
            r->setMarginRight(14);
            meta->addView(r);
            any = true;
        }
        if (m_heroItem.year > 0) {
            auto* y = new brls::Label();
            y->setText(std::to_string(m_heroItem.year));
            y->setFontSize(15);
            y->setTextColor(hpal::muted());
            y->setMarginRight(14);
            meta->addView(y);
            any = true;
        }
        if (!m_heroItem.contentRating.empty()) {
            auto* pill = new brls::Box();
            pill->setHeight(22);
            pill->setCornerRadius(4);
            pill->setPadding(0, 8, 0, 8);
            pill->setJustifyContent(brls::JustifyContent::CENTER);
            pill->setAlignItems(brls::AlignItems::CENTER);
            pill->setBackgroundColor(hpal::surface3());
            pill->setMarginRight(14);
            auto* pl = new brls::Label();
            pl->setText(m_heroItem.contentRating);
            pl->setFontSize(12);
            pl->setTextColor(hpal::muted());
            pill->addView(pl);
            meta->addView(pill);
            any = true;
        }
        if (m_heroItem.duration > 0) {
            auto* d = new brls::Label();
            d->setText(std::to_string(m_heroItem.duration / 60000) + " min");
            d->setFontSize(15);
            d->setTextColor(hpal::muted());
            meta->addView(d);
            any = true;
        }
        if (any) content->addView(meta);
    }

    // Action row: Play / Resume (gold) + More info (secondary).
    auto* actions = new brls::Box();
    actions->setAxis(brls::Axis::ROW);
    actions->setAlignItems(brls::AlignItems::CENTER);

    const MediaItem heroItem = m_heroItem;
    const bool resolved = m_heroResolved;
    const bool resume = m_heroResolved && m_heroItem.viewOffset > 30000;

    m_heroPlayButton = new brls::Button();
    m_heroPlayButton->setText(resume ? "Resume" : "Play");
    m_heroPlayButton->setHeight(46);
    m_heroPlayButton->setPadding(0, 22, 0, 44);
    m_heroPlayButton->setCornerRadius(8);
    m_heroPlayButton->setTextColor(hpal::goldInk());     // label first; applyStyle resets bg
    m_heroPlayButton->setBackgroundColor(hpal::gold());
    m_heroPlayButton->setMarginRight(12);
    m_heroPlayButton->registerClickAction([resolved, heroItem](brls::View*) {
        if (resolved) Application::getInstance().pushPlayerActivity(heroItem.ratingKey);
        return true;
    });
    m_heroPlayButton->addGestureRecognizer(new brls::TapGestureRecognizer(m_heroPlayButton));
    {
        auto* pIcon = new brls::Image();
        pIcon->setImageFromRes("icons/play.png");
        pIcon->setWidth(18);
        pIcon->setHeight(18);
        pIcon->setScalingType(brls::ImageScalingType::FIT);
        pIcon->setPositionType(brls::PositionType::ABSOLUTE);
        pIcon->setPositionLeft(16);
        pIcon->setPositionTop(14);
        m_heroPlayButton->addView(pIcon);
    }
    actions->addView(m_heroPlayButton);

    auto* moreInfo = new brls::Button();
    moreInfo->setText("More info");
    moreInfo->setHeight(46);
    moreInfo->setPadding(0, 20, 0, 20);
    moreInfo->setCornerRadius(8);
    moreInfo->setTextColor(hpal::text());
    moreInfo->setBackgroundColor(hpal::surface2());
    moreInfo->registerClickAction([resolved, heroItem](brls::View*) {
        if (resolved) {
            auto* dv = new MediaDetailView(heroItem);
            brls::Application::pushActivity(new brls::Activity(dv));
        }
        return true;
    });
    moreInfo->addGestureRecognizer(new brls::TapGestureRecognizer(moreInfo));
    actions->addView(moreInfo);

    content->addView(actions);
    hero->addView(content);
    parent->addView(hero);
}

void HomeTab::rebuildHeader() {
    // clearViews() inside buildHeader() frees the old header subtree without
    // reassigning Application focus. If focus is currently inside the header
    // we're about to free, move it onto the rebuilt hero (or the rails when the
    // hero is now off) so borealis never holds a freed view.
    brls::View* focused = brls::Application::getCurrentFocus();
    bool focusInHeader = false;
    for (brls::View* v = focused; v != nullptr; v = v->getParent()) {
        if (v == m_heroContainer) { focusInHeader = true; break; }
    }

    buildHeader();

    if (focusInHeader) {
        brls::View* target = m_heroPlayButton;
        if (!target && m_scrollContent) target = m_scrollContent->getDefaultFocus();
        if (target) brls::Application::giveFocus(target);
    }
}

void HomeTab::updateHeroFromData() {
    // Only the hero region needs rebuilding, and only when the banner is on.
    if (Application::getInstance().getSettings().showFeaturedBanner)
        rebuildHeader();
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

    // Fresh image-alive token for this batch of channel logos so any in-flight
    // load from a previous build bails before touching a freed Image.
    if (m_channelImgAlive) *m_channelImgAlive = false;
    m_channelImgAlive = std::make_shared<std::atomic<bool>>(true);

    const auto& ic = platform::getImageConstraints();
    PlexClient& client = PlexClient::getInstance();

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

        // 16:9 logo tile (or colored placeholder showing the call sign).
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

        auto* logo = new brls::Image();
        logo->setPositionType(brls::PositionType::ABSOLUTE);
        logo->setPositionTop(0);
        logo->setPositionLeft(0);
        logo->setWidth((float)ic.landscapeWidth);
        logo->setHeight((float)ic.landscapeHeight);
        logo->setCornerRadius(8);
        logo->setScalingType(brls::ImageScalingType::FIT);
        logo->setVisibility(brls::Visibility::INVISIBLE);
        tile->addView(logo);
        if (!ch.thumb.empty()) {
            std::string url = client.getThumbnailUrl(ch.thumb, ic.landscapeWidth * 2, ic.landscapeHeight * 2);
            ImageLoader::loadAsync(url, [](brls::Image* img) {
                if (img) img->setVisibility(brls::Visibility::VISIBLE);
            }, logo, m_channelImgAlive);
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
        if (!ch.currentProgram.empty())
            sub += (sub.empty() ? "" : "  \xC2\xB7  ") + ch.currentProgram;
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
    if (m_orientationAlive) { *m_orientationAlive = false; }
    if (m_heroImgAlive) { *m_heroImgAlive = false; }
    if (m_channelImgAlive) { *m_channelImgAlive = false; }
}

void HomeTab::willDisappear(bool resetState) {
    brls::Box::willDisappear(resetState);
    // Invalidate alive flag so pending async callbacks bail out
    if (m_alive) *m_alive = false;
    if (m_heroImgAlive) *m_heroImgAlive = false;
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

    // Drop the untrimmed hero copy (it holds backdrop art).
    m_heroItem = MediaItem();
    m_heroResolved = false;
    m_heroFromContinue = false;

    // Mark as not loaded so data is re-fetched when returning
    m_loaded = false;
}

void HomeTab::onFocusGained() {
    brls::Box::onFocusGained();
    // Re-create alive flag so new async callbacks work (old ones still bail out)
    m_alive = std::make_shared<bool>(true);

    // Apply a Settings change to "Show featured banner" without an app restart.
    if (Application::getInstance().getSettings().showFeaturedBanner != m_lastShowFeaturedBanner)
        rebuildHeader();

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

            // Capture the hero candidate UNTRIMMED — trimForGrid() drops `art`,
            // which the hero backdrop needs. Continue Watching always wins the hero.
            MediaItem heroCandidate;
            bool hasHero = !items.empty();
            if (hasHero) heroCandidate = items.front();

            // Trim heavy fields to reduce memory
            for (auto& item : items) item.trimForGrid();

            brls::sync([this, items, heroCandidate, hasHero, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                m_continueWatching = items;
                populateRow(m_continueWatchingRow, m_continueWatching, true);

                if (hasHero) {
                    m_heroItem = heroCandidate;
                    m_heroResolved = true;
                    m_heroFromContinue = true;
                    updateHeroFromData();
                }
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

        // Capture the first movie UNTRIMMED as a hero fallback (used only when
        // there's no Continue Watching item).
        MediaItem movieHero;
        bool hasMovieHero = !movies.empty();
        if (hasMovieHero) movieHero = movies.front();

        // Trim heavy fields to reduce memory for grid display
        for (auto& item : movies) item.trimForGrid();
        for (auto& item : shows) item.trimForGrid();
        for (auto& item : music) item.trimForGrid();

        // Update UI on main thread
        brls::sync([this, movies, shows, music, movieHero, hasMovieHero, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !*alive) return;

            m_recentMovies = movies;
            m_recentShows = shows;
            m_recentMusic = music;

            populateRow(m_moviesRow, m_recentMovies);
            populateRow(m_showsRow, m_recentShows);
            populateRow(m_musicRow, m_recentMusic);

            // Hero fallback: only if Continue Watching hasn't claimed it.
            if (!m_heroFromContinue && hasMovieHero) {
                m_heroItem = movieHero;
                m_heroResolved = true;
                updateHeroFromData();
            }
        });
    });

    m_loaded = true;
    brls::Logger::debug("HomeTab: Async content loading started");
}

void HomeTab::loadRecentChannels() {
    asyncRun([this, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        brls::Logger::debug("HomeTab: Fetching live channels (async)...");
        PlexClient& client = PlexClient::getInstance();

        // Same data path the Live TV tab uses (fetchEPGGrid). A small window is
        // enough to surface the now-playing title + current-program metadata.
        std::vector<LiveTVChannel> channels;
        if (!client.fetchEPGGrid(channels, 2)) {
            brls::Logger::debug("HomeTab: no live channels (rail stays hidden)");
            return;
        }
        if (channels.size() > 10) channels.resize(10);

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
