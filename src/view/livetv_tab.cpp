/**
 * VitaPlex - Live TV Tab implementation
 *
 * Layout: On-Now hero (featured channel + current program) → full-width
 * Program Guide (the dominant block) → DVR strip. Built directly on
 * borealis boxes; no XML. Sizes derive from platform::getImageConstraints()
 * so the same code scales from Vita's 960×544 logical viewport up to a
 * 1080p TV.
 */

#include "view/livetv_tab.hpp"
#include "app/application.hpp"
#include "utils/async.hpp"
#include "utils/image_loader.hpp"
#include "utils/http_client.hpp"
#include "platform/platform.hpp"
#include <algorithm>
#include <atomic>
#include <ctime>

namespace vitaplex {

// ============================================================================
// Design tokens
// ============================================================================
// Borealis already exposes most of these via the dark theme, but the hero
// + guide redesign mixes a couple of bespoke shades (current-cell tint,
// cyan accent for the now-line) so we collect them in one place for
// consistency rather than scattering nvgRGBA literals across the build
// methods.
namespace tok {
    static inline NVGcolor accent()       { return nvgRGB(137, 241, 242); }
    static inline NVGcolor accentDeep()   { return nvgRGB(25, 138, 198); }
    static inline NVGcolor live()         { return nvgRGB(255, 86, 88); }
    static inline NVGcolor text()         { return nvgRGB(255, 255, 255); }
    static inline NVGcolor muted()        { return nvgRGB(163, 163, 163); }
    static inline NVGcolor dim()          { return nvgRGB(124, 124, 132); }
    static inline NVGcolor card()         { return nvgRGB(52, 52, 62); }
    static inline NVGcolor cardRaised()   { return nvgRGB(60, 60, 72); }
    static inline NVGcolor hairline()     { return nvgRGB(67, 67, 74); }
    static inline NVGcolor hero()         { return nvgRGB(38, 42, 48); }
    static inline NVGcolor cellUpcoming() { return nvgRGB(60, 60, 72); }
    static inline NVGcolor cellNow()      { return nvgRGB(47, 68, 82); }
    static inline NVGcolor placeholder()  { return nvgRGB(42, 42, 49); }
    static inline NVGcolor primaryInk()   { return nvgRGB(22, 32, 42); }
    static inline NVGcolor btnSecondary() { return nvgRGB(67, 67, 79); }
}

// Constants for EPG grid layout. Time-slot / row / channel-column widths are
// derived from the platform layer at runtime so larger-screen builds get wider
// cells than the Vita default.
static const int TIME_HEADER_HEIGHT = 36;

static inline int livetvTimeSlotWidth() {
    return platform::getImageConstraints().livetvChannelCardWidth;
}
static inline int livetvRowHeight() {
    // Slightly taller than the platform's listRowHeight so two-line cells
    // fit the redesign's 58px target without crowding.
    return std::max(54, platform::getImageConstraints().listRowHeight + 6);
}
static inline int livetvChannelColWidth() {
    // The redesign target is 158 on logical 1280×720; clamp to a reasonable
    // minimum on tighter Vita layouts.
    return std::max(110, platform::getImageConstraints().livetvChannelCardWidth + 38);
}

// Cache durations
static const int64_t FULL_RELOAD_INTERVAL = 300;   // 5 minutes between full EPG reloads
static const int64_t REFRESH_INTERVAL = 60;         // 1 minute between "now playing" refreshes

// EPG grid render window. We fetch m_hoursToShow hours of programming so the
// quick-access cards and the program-selected dialog can describe "next up",
// but we only build cells for the first few hours — the row containers don't
// have horizontal scroll, so anything past the viewport width is unreachable
// and pure overdraw. Four hours = eight 30-min slots ≈ on-screen on Vita.
static const int EPG_GRID_HOURS_VISIBLE = 4;

// Hero dimensions. Derived from livetvChannelRowHeight so the same scaling
// math that sizes the favourites pills also sizes the hero — bumping a
// platform's channel row up scales the hero proportionally.
static inline int heroHeight() {
    return std::max(220, platform::getImageConstraints().livetvChannelRowHeight * 2 + 40);
}
static inline int heroThumbWidth() {
    // Roughly 16:9 of the hero's inner height (height - padding).
    return std::max(240, (int)((heroHeight() - 16) * 16.0 / 9.0));
}


LiveTVTab::LiveTVTab() {
    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setGrow(1.0f);

    // Create vertical scrolling container
    m_scrollView = new brls::ScrollingFrame();
    m_scrollView->setGrow(1.0f);

    m_scrollContent = new brls::Box();
    m_scrollContent->setAxis(brls::Axis::COLUMN);
    m_scrollContent->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_scrollContent->setAlignItems(brls::AlignItems::STRETCH);
    m_scrollContent->setPadding(20);

    // Title
    m_titleLabel = new brls::Label();
    m_titleLabel->setText("Live TV");
    m_titleLabel->setFontSize(28);
    m_titleLabel->setMarginBottom(16);
    m_scrollContent->addView(m_titleLabel);

    // On-Now hero (single big focusable card at the top)
    buildHero();
    m_scrollContent->addView(m_heroBox);

    // EPG Guide — the dominant block.
    m_guideLabel = new brls::Label();
    m_guideLabel->setText("Program Guide");
    m_guideLabel->setFontSize(20);
    m_guideLabel->setMarginBottom(8);
    m_guideLabel->setMarginTop(4);
    m_scrollContent->addView(m_guideLabel);

    m_guideContainer = new brls::Box();
    m_guideContainer->setAxis(brls::Axis::COLUMN);
    // Bumped from the platform default to put ~6 channel rows on screen
    // since the hero/favourites already eat ~270px above.
    m_guideContainer->setHeight(std::max(280, platform::getImageConstraints().livetvGuideHeight + 40));
    m_guideContainer->setMarginBottom(20);
    m_guideContainer->setBackgroundColor(tok::card());
    m_guideContainer->setCornerRadius(14);
    m_guideContainer->setBorderColor(tok::hairline());
    m_guideContainer->setBorderThickness(1);

    // Time header row (horizontal scroll, offset to clear the sticky
    // channel column on the left).
    m_timeHeaderScroll = new brls::HScrollingFrame();
    m_timeHeaderScroll->setHeight(TIME_HEADER_HEIGHT);
    m_timeHeaderScroll->setMarginLeft(livetvChannelColWidth());

    m_timeHeaderBox = new brls::Box();
    m_timeHeaderBox->setAxis(brls::Axis::ROW);
    m_timeHeaderBox->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_timeHeaderBox->setBackgroundColor(nvgRGBA(0, 0, 0, 56));  // ~22% black overlay
    m_timeHeaderScroll->setContentView(m_timeHeaderBox);
    m_guideContainer->addView(m_timeHeaderScroll);

    // EPG Grid (vertical scroll containing channel rows)
    m_guideScrollV = new brls::ScrollingFrame();
    m_guideScrollV->setGrow(1.0f);

    m_guideBox = new brls::Box();
    m_guideBox->setAxis(brls::Axis::COLUMN);
    m_guideBox->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_guideBox->setAlignItems(brls::AlignItems::STRETCH);
    m_guideScrollV->setContentView(m_guideBox);
    m_guideContainer->addView(m_guideScrollV);

    // Current-time vertical line: an absolutely-positioned cyan rule
    // overlaid on the guide container. Positioned by updateCurrentTimeLine
    // once the grid is built and any time the layout refreshes.
    m_currentTimeLine = new brls::Box();
    m_currentTimeLine->setPositionType(brls::PositionType::ABSOLUTE);
    m_currentTimeLine->setBackgroundColor(tok::accent());
    m_currentTimeLine->setWidth(2);
    m_currentTimeLine->setHeight(1);  // grows in updateCurrentTimeLine
    m_currentTimeLine->setPositionTop(0);
    m_currentTimeLine->setPositionLeft(-1);
    m_currentTimeLine->setVisibility(brls::Visibility::INVISIBLE);
    m_guideContainer->addView(m_currentTimeLine);

    m_scrollContent->addView(m_guideContainer);

    // DVR Recordings strip
    m_dvrLabel = new brls::Label();
    m_dvrLabel->setText("DVR Recordings");
    m_dvrLabel->setFontSize(20);
    m_dvrLabel->setMarginBottom(8);
    m_dvrLabel->setMarginTop(4);
    m_scrollContent->addView(m_dvrLabel);

    m_dvrRow = new brls::HScrollingFrame();
    m_dvrRow->setHeight(platform::getImageConstraints().livetvChannelRowHeight);
    m_dvrRow->setMarginBottom(20);

    m_dvrContent = new brls::Box();
    m_dvrContent->setAxis(brls::Axis::ROW);
    m_dvrContent->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_dvrContent->setAlignItems(brls::AlignItems::CENTER);

    m_dvrRow->setContentView(m_dvrContent);
    m_scrollContent->addView(m_dvrRow);

    m_scrollView->setContentView(m_scrollContent);
    this->addView(m_scrollView);

    brls::Logger::debug("LiveTVTab: Loading content...");
    loadChannels();
}

LiveTVTab::~LiveTVTab() {
    if (m_alive) { *m_alive = false; }
    if (m_heroThumbAlive) { m_heroThumbAlive->store(false); }
}

void LiveTVTab::willDisappear(bool resetState) {
    brls::Box::willDisappear(resetState);
    if (m_alive) *m_alive = false;
    if (m_heroThumbAlive) m_heroThumbAlive->store(false);
    ImageLoader::cancelAll();
}

bool LiveTVTab::isDescendantOf(brls::View* view, brls::View* ancestor) {
    brls::View* current = view;
    while (current) {
        if (current == ancestor) return true;
        current = current->getParent();
    }
    return false;
}

brls::View* LiveTVTab::findFirstFocusableInBox(brls::Box* box) {
    if (!box) return nullptr;
    for (auto* child : box->getChildren()) {
        if (child->getVisibility() != brls::Visibility::VISIBLE) continue;
        if (child->isFocusable()) return child;
        brls::Box* childBox = dynamic_cast<brls::Box*>(child);
        if (childBox) {
            brls::View* found = findFirstFocusableInBox(childBox);
            if (found) return found;
        }
    }
    return nullptr;
}

brls::View* LiveTVTab::findLastFocusableInBox(brls::Box* box) {
    if (!box) return nullptr;
    auto& children = box->getChildren();
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        brls::View* child = *it;
        if (child->getVisibility() != brls::Visibility::VISIBLE) continue;
        brls::Box* childBox = dynamic_cast<brls::Box*>(child);
        if (childBox) {
            brls::View* found = findLastFocusableInBox(childBox);
            if (found) return found;
        }
        if (child->isFocusable()) return child;
    }
    return nullptr;
}

brls::View* LiveTVTab::getNextFocus(brls::FocusDirection direction, brls::View* currentView) {
    // Section layout (top to bottom):
    //   Hero (m_heroBox) -> Guide (m_guideBox) -> DVR (m_dvrRow)
    //
    // Row-to-row movement *inside* the guide is handled by borealis' natural
    // navigation (m_guideBox is a COLUMN box, so it resolves UP/DOWN between
    // rows itself). This override only kicks in when that bubbles up to the
    // tab — i.e. at a section boundary — so all we do is hop to the adjacent
    // section.
    const bool inHero  = isDescendantOf(currentView, m_heroBox);
    const bool inGuide = isDescendantOf(currentView, m_guideContainer);
    const bool inDvr   = isDescendantOf(currentView, m_dvrRow);

    if (direction == brls::FocusDirection::DOWN) {
        if (inHero) {
            if (brls::View* t = findFirstFocusableInBox(m_guideBox)) return t;
            if (brls::View* t = findFirstFocusableInBox(m_dvrContent)) return t;
        } else if (inGuide) {
            if (brls::View* t = findFirstFocusableInBox(m_dvrContent)) return t;
        }
    }
    else if (direction == brls::FocusDirection::UP) {
        if (inDvr) {
            if (brls::View* t = findLastFocusableInBox(m_guideBox)) return t;
            if (brls::View* t = findFirstFocusableInBox(m_heroBox)) return t;
        } else if (inGuide) {
            if (brls::View* t = findFirstFocusableInBox(m_heroBox)) return t;
        }
    }

    // Default behavior for left/right and unhandled cases
    return brls::Box::getNextFocus(direction, currentView);
}

void LiveTVTab::cullToViewport(brls::Box* content, brls::View* viewport, bool vertical) {
    if (!content || !viewport) return;

    const float margin = vertical ? (float)livetvRowHeight() : (float)livetvTimeSlotWidth();

    const float vpStart = vertical ? viewport->getY() : viewport->getX();
    const float vpEnd   = vpStart + (vertical ? viewport->getHeight() : viewport->getWidth());

    brls::View* focus = brls::Application::getCurrentFocus();

    for (brls::View* child : content->getChildren()) {
        const float cStart = vertical ? child->getY() : child->getX();
        const float cEnd   = cStart + (vertical ? child->getHeight() : child->getWidth());

        bool visible = (cEnd >= vpStart - margin) && (cStart <= vpEnd + margin);

        if (!visible && focus && isDescendantOf(focus, child))
            visible = true;

        const brls::Visibility want = visible ? brls::Visibility::VISIBLE
                                              : brls::Visibility::INVISIBLE;
        if (child->getVisibility() != want)
            child->setVisibility(want);
    }
}

void LiveTVTab::draw(NVGcontext* vg, float x, float y, float width, float height,
                     brls::Style style, brls::FrameContext* ctx) {
    // With 100+ channels the EPG grid holds ~100 rows and the favourites /
    // DVR rows hold ~100 cards each. borealis only culls off-screen *leaf*
    // views, never nested Boxes, so every off-screen row/card still painted
    // every frame — pure overdraw. Toggle INVISIBLE on anything scrolled
    // out of its viewport so frame() early-outs for the entire subtree.
    cullToViewport(m_guideBox, m_guideScrollV, /*vertical=*/true);
    cullToViewport(m_dvrContent, m_dvrRow, /*vertical=*/false);

    // Slide the cyan "now" line each frame so it tracks the wall clock
    // even when the guide grid itself doesn't rebuild.
    updateCurrentTimeLine();

    brls::Box::draw(vg, x, y, width, height, style, ctx);
}

void LiveTVTab::onFocusGained() {
    brls::Box::onFocusGained();
    m_alive = std::make_shared<bool>(true);

    if (!m_loaded) {
        loadChannels();
    } else {
        int64_t now = time(nullptr);
        if (now - m_lastFullLoadTime > FULL_RELOAD_INTERVAL) {
            brls::Logger::debug("LiveTVTab: Full EPG reload (stale data)");
            loadChannels();
        } else if (now - m_lastRefreshTime > REFRESH_INTERVAL) {
            brls::Logger::debug("LiveTVTab: Refreshing current programs only");
            refreshCurrentPrograms();
        }
    }
}

std::string LiveTVTab::formatTime(int64_t timestamp) {
    if (timestamp == 0) timestamp = time(nullptr);

    time_t t = (time_t)timestamp;
    struct tm* tm_info = localtime(&t);

    char buffer[16];
    strftime(buffer, sizeof(buffer), "%I:%M %p", tm_info);
    if (buffer[0] == '0') return std::string(buffer + 1);
    return std::string(buffer);
}

// ============================================================================
// Hero panel
// ============================================================================

void LiveTVTab::buildHero() {
    m_heroBox = new brls::Box();
    m_heroBox->setAxis(brls::Axis::ROW);
    m_heroBox->setHeight(heroHeight());
    m_heroBox->setMarginBottom(16);
    m_heroBox->setPadding(8);
    m_heroBox->setBackgroundColor(tok::hero());
    m_heroBox->setCornerRadius(14);
    m_heroBox->setBorderColor(tok::hairline());
    m_heroBox->setBorderThickness(1);
    // Deliberately NOT focusable: borealis' getDefaultFocus stops at the
    // first focusable ancestor, so making the whole hero focusable would
    // hide the Watch live / Record buttons inside it from the focus chain.
    // The buttons themselves are the actual focus targets.

    // Thumbnail holder. The Image itself is added invisible — we flip it
    // on once the async load finishes; the holder Box owns the slot in
    // the meantime so layout doesn't jump.
    m_heroThumbHolder = new brls::Box();
    m_heroThumbHolder->setWidth(heroThumbWidth());
    m_heroThumbHolder->setHeight(heroHeight() - 16);
    m_heroThumbHolder->setMarginRight(14);
    m_heroThumbHolder->setBackgroundColor(tok::placeholder());
    m_heroThumbHolder->setCornerRadius(10);

    m_heroThumb = new brls::Image();
    m_heroThumb->setWidth(heroThumbWidth());
    m_heroThumb->setHeight(heroHeight() - 16);
    m_heroThumb->setScalingType(brls::ImageScalingType::FIT);
    m_heroThumb->setCornerRadius(10);
    m_heroThumb->setVisibility(brls::Visibility::INVISIBLE);
    m_heroThumbHolder->addView(m_heroThumb);
    m_heroBox->addView(m_heroThumbHolder);

    // Right info column.
    auto* info = new brls::Box();
    info->setAxis(brls::Axis::COLUMN);
    info->setGrow(1.0f);
    info->setPadding(4);
    info->setJustifyContent(brls::JustifyContent::FLEX_START);

    // Row: LIVE badge + channel name + channel id.
    auto* topRow = new brls::Box();
    topRow->setAxis(brls::Axis::ROW);
    topRow->setAlignItems(brls::AlignItems::CENTER);
    topRow->setHeight(22);
    topRow->setMarginBottom(8);

    m_heroLiveBadge = new brls::Box();
    m_heroLiveBadge->setWidth(46);
    m_heroLiveBadge->setHeight(20);
    m_heroLiveBadge->setBackgroundColor(tok::live());
    m_heroLiveBadge->setCornerRadius(4);
    m_heroLiveBadge->setJustifyContent(brls::JustifyContent::CENTER);
    m_heroLiveBadge->setAlignItems(brls::AlignItems::CENTER);
    m_heroLiveBadge->setMarginRight(10);
    auto* liveTxt = new brls::Label();
    liveTxt->setText("LIVE");
    liveTxt->setFontSize(11);
    liveTxt->setTextColor(tok::text());
    m_heroLiveBadge->addView(liveTxt);
    topRow->addView(m_heroLiveBadge);

    m_heroChannelName = new brls::Label();
    m_heroChannelName->setFontSize(14);
    m_heroChannelName->setTextColor(tok::text());
    m_heroChannelName->setMarginRight(8);
    topRow->addView(m_heroChannelName);

    m_heroChannelId = new brls::Label();
    m_heroChannelId->setFontSize(12);
    m_heroChannelId->setTextColor(tok::muted());
    topRow->addView(m_heroChannelId);

    info->addView(topRow);

    // Program title.
    m_heroTitleLabel = new brls::Label();
    m_heroTitleLabel->setFontSize(22);
    m_heroTitleLabel->setTextColor(tok::text());
    m_heroTitleLabel->setMarginBottom(4);
    info->addView(m_heroTitleLabel);

    // Summary — let it use the remaining vertical space between the title
    // and the progress row so two or three lines of description fit
    // instead of getting clipped.
    m_heroSummaryLabel = new brls::Label();
    m_heroSummaryLabel->setFontSize(13);
    m_heroSummaryLabel->setTextColor(tok::muted());
    m_heroSummaryLabel->setGrow(1.0f);
    m_heroSummaryLabel->setMarginBottom(8);
    info->addView(m_heroSummaryLabel);

    // Progress row: start time · [bar] · end time · pct.
    auto* progressRow = new brls::Box();
    progressRow->setAxis(brls::Axis::ROW);
    progressRow->setAlignItems(brls::AlignItems::CENTER);
    progressRow->setHeight(18);
    progressRow->setMarginBottom(10);

    m_heroStartLabel = new brls::Label();
    m_heroStartLabel->setFontSize(11);
    m_heroStartLabel->setTextColor(tok::dim());
    m_heroStartLabel->setWidth(56);
    progressRow->addView(m_heroStartLabel);

    m_heroProgressTrack = new brls::Box();
    m_heroProgressTrack->setHeight(5);
    m_heroProgressTrack->setGrow(1.0f);
    m_heroProgressTrack->setBackgroundColor(nvgRGBA(255, 255, 255, 33));
    m_heroProgressTrack->setCornerRadius(3);
    m_heroProgressTrack->setMarginLeft(8);
    m_heroProgressTrack->setMarginRight(8);

    m_heroProgressFill = new brls::Box();
    m_heroProgressFill->setHeight(5);
    m_heroProgressFill->setWidth(0);
    m_heroProgressFill->setBackgroundColor(tok::accent());
    m_heroProgressFill->setCornerRadius(3);
    m_heroProgressTrack->addView(m_heroProgressFill);
    progressRow->addView(m_heroProgressTrack);

    m_heroEndLabel = new brls::Label();
    m_heroEndLabel->setFontSize(11);
    m_heroEndLabel->setTextColor(tok::dim());
    m_heroEndLabel->setWidth(56);
    m_heroEndLabel->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    progressRow->addView(m_heroEndLabel);

    m_heroPctLabel = new brls::Label();
    m_heroPctLabel->setFontSize(11);
    m_heroPctLabel->setTextColor(tok::accent());
    m_heroPctLabel->setWidth(40);
    m_heroPctLabel->setMarginLeft(8);
    m_heroPctLabel->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    progressRow->addView(m_heroPctLabel);

    info->addView(progressRow);

    // Button row.
    auto* btnRow = new brls::Box();
    btnRow->setAxis(brls::Axis::ROW);
    btnRow->setHeight(36);
    btnRow->setAlignItems(brls::AlignItems::CENTER);

    m_heroWatchBtn = new brls::Box();
    m_heroWatchBtn->setHeight(34);
    m_heroWatchBtn->setWidth(140);
    m_heroWatchBtn->setMarginRight(10);
    m_heroWatchBtn->setPadding(8);
    m_heroWatchBtn->setCornerRadius(8);
    m_heroWatchBtn->setBackgroundColor(tok::accent());
    m_heroWatchBtn->setJustifyContent(brls::JustifyContent::CENTER);
    m_heroWatchBtn->setAlignItems(brls::AlignItems::CENTER);
    m_heroWatchBtn->setFocusable(true);
    auto* watchTxt = new brls::Label();
    watchTxt->setText("▶  Watch live");
    watchTxt->setFontSize(14);
    watchTxt->setTextColor(tok::primaryInk());
    m_heroWatchBtn->addView(watchTxt);
    m_heroWatchBtn->registerClickAction([this](brls::View*) {
        onChannelSelected(m_heroChannel);
        return true;
    });
    btnRow->addView(m_heroWatchBtn);

    m_heroRecordBtn = new brls::Box();
    m_heroRecordBtn->setHeight(34);
    m_heroRecordBtn->setWidth(110);
    m_heroRecordBtn->setPadding(8);
    m_heroRecordBtn->setCornerRadius(8);
    m_heroRecordBtn->setBackgroundColor(tok::btnSecondary());
    m_heroRecordBtn->setJustifyContent(brls::JustifyContent::CENTER);
    m_heroRecordBtn->setAlignItems(brls::AlignItems::CENTER);
    m_heroRecordBtn->setFocusable(true);
    auto* recTxt = new brls::Label();
    recTxt->setText("●  Record");
    recTxt->setFontSize(14);
    recTxt->setTextColor(tok::text());
    m_heroRecordBtn->addView(recTxt);
    m_heroRecordBtn->registerClickAction([this](brls::View*) {
        if (m_heroProgramValid) {
            scheduleRecording(m_heroProgram, m_heroChannel);
        } else {
            brls::Dialog* dialog = new brls::Dialog(
                std::string("No current program to record on ") + m_heroChannel.title);
            dialog->addButton("OK", []() {});
            dialog->open();
        }
        return true;
    });
    btnRow->addView(m_heroRecordBtn);

    info->addView(btnRow);

    m_heroBox->addView(info);

    // Note: no card-wide click action — the box isn't focusable, so the
    // dpad lands on the Watch live / Record buttons directly. The thumbnail
    // remains a non-interactive visual.
}

void LiveTVTab::updateHeroForChannel(const LiveTVChannel& channel) {
    // Find the currently-airing program and forward to the per-program
    // updater. Falls back to the channel's legacy currentProgram/start/end
    // fields if the EPG didn't fill the programs vector.
    time_t now = time(nullptr);
    GuideProgram nowProg;
    bool found = false;
    for (const auto& p : channel.programs) {
        if (p.startTime <= (int64_t)now && p.endTime > (int64_t)now) {
            nowProg.title       = p.title;
            nowProg.summary     = p.summary;
            nowProg.startTime   = p.startTime;
            nowProg.endTime     = p.endTime;
            nowProg.ratingKey   = p.ratingKey;
            nowProg.metadataKey = p.metadataKey;
            nowProg.thumb       = p.thumb;
            found = true;
            break;
        }
    }
    if (!found && !channel.currentProgram.empty() && channel.programStart > 0) {
        nowProg.title     = channel.currentProgram;
        nowProg.startTime = channel.programStart;
        nowProg.endTime   = channel.programEnd > 0
                            ? channel.programEnd
                            : channel.programStart + 1800;
        found = true;
    }

    if (found) {
        updateHeroForProgram(channel, nowProg);
        return;
    }

    // No program data — show channel-only state.
    m_heroChannel = channel;
    m_heroProgramValid = false;
    if (m_heroChannelName)
        m_heroChannelName->setText(channel.callSign.empty() ? channel.title : channel.callSign);
    if (m_heroChannelId) {
        std::string chId = !channel.channelIdentifier.empty()
            ? "CH " + channel.channelIdentifier
            : (channel.channelNumber > 0 ? "CH " + std::to_string(channel.channelNumber) : "");
        m_heroChannelId->setText(chId);
    }
    if (m_heroTitleLabel)   m_heroTitleLabel->setText(channel.title);
    if (m_heroSummaryLabel) m_heroSummaryLabel->setText("No guide data");
    if (m_heroStartLabel)   m_heroStartLabel->setText("");
    if (m_heroEndLabel)     m_heroEndLabel->setText("");
    if (m_heroPctLabel)     m_heroPctLabel->setText("");
    if (m_heroProgressFill) m_heroProgressFill->setWidth(0);
    if (m_heroLiveBadge)    m_heroLiveBadge->setVisibility(brls::Visibility::INVISIBLE);

    // Thumb falls back to the channel's own art.
    if (m_heroThumbAlive) m_heroThumbAlive->store(false);
    m_heroThumbAlive = std::make_shared<std::atomic<bool>>(true);
    if (m_heroThumb) m_heroThumb->setVisibility(brls::Visibility::INVISIBLE);
    if (!channel.thumb.empty()) {
        PlexClient& client = PlexClient::getInstance();
        std::string url = client.getThumbnailUrl(channel.thumb, heroThumbWidth(), heroHeight() - 16);
        ImageLoader::loadAsync(url, [](brls::Image* img) {
            if (img) img->setVisibility(brls::Visibility::VISIBLE);
        }, m_heroThumb, m_heroThumbAlive);
    }
}

void LiveTVTab::updateHeroForProgram(const LiveTVChannel& channel,
                                     const GuideProgram& program) {
    m_heroChannel      = channel;
    m_heroProgram      = program;
    m_heroProgramValid = true;

    // Channel chrome.
    if (m_heroChannelName)
        m_heroChannelName->setText(channel.callSign.empty() ? channel.title : channel.callSign);
    if (m_heroChannelId) {
        std::string chId = !channel.channelIdentifier.empty()
            ? "CH " + channel.channelIdentifier
            : (channel.channelNumber > 0 ? "CH " + std::to_string(channel.channelNumber) : "");
        m_heroChannelId->setText(chId);
    }

    if (m_heroTitleLabel)   m_heroTitleLabel->setText(program.title);
    if (m_heroSummaryLabel)
        m_heroSummaryLabel->setText(program.summary.empty() ? std::string(" ") : program.summary);
    if (m_heroStartLabel)   m_heroStartLabel->setText(formatTime(program.startTime));
    if (m_heroEndLabel)     m_heroEndLabel->setText(formatTime(program.endTime));

    // Progress + LIVE badge are only meaningful when the program is
    // currently airing; future / past programs show 0% and no badge.
    time_t now = time(nullptr);
    bool isLive = (program.startTime <= (int64_t)now && program.endTime > (int64_t)now);

    int pct = 0;
    if (isLive && program.endTime > program.startTime) {
        int64_t elapsed = std::max<int64_t>(0, (int64_t)now - program.startTime);
        int64_t dur     = program.endTime - program.startTime;
        pct = (int)std::min<int64_t>(100, (elapsed * 100) / dur);
    }
    if (m_heroPctLabel) m_heroPctLabel->setText(std::to_string(pct) + "%");
    if (m_heroProgressFill && m_heroProgressTrack) {
        float trackW = m_heroProgressTrack->getWidth();
        if (trackW <= 0) trackW = 200;  // pre-layout fallback
        m_heroProgressFill->setWidth(trackW * (pct / 100.0f));
    }
    if (m_heroLiveBadge)
        m_heroLiveBadge->setVisibility(isLive ? brls::Visibility::VISIBLE
                                              : brls::Visibility::INVISIBLE);

    // Thumbnail: prefer the program's own artwork; fall back to channel
    // logo if EPG didn't carry program art for this entry.
    if (m_heroThumbAlive) m_heroThumbAlive->store(false);
    m_heroThumbAlive = std::make_shared<std::atomic<bool>>(true);
    if (m_heroThumb) m_heroThumb->setVisibility(brls::Visibility::INVISIBLE);

    std::string thumbSrc = !program.thumb.empty() ? program.thumb : channel.thumb;
    if (!thumbSrc.empty()) {
        PlexClient& client = PlexClient::getInstance();
        std::string url = client.getThumbnailUrl(thumbSrc, heroThumbWidth(), heroHeight() - 16);
        ImageLoader::loadAsync(url, [](brls::Image* img) {
            if (img) img->setVisibility(brls::Visibility::VISIBLE);
        }, m_heroThumb, m_heroThumbAlive);
    }
}

void LiveTVTab::refreshCurrentPrograms() {
    // Lightweight refresh: fetch EPG data and just update the hero's
    // "now playing" info without rebuilding the entire UI.
    asyncRun([this, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<LiveTVChannel> freshChannels;
        bool success = client.fetchEPGGrid(freshChannels, m_hoursToShow);

        if (success) {
            brls::sync([this, freshChannels, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_lastRefreshTime = time(nullptr);

                for (size_t i = 0; i < m_channels.size() && i < freshChannels.size(); i++) {
                    for (const auto& freshCh : freshChannels) {
                        if (freshCh.channelIdentifier == m_channels[i].channelIdentifier ||
                            freshCh.channelNumber == m_channels[i].channelNumber) {
                            m_channels[i].currentProgram = freshCh.currentProgram;
                            m_channels[i].programs = freshCh.programs;
                            m_channels[i].programStart = freshCh.programStart;
                            m_channels[i].programEnd = freshCh.programEnd;
                            break;
                        }
                    }
                }

                if (!m_channels.empty()) updateHeroForChannel(m_channels.front());
            });
        }
    });
}

void LiveTVTab::loadChannels() {
    asyncRun([this, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        brls::Logger::debug("LiveTVTab: Fetching EPG data (async)...");
        PlexClient& client = PlexClient::getInstance();

        std::vector<LiveTVChannel> channels;
        bool success = client.fetchEPGGrid(channels, m_hoursToShow);

        if (success) {
            brls::Logger::info("LiveTVTab: Got {} channels with EPG", channels.size());

            brls::sync([this, channels, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                m_channels = channels;

                if (!m_channels.empty()) {
                    updateHeroForChannel(m_channels.front());
                }

                buildEPGGrid();

                m_loaded = true;
                m_lastFullLoadTime = time(nullptr);
                m_lastRefreshTime = m_lastFullLoadTime;
            });
        } else {
            brls::Logger::error("LiveTVTab: Failed to fetch EPG data");
            brls::sync([this, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                m_loaded = true;
            });
        }
    });

    loadRecordings();
}

void LiveTVTab::updateCurrentTimeLine() {
    if (!m_currentTimeLine || !m_guideContainer || m_guideStartTime == 0) return;

    int gridHours = std::min(m_hoursToShow, EPG_GRID_HOURS_VISIBLE);
    int64_t guideEnd = m_guideStartTime + (gridHours * 3600);
    int64_t now = (int64_t)time(nullptr);

    if (now < m_guideStartTime || now > guideEnd) {
        if (m_currentTimeLine->getVisibility() != brls::Visibility::INVISIBLE)
            m_currentTimeLine->setVisibility(brls::Visibility::INVISIBLE);
        return;
    }

    float xOffset = (float)((now - m_guideStartTime) * livetvTimeSlotWidth() / 1800.0);
    float left = (float)livetvChannelColWidth() + xOffset;
    float top = 0;
    float height = m_guideContainer->getHeight();
    if (height <= 1) return;  // pre-layout

    m_currentTimeLine->setPositionLeft(left);
    m_currentTimeLine->setPositionTop(top);
    m_currentTimeLine->setHeight(height);
    if (m_currentTimeLine->getVisibility() != brls::Visibility::VISIBLE)
        m_currentTimeLine->setVisibility(brls::Visibility::VISIBLE);
}

void LiveTVTab::buildEPGGrid() {
    m_timeHeaderBox->clearViews();
    m_guideBox->clearViews();

    if (m_channels.empty()) {
        auto* noDataLabel = new brls::Label();
        noDataLabel->setText("No program guide data available");
        noDataLabel->setFontSize(14);
        noDataLabel->setTextColor(tok::muted());
        noDataLabel->setMarginLeft(12);
        noDataLabel->setMarginTop(12);
        m_guideBox->addView(noDataLabel);
        return;
    }

    // Set guide start time to current time rounded down to 30 minutes.
    time_t now = time(nullptr);
    m_guideStartTime = now - (now % 1800);

    int gridHours = std::min(m_hoursToShow, EPG_GRID_HOURS_VISIBLE);
    int totalSlots = gridHours * 2;

    // Time header — each 30-min slot. Bold muted labels, left hairline
    // separating slots.
    for (int i = 0; i < totalSlots; i++) {
        int64_t slotTime = m_guideStartTime + (i * 1800);

        auto* timeSlot = new brls::Box();
        timeSlot->setWidth(livetvTimeSlotWidth());
        timeSlot->setHeight(TIME_HEADER_HEIGHT);
        timeSlot->setJustifyContent(brls::JustifyContent::CENTER);
        timeSlot->setAlignItems(brls::AlignItems::CENTER);
        if (i > 0) {
            timeSlot->setBorderColor(tok::hairline());
            timeSlot->setBorderThickness(0);  // border drawn via line below
            // borealis Box draws a uniform border; we want only a left
            // hairline. Skip the border and draw a thin vertical strip
            // instead so the visual stays clean.
            auto* leftRule = new brls::Box();
            leftRule->setPositionType(brls::PositionType::ABSOLUTE);
            leftRule->setWidth(1);
            leftRule->setHeight(TIME_HEADER_HEIGHT - 12);
            leftRule->setPositionLeft(0);
            leftRule->setPositionTop(6);
            leftRule->setBackgroundColor(tok::hairline());
            timeSlot->addView(leftRule);
        }

        auto* timeLabel = new brls::Label();
        timeLabel->setText(formatTime(slotTime));
        timeLabel->setFontSize(13);
        timeLabel->setTextColor(tok::muted());
        timeSlot->addView(timeLabel);

        m_timeHeaderBox->addView(timeSlot);
    }

    // Build channel rows
    for (const auto& channel : m_channels) {
        auto* rowBox = new brls::Box();
        rowBox->setAxis(brls::Axis::ROW);
        rowBox->setHeight(livetvRowHeight());
        rowBox->setJustifyContent(brls::JustifyContent::FLEX_START);
        rowBox->setAlignItems(brls::AlignItems::CENTER);

        // Sticky channel column.
        auto* channelCol = new brls::Box();
        channelCol->setAxis(brls::Axis::COLUMN);
        channelCol->setWidth(livetvChannelColWidth());
        channelCol->setHeight(livetvRowHeight());
        channelCol->setPadding(6);
        channelCol->setBackgroundColor(nvgRGBA(0, 0, 0, 31));  // ~12% black overlay
        channelCol->setJustifyContent(brls::JustifyContent::CENTER);
        // Right hairline separating the column from the program track.
        auto* colRule = new brls::Box();
        colRule->setPositionType(brls::PositionType::ABSOLUTE);
        colRule->setWidth(1);
        colRule->setHeight(livetvRowHeight() - 6);
        colRule->setPositionLeft(livetvChannelColWidth() - 1);
        colRule->setPositionTop(3);
        colRule->setBackgroundColor(tok::hairline());
        channelCol->addView(colRule);

        auto* chNameLabel = new brls::Label();
        std::string chName = channel.callSign.empty() ? channel.title : channel.callSign;
        size_t gridChanChars = (size_t)platform::getImageConstraints().maxLiveTVChannelChars;
        if (gridChanChars > 2 && chName.length() > gridChanChars - 2) {
            chName = chName.substr(0, gridChanChars - 3) + "..";
        }
        chNameLabel->setText(chName);
        chNameLabel->setFontSize(14);
        chNameLabel->setTextColor(tok::text());
        channelCol->addView(chNameLabel);

        auto* chNumLabel = new brls::Label();
        chNumLabel->setText(!channel.channelIdentifier.empty()
                            ? channel.channelIdentifier
                            : std::to_string(channel.channelNumber));
        chNumLabel->setFontSize(11);
        chNumLabel->setTextColor(tok::dim());
        chNumLabel->setMarginTop(2);
        channelCol->addView(chNumLabel);

        LiveTVChannel capturedChannel = channel;
        channelCol->setFocusable(true);
        channelCol->registerClickAction([this, capturedChannel](brls::View*) {
            onChannelSelected(capturedChannel);
            return true;
        });
        // Hover on the channel column shows the channel's current show
        // in the hero — keeps the preview in sync as the user scrolls
        // through the channel list with the dpad.
        channelCol->getFocusEvent()->subscribe(
            [this, capturedChannel](brls::View*) {
                updateHeroForChannel(capturedChannel);
            });

        rowBox->addView(channelCol);

        // Program cells container.
        auto* programsBox = new brls::Box();
        programsBox->setAxis(brls::Axis::ROW);
        programsBox->setJustifyContent(brls::JustifyContent::FLEX_START);
        programsBox->setAlignItems(brls::AlignItems::STRETCH);
        programsBox->setGrow(1.0f);

        if (!channel.programs.empty()) {
            int64_t guideEndTime = m_guideStartTime + (gridHours * 3600);
            int64_t lastEndTime = m_guideStartTime;

            for (size_t pi = 0; pi < channel.programs.size(); pi++) {
                const auto& prog = channel.programs[pi];

                if (prog.endTime <= m_guideStartTime || prog.startTime >= guideEndTime) continue;

                int64_t visStart = std::max(prog.startTime, m_guideStartTime);
                int64_t visEnd = std::min(prog.endTime > 0 ? prog.endTime : visStart + 1800, guideEndTime);

                if (visStart > lastEndTime) {
                    int gapPixels = (int)((visStart - lastEndTime) * livetvTimeSlotWidth() / 1800);
                    if (gapPixels > 0) {
                        auto* spacer = new brls::Box();
                        spacer->setWidth(gapPixels);
                        spacer->setHeight(livetvRowHeight() - 4);
                        programsBox->addView(spacer);
                    }
                }

                int64_t durationSec = visEnd - visStart;
                int cellWidth = (int)(durationSec * livetvTimeSlotWidth() / 1800);
                if (cellWidth < 40) cellWidth = 40;

                time_t nowSec = time(nullptr);
                bool isCurrently = (prog.startTime <= (int64_t)nowSec && prog.endTime > (int64_t)nowSec);

                auto* progCell = new brls::Box();
                progCell->setAxis(brls::Axis::COLUMN);
                progCell->setWidth(cellWidth);
                progCell->setHeight(livetvRowHeight() - 6);
                progCell->setPadding(8);
                progCell->setMargins(3, 3, 3, 3);
                progCell->setBackgroundColor(isCurrently ? tok::cellNow() : tok::cellUpcoming());
                progCell->setCornerRadius(7);
                progCell->setFocusable(true);
                if (isCurrently) {
                    progCell->setBorderColor(tok::accent());
                    progCell->setBorderThickness(1);
                }

                auto* progTitle = new brls::Label();
                std::string title = prog.title;
                int maxChars = cellWidth / 8;
                if (maxChars < 4) maxChars = 4;
                if ((int)title.length() > maxChars) title = title.substr(0, maxChars - 2) + "..";
                progTitle->setText(title);
                progTitle->setFontSize(13);
                progTitle->setTextColor(tok::text());
                progCell->addView(progTitle);

                auto* timeRange = new brls::Label();
                std::string sub = formatTime(prog.startTime) + " – " + formatTime(prog.endTime);
                if (isCurrently) sub += "  ·  on now";
                timeRange->setText(sub);
                timeRange->setFontSize(10);
                timeRange->setTextColor(tok::dim());
                timeRange->setMarginTop(3);
                progCell->addView(timeRange);

                GuideProgram gp;
                gp.title = prog.title;
                gp.summary = prog.summary;
                gp.startTime = prog.startTime;
                gp.endTime = prog.endTime;
                gp.ratingKey = prog.ratingKey;
                gp.metadataKey = prog.metadataKey;
                gp.thumb = prog.thumb;

                progCell->registerClickAction([this, gp, capturedChannel](brls::View*) {
                    onProgramSelected(gp, capturedChannel);
                    return true;
                });
                // Hover → live-update the hero with this program's
                // details (title, summary, thumb, progress, channel
                // chrome). Watch live + Record buttons follow because
                // they read from m_heroChannel / m_heroProgram.
                progCell->getFocusEvent()->subscribe(
                    [this, gp, capturedChannel](brls::View*) {
                        updateHeroForProgram(capturedChannel, gp);
                    });

                programsBox->addView(progCell);
                lastEndTime = visEnd;
            }
        } else if (!channel.currentProgram.empty() && channel.programStart > 0) {
            int64_t progStart = std::max(channel.programStart, m_guideStartTime);
            int64_t guideEndTime = m_guideStartTime + (gridHours * 3600);
            int64_t progEnd = std::min(channel.programEnd > 0 ? channel.programEnd : progStart + 1800, guideEndTime);

            int startOffset = (int)((progStart - m_guideStartTime) * livetvTimeSlotWidth() / 1800);
            int cellWidth = (int)((progEnd - progStart) * livetvTimeSlotWidth() / 1800);
            if (cellWidth < 40) cellWidth = 40;

            if (startOffset > 0) {
                auto* spacer = new brls::Box();
                spacer->setWidth(startOffset);
                spacer->setHeight(livetvRowHeight() - 4);
                programsBox->addView(spacer);
            }

            auto* progCell = new brls::Box();
            progCell->setAxis(brls::Axis::COLUMN);
            progCell->setWidth(cellWidth);
            progCell->setHeight(livetvRowHeight() - 6);
            progCell->setPadding(8);
            progCell->setMargins(3, 3, 3, 3);
            progCell->setBackgroundColor(tok::cellNow());
            progCell->setCornerRadius(7);
            progCell->setBorderColor(tok::accent());
            progCell->setBorderThickness(1);
            progCell->setFocusable(true);

            auto* progTitle = new brls::Label();
            std::string title = channel.currentProgram;
            int maxChars = cellWidth / 8;
            if ((int)title.length() > maxChars) title = title.substr(0, maxChars - 2) + "..";
            progTitle->setText(title);
            progTitle->setFontSize(13);
            progTitle->setTextColor(tok::text());
            progCell->addView(progTitle);

            auto* timeRange = new brls::Label();
            timeRange->setText(formatTime(channel.programStart) + " – " + formatTime(channel.programEnd) + "  ·  on now");
            timeRange->setFontSize(10);
            timeRange->setTextColor(tok::dim());
            timeRange->setMarginTop(3);
            progCell->addView(timeRange);

            // Hover on the legacy fallback updates the hero with this
            // channel's "now playing" — we don't have a full program
            // struct here, so synthesize one from the channel fields.
            GuideProgram legacyGp;
            legacyGp.title     = channel.currentProgram;
            legacyGp.startTime = channel.programStart;
            legacyGp.endTime   = channel.programEnd > 0
                                  ? channel.programEnd
                                  : channel.programStart + 1800;
            progCell->getFocusEvent()->subscribe(
                [this, legacyGp, capturedChannel](brls::View*) {
                    updateHeroForProgram(capturedChannel, legacyGp);
                });

            programsBox->addView(progCell);
        } else {
            auto* emptyCell = new brls::Box();
            emptyCell->setWidth(livetvTimeSlotWidth() * 2);
            emptyCell->setHeight(livetvRowHeight() - 6);
            emptyCell->setMargins(3, 3, 3, 3);
            emptyCell->setBackgroundColor(tok::cardRaised());
            emptyCell->setCornerRadius(7);
            emptyCell->setFocusable(true);
            emptyCell->setPadding(8);

            auto* noInfo = new brls::Label();
            noInfo->setText("No guide data");
            noInfo->setFontSize(11);
            noInfo->setTextColor(tok::dim());
            emptyCell->addView(noInfo);

            emptyCell->registerClickAction([this, capturedChannel](brls::View*) {
                onChannelSelected(capturedChannel);
                return true;
            });
            // Hover on a no-data cell still updates the channel chrome
            // on the hero so the user knows which channel they'd tune.
            emptyCell->getFocusEvent()->subscribe(
                [this, capturedChannel](brls::View*) {
                    updateHeroForChannel(capturedChannel);
                });

            programsBox->addView(emptyCell);
        }

        rowBox->addView(programsBox);
        m_guideBox->addView(rowBox);
    }

    // Update the cyan time-line position now that the grid exists; the
    // draw() override keeps it tracking the wall clock thereafter.
    updateCurrentTimeLine();
}

void LiveTVTab::loadGuide() {
    // Already handled in loadChannels with fetchEPGGrid
}

void LiveTVTab::loadRecordings() {
    asyncRun([this, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        brls::Logger::debug("LiveTVTab: Fetching DVR recordings...");

        PlexClient& client = PlexClient::getInstance();
        HttpClient httpClient;

        std::string subsUrl = client.buildApiUrlPublic("/media/subscriptions?includeGrabs=1");
        HttpRequest req;
        req.url = subsUrl;
        req.method = "GET";
        req.headers["Accept"] = "application/json";
        req.timeout = 15;

        HttpResponse resp = httpClient.request(req);

        std::vector<DVRRecording> recordings;

        if (resp.statusCode == 200 && !resp.body.empty()) {
            brls::Logger::debug("LiveTVTab: DVR subscriptions response ({} bytes)", resp.body.length());

            size_t msPos = resp.body.find("\"MediaSubscription\"");
            if (msPos != std::string::npos) {
                size_t arrStart = resp.body.find('[', msPos);
                if (arrStart != std::string::npos) {
                    size_t pos = arrStart + 1;
                    while (pos < resp.body.length()) {
                        size_t objStart = resp.body.find('{', pos);
                        if (objStart == std::string::npos) break;

                        std::string between = resp.body.substr(pos, objStart - pos);
                        if (between.find(']') != std::string::npos) break;

                        int braceCount = 1;
                        size_t objEnd = objStart + 1;
                        while (braceCount > 0 && objEnd < resp.body.length()) {
                            if (resp.body[objEnd] == '{') braceCount++;
                            else if (resp.body[objEnd] == '}') braceCount--;
                            objEnd++;
                        }
                        std::string obj = resp.body.substr(objStart, objEnd - objStart);

                        std::string title = client.extractJsonValuePublic(obj, "title");
                        std::string key = client.extractJsonValuePublic(obj, "key");

                        if (!title.empty() && !key.empty()) {
                            DVRRecording rec;
                            rec.title = title;
                            rec.ratingKey = key;
                            rec.mediaSubscriptionId = key;

                            std::string beginsAt = client.extractJsonValuePublic(obj, "beginsAt");
                            if (!beginsAt.empty()) rec.scheduledTime = atoll(beginsAt.c_str());

                            std::string status = client.extractJsonValuePublic(obj, "status");
                            if (status == "2" || status == "recording") rec.status = "recording";
                            else rec.status = "scheduled";

                            recordings.push_back(rec);
                        }

                        pos = objEnd;
                    }
                }
            }
        }

        brls::Logger::info("LiveTVTab: Found {} DVR recordings/subscriptions", recordings.size());

        brls::sync([this, recordings, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !*alive) return;

            m_recordings = recordings;
            m_dvrContent->clearViews();

            if (m_recordings.empty()) {
                auto* placeholder = new brls::Label();
                placeholder->setText("No scheduled recordings");
                placeholder->setFontSize(13);
                placeholder->setTextColor(tok::muted());
                m_dvrContent->addView(placeholder);
                return;
            }

            const auto& dvrIc = platform::getImageConstraints();
            for (const auto& rec : m_recordings) {
                auto* card = new brls::Box();
                card->setAxis(brls::Axis::ROW);
                card->setAlignItems(brls::AlignItems::CENTER);
                card->setWidth(dvrIc.livetvChannelCardWidth + 80);
                card->setHeight(std::max(60, dvrIc.livetvChannelRowHeight - 20));
                card->setMarginRight(10);
                card->setPadding(10);
                card->setFocusable(true);
                card->setCornerRadius(11);
                card->setBackgroundColor(tok::cardRaised());
                card->setBorderColor(tok::hairline());
                card->setBorderThickness(1);

                // Status chip
                auto* chip = new brls::Box();
                chip->setWidth(28);
                chip->setHeight(28);
                chip->setCornerRadius(14);
                chip->setJustifyContent(brls::JustifyContent::CENTER);
                chip->setAlignItems(brls::AlignItems::CENTER);
                chip->setMarginRight(10);
                auto* chipDot = new brls::Label();
                if (rec.status == "recording") {
                    chip->setBackgroundColor(nvgRGBA(255, 86, 88, 64));
                    chipDot->setText("●");
                    chipDot->setTextColor(tok::live());
                } else {
                    chip->setBackgroundColor(nvgRGBA(137, 241, 242, 48));
                    chipDot->setText("⧗");  // clock-ish glyph
                    chipDot->setTextColor(tok::accent());
                }
                chipDot->setFontSize(14);
                chip->addView(chipDot);
                card->addView(chip);

                // Text block
                auto* col = new brls::Box();
                col->setAxis(brls::Axis::COLUMN);
                col->setGrow(1.0f);

                auto* titleLabel = new brls::Label();
                std::string title = rec.title;
                size_t dvrMaxChars = (size_t)(platform::getImageConstraints().maxLiveTVProgramChars + 6);
                if (dvrMaxChars > 2 && title.length() > dvrMaxChars) {
                    title = title.substr(0, dvrMaxChars - 1) + "..";
                }
                titleLabel->setText(title);
                titleLabel->setFontSize(13);
                titleLabel->setTextColor(tok::text());
                col->addView(titleLabel);

                auto* statusLabel = new brls::Label();
                std::string statusText = rec.status;
                if (rec.scheduledTime > 0) statusText += " · " + formatTime(rec.scheduledTime);
                statusLabel->setText(statusText);
                statusLabel->setFontSize(10);
                statusLabel->setTextColor(tok::dim());
                statusLabel->setMarginTop(3);
                col->addView(statusLabel);

                card->addView(col);

                DVRRecording capturedRec = rec;
                card->registerClickAction([this, capturedRec](brls::View*) {
                    brls::Dialog* dialog = new brls::Dialog(capturedRec.title);
                    dialog->addButton("Cancel Recording", [this, capturedRec]() {
                        cancelRecording(capturedRec);
                    });
                    dialog->addButton("Close", []() {});
                    dialog->open();
                    return true;
                });

                m_dvrContent->addView(card);
            }
        });
    });
}

void LiveTVTab::onChannelSelected(const LiveTVChannel& channel) {
    brls::Logger::info("LiveTVTab: Selected channel: {} ({})", channel.title, channel.channelNumber);

    std::string tuneChannel = channel.key;
    if (tuneChannel.empty()) tuneChannel = channel.channelIdentifier;
    if (tuneChannel.empty()) tuneChannel = std::to_string(channel.channelNumber);

    std::string programMetadataKey;
    time_t now = time(nullptr);
    for (const auto& prog : channel.programs) {
        if (prog.startTime <= (int64_t)now && prog.endTime > (int64_t)now && !prog.metadataKey.empty()) {
            programMetadataKey = prog.metadataKey;
            brls::Logger::info("LiveTVTab: Current program metadata key: {}", programMetadataKey);
            break;
        }
    }

    asyncRun([this, channel, tuneChannel, programMetadataKey, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        PlexClient& client = PlexClient::getInstance();
        std::string streamUrl;
        std::string liveSessionUuid;

        if (client.tuneLiveTVChannel(tuneChannel, streamUrl, liveSessionUuid, programMetadataKey)) {
            brls::Logger::info("LiveTVTab: Got stream URL for channel {}", channel.title);
            brls::sync([streamUrl, liveSessionUuid, channel]() {
                std::string title = channel.title;
                if (!channel.currentProgram.empty()) title += " - " + channel.currentProgram;
                Application::getInstance().pushLiveTVPlayerActivity(streamUrl, title, liveSessionUuid);
            });
        } else {
            brls::Logger::error("LiveTVTab: Failed to tune channel {}", channel.title);
            brls::sync([channel]() {
                brls::Dialog* dialog = new brls::Dialog("Failed to tune channel: " + channel.title);
                dialog->addButton("OK", []() {});
                dialog->open();
            });
        }
    });
}

void LiveTVTab::onProgramSelected(const GuideProgram& program, const LiveTVChannel& channel) {
    std::string message = program.title;
    if (program.startTime > 0 && program.endTime > 0) {
        message += "\n\n" + formatTime(program.startTime) + " - " + formatTime(program.endTime);
    }
    if (!program.summary.empty()) message += "\n\n" + program.summary;

    brls::Dialog* dialog = new brls::Dialog(message);
    dialog->addButton("Watch Now", [this, channel]() { onChannelSelected(channel); });
    dialog->addButton("Record", [this, program, channel]() { scheduleRecording(program, channel); });
    dialog->addButton("Cancel", []() {});
    dialog->open();
}

void LiveTVTab::scheduleRecording(const GuideProgram& program, const LiveTVChannel& channel) {
    (void)channel;

    if (program.ratingKey.empty()) {
        brls::Logger::error("LiveTVTab: scheduleRecording: missing program ratingKey");
        brls::Dialog* dialog = new brls::Dialog("Failed to schedule recording: " + program.title);
        dialog->addButton("OK", []() {});
        dialog->open();
        return;
    }

    asyncRun([this, program, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        PlexClient& client = PlexClient::getInstance();
        HttpClient httpClient;

        // GET /media/subscriptions/template?guid=<programGuid> returns the
        // pre-encoded querystring (hints[*] and params[*]) the server
        // expects, plus the recommended type and target library section.
        // We paste it verbatim and only layer recording prefs on top.
        std::string tmplUrl = client.buildApiUrlPublic(
            "/media/subscriptions/template?guid=" + program.ratingKey);

        HttpRequest tmplReq;
        tmplReq.url = tmplUrl;
        tmplReq.method = "GET";
        tmplReq.headers["Accept"] = "application/json";
        tmplReq.timeout = 15;

        brls::Logger::debug("LiveTVTab: Recording template URL: {}", tmplUrl);
        HttpResponse tmplResp = httpClient.request(tmplReq);
        if (tmplResp.statusCode != 200 || tmplResp.body.empty()) {
            brls::Logger::error("LiveTVTab: subscription template failed ({}): {}",
                                tmplResp.statusCode,
                                tmplResp.body.empty() ? "(empty)" : tmplResp.body.substr(0, 300));
            brls::sync([program]() {
                brls::Dialog* dialog = new brls::Dialog("Failed to schedule recording: " + program.title);
                dialog->addButton("OK", []() {});
                dialog->open();
            });
            return;
        }

        const std::string& body = tmplResp.body;
        size_t pickAt = std::string::npos;
        {
            size_t scan = 0;
            const std::string sel = "\"selected\":true";
            while (true) {
                size_t at = body.find(sel, scan);
                if (at == std::string::npos) break;
                int depth = 0;
                for (size_t i = at; i > 0; i--) {
                    if (body[i] == '}') depth++;
                    else if (body[i] == '{') {
                        if (depth == 0) { pickAt = i; break; }
                        depth--;
                    }
                }
                if (pickAt != std::string::npos) break;
                scan = at + sel.length();
            }
        }
        if (pickAt == std::string::npos) {
            size_t msArr = body.find("\"MediaSubscription\"");
            if (msArr != std::string::npos) pickAt = body.find('{', msArr);
        }
        if (pickAt == std::string::npos) {
            brls::Logger::error("LiveTVTab: subscription template parse failed: {}",
                                body.substr(0, 300));
            brls::sync([program]() {
                brls::Dialog* dialog = new brls::Dialog("Failed to schedule recording: " + program.title);
                dialog->addButton("OK", []() {});
                dialog->open();
            });
            return;
        }

        size_t depth = 0;
        size_t objEnd = pickAt;
        for (; objEnd < body.length(); objEnd++) {
            if (body[objEnd] == '{') depth++;
            else if (body[objEnd] == '}') {
                if (--depth == 0) { objEnd++; break; }
            }
        }
        std::string ms = body.substr(pickAt, objEnd - pickAt);

        std::string parameters    = client.extractJsonValuePublic(ms, "parameters");
        std::string typeStr       = client.extractJsonValuePublic(ms, "type");
        std::string targetSection = client.extractJsonValuePublic(ms, "targetLibrarySectionID");

        if (parameters.empty() || typeStr.empty()) {
            brls::Logger::error("LiveTVTab: template missing required fields (parameters={}, type={})",
                                parameters.empty() ? "(empty)" : "ok",
                                typeStr.empty() ? "(empty)" : typeStr);
            brls::sync([program]() {
                brls::Dialog* dialog = new brls::Dialog("Failed to schedule recording: " + program.title);
                dialog->addButton("OK", []() {});
                dialog->open();
            });
            return;
        }

        std::string post = client.buildApiUrlPublic("/media/subscriptions");
        post += "&" + parameters;
        post += "&type=" + typeStr;
        if (!targetSection.empty()) post += "&targetLibrarySectionID=" + targetSection;
        post += "&includeGrabs=1";
        post += "&prefs[oneShot]=true";
        post += "&prefs[recordPartials]=true";
        post += "&prefs[minVideoQuality]=0";
        post += "&prefs[startOffsetMinutes]=2";
        post += "&prefs[endOffsetMinutes]=2";

        HttpRequest req;
        req.url = post;
        req.method = "POST";
        req.headers["Accept"] = "application/json";
        req.timeout = 15;

        brls::Logger::debug("LiveTVTab: Recording POST URL: {}", post);
        HttpResponse resp = httpClient.request(req);

        bool success = (resp.statusCode == 200 || resp.statusCode == 201);
        std::string title = program.title;

        brls::Logger::debug("LiveTVTab: Recording response: {} ({} bytes): {}",
                            resp.statusCode, resp.body.length(),
                            resp.body.substr(0, 500));

        brls::sync([this, success, title, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !*alive) return;

            if (success) {
                brls::Dialog* dialog = new brls::Dialog("Recording scheduled: " + title);
                dialog->addButton("OK", []() {});
                dialog->open();
                loadRecordings();
            } else {
                brls::Dialog* dialog = new brls::Dialog("Failed to schedule recording: " + title);
                dialog->addButton("OK", []() {});
                dialog->open();
            }
        });
    });
}

void LiveTVTab::cancelRecording(const DVRRecording& recording) {
    asyncRun([this, recording, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        PlexClient& client = PlexClient::getInstance();
        HttpClient httpClient;

        // DELETE /media/subscriptions/{id}
        std::string delUrl = client.buildApiUrlPublic("/media/subscriptions/" + recording.mediaSubscriptionId);

        HttpRequest req;
        req.url = delUrl;
        req.method = "DELETE";
        req.headers["Accept"] = "application/json";
        req.timeout = 15;

        HttpResponse resp = httpClient.request(req);
        bool success = (resp.statusCode == 200 || resp.statusCode == 204);

        brls::sync([this, success, recording, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !*alive) return;

            if (success) {
                brls::Dialog* dialog = new brls::Dialog("Cancelled recording: " + recording.title);
                dialog->addButton("OK", []() {});
                dialog->open();
                loadRecordings();
            } else {
                brls::Dialog* dialog = new brls::Dialog("Failed to cancel recording");
                dialog->addButton("OK", []() {});
                dialog->open();
            }
        });
    });
}

} // namespace vitaplex
