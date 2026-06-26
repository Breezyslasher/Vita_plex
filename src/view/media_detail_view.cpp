/**
 * VitaPlex - Media Detail View implementation
 */

#include "view/media_detail_view.hpp"
#include "view/media_item_cell.hpp"
#include "view/recycling_grid.hpp"
#include "view/long_press_gesture.hpp"
#include "view/progress_dialog.hpp"
#include "activity/player_activity.hpp"
#include "app/application.hpp"
#include "app/downloads_manager.hpp"
#include "app/music_queue.hpp"
#include "app/hint_icons.hpp"
#include "utils/image_loader.hpp"
#include "utils/async.hpp"
#include "platform/platform.hpp"
#include <thread>

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#endif

namespace vitaplex {

MediaDetailView::MediaDetailView(const MediaItem& item)
    : m_item(item), m_alive(std::make_shared<std::atomic<bool>>(true)) {

    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setGrow(1.0f);

    // Register back button (B/Circle) to pop this activity
    this->registerAction("Back", brls::ControllerButton::BUTTON_B, [](brls::View* view) {
        brls::Application::popActivity();
        return true;
    }, false, false, brls::Sound::SOUND_BACK);

    // Movies / standalone items use the Direction-B (poster-left) two-column
    // layout, built and wired entirely in buildMovieLayout(). Everything below
    // is the layout for shows / seasons / music / etc.
    if (m_item.mediaType == MediaType::MOVIE) {
        buildMovieLayout();
        loadDetails();
        return;
    }

    // Create scrollable content
    m_scrollView = new brls::ScrollingFrame();
    m_scrollView->setGrow(1.0f);
    // CENTERED so focus can move to a view that's currently off-screen — e.g.
    // the Mark Watched button up in the header while focus is down among the
    // rows. With the default NATURAL behaviour borealis refuses to focus a view
    // that isn't already fully on-screen and just scrolls the frame instead,
    // which is why focus wouldn't hand off to an off-screen Mark Watched.
    m_scrollView->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

    m_mainContent = new brls::Box();
    m_mainContent->setAxis(brls::Axis::COLUMN);
    m_mainContent->setPadding(30);

    // Check if this is music content
    bool isMusic = (m_item.mediaType == MediaType::MUSIC_ARTIST ||
                    m_item.mediaType == MediaType::MUSIC_ALBUM ||
                    m_item.mediaType == MediaType::MUSIC_TRACK);

    // Top row - poster and info
    auto* topRow = new brls::Box();
    topRow->setAxis(brls::Axis::ROW);
    topRow->setJustifyContent(brls::JustifyContent::FLEX_START);
    topRow->setAlignItems(brls::AlignItems::FLEX_START);
    topRow->setMarginBottom(20);

    // Left side - poster
    auto* leftBox = new brls::Box();
    leftBox->setAxis(brls::Axis::COLUMN);
    leftBox->setWidth(200);
    leftBox->setMarginRight(30);

    // Wrap poster in a container for overlaying the star icon
    auto* posterContainer = new brls::Box();
    posterContainer->setAxis(brls::Axis::COLUMN);
    posterContainer->setWidth(200);

    m_posterImage = new brls::Image();
    if (isMusic) {
        // Square album art
        m_posterImage->setWidth(200);
        m_posterImage->setHeight(200);
        posterContainer->setHeight(200);
    } else {
        // Portrait poster
        m_posterImage->setWidth(200);
        m_posterImage->setHeight(300);
        posterContainer->setHeight(300);
    }
    m_posterImage->setScalingType(brls::ImageScalingType::FIT);
    m_posterImage->setVisibility(brls::Visibility::INVISIBLE);
    posterContainer->addView(m_posterImage);

    // Star/rating icon overlay in top-right corner of album art
    if (m_item.rating > 0.0f) {
        auto* starIcon = new brls::Image();
        starIcon->setImageFromRes("icons/star.png");
        starIcon->setWidth(24);
        starIcon->setHeight(24);
        starIcon->setPositionType(brls::PositionType::ABSOLUTE);
        starIcon->setPositionTop(4);
        starIcon->setPositionRight(4);
        posterContainer->addView(starIcon);
    }

    leftBox->addView(posterContainer);

    topRow->addView(leftBox);

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

    // Summary — always created, populated when fetchMediaDetails returns
    // (grid responses skip the summary field to keep payloads small, so
    // m_item.summary is almost always empty at this point — only the
    // async loadDetails fetch fills it). Starts hidden so layout doesn't
    // reserve 200 px of empty space when the server has no summary at
    // all, and the async update below flips it visible.
    m_fullDescription = m_item.summary;

    m_summaryScroll = new brls::ScrollingFrame();
    m_summaryScroll->setHeight(200);
    m_summaryScroll->setMarginBottom(20);
    // Display-only for now: non-focusable so the description never enters the
    // navigation flow or captures D-pad input. A long description is simply
    // clipped to the 200px box (no in-place scroll / popup).
    m_summaryScroll->setFocusable(false);

    m_summaryLabel = new brls::Label();
    m_summaryLabel->setFontSize(16);
    m_summaryLabel->setText(m_fullDescription);
    // The description is display-only. A focusable label renders as a single
    // marquee line and steals RIGHT/UP navigation (landing on the text
    // instead of AUDIO or the action buttons). Keeping it non-focusable lets
    // it wrap into a normal multi-line paragraph and lets navigation flow
    // straight between the action buttons and the children/tracks — the same
    // way the movie detail already behaves.
    m_summaryLabel->setFocusable(false);

    m_summaryScroll->setContentView(m_summaryLabel);
    if (m_fullDescription.empty()) {
        m_summaryScroll->setVisibility(brls::Visibility::GONE);
    }
    rightBox->addView(m_summaryScroll);

    topRow->addView(rightBox);
    m_mainContent->addView(topRow);

    // Combined scrolling container for seasons/episodes + extras
    // This keeps the header/description fixed while only the media rows scroll.
    // Movies are handled separately below (whole page scrolls).
    if (m_item.mediaType == MediaType::SHOW ||
        m_item.mediaType == MediaType::SEASON) {

        m_mediaContentScroll = new brls::ScrollingFrame();
        m_mediaContentScroll->setGrow(1.0f);
        m_mediaContentScroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

        m_mediaContentBox = new brls::Box();
        m_mediaContentBox->setAxis(brls::Axis::COLUMN);
        m_mediaContentBox->setJustifyContent(brls::JustifyContent::FLEX_START);
        m_mediaContentBox->setAlignItems(brls::AlignItems::STRETCH);

        // Children container (for shows/seasons - horizontal cards)
        if (m_item.mediaType == MediaType::SHOW ||
            m_item.mediaType == MediaType::SEASON) {

            m_childrenLabel = new brls::Label();
            if (m_item.mediaType == MediaType::SHOW) {
                m_childrenLabel->setText("Seasons");
            } else {
                m_childrenLabel->setText("Episodes");
            }
            m_childrenLabel->setFontSize(20);
            m_childrenLabel->setMarginBottom(10);
            m_mediaContentBox->addView(m_childrenLabel);

            m_childrenScroll = new brls::HScrollingFrame();
            // Episodes use landscape cells, seasons use portrait cells — pick
            // the right platform-tuned row height so covers aren't clipped on
            // non-Vita targets where cells are taller.
            {
                const auto& ic = platform::getImageConstraints();
                m_childrenScroll->setHeight(m_item.mediaType == MediaType::SEASON
                                                ? ic.landscapeRowHeight
                                                : ic.homeRowHeight);
            }
            m_childrenScroll->setMarginBottom(20);

            m_childrenBox = new brls::Box();
            m_childrenBox->setAxis(brls::Axis::ROW);
            m_childrenBox->setJustifyContent(brls::JustifyContent::FLEX_START);

            m_childrenScroll->setContentView(m_childrenBox);
            m_mediaContentBox->addView(m_childrenScroll);
        }

        // Extras container (trailers, featurettes, etc.) for shows. Movies
        // build their own extras row below (whole-page scroll, not nested).
        if (m_item.mediaType == MediaType::SHOW) {

            m_extrasLabel = new brls::Label();
            m_extrasLabel->setText("Extras");
            m_extrasLabel->setFontSize(20);
            m_extrasLabel->setMarginBottom(10);
            m_extrasLabel->setMarginTop(15);
            m_extrasLabel->setVisibility(brls::Visibility::GONE);
            m_mediaContentBox->addView(m_extrasLabel);

            m_extrasScroll = new brls::HScrollingFrame();
            m_extrasScroll->setHeight(platform::getImageConstraints().landscapeRowHeight);
            m_extrasScroll->setMarginBottom(20);
            m_extrasScroll->setVisibility(brls::Visibility::GONE);

            m_extrasBox = new brls::Box();
            m_extrasBox->setAxis(brls::Axis::ROW);
            m_extrasBox->setJustifyContent(brls::JustifyContent::FLEX_START);

            m_extrasScroll->setContentView(m_extrasBox);
            m_mediaContentBox->addView(m_extrasScroll);
        }

        // Shows also get Cast & Crew + Recommended rows (seasons keep just
        // their episode list).
        if (m_item.mediaType == MediaType::SHOW) {
            buildPeopleAndRecommendedRows(m_mediaContentBox);
        }

        m_mediaContentScroll->setContentView(m_mediaContentBox);
        m_mainContent->addView(m_mediaContentScroll);
    }

    // Track list for albums (vertical list with nested scrolling)
    if (m_item.mediaType == MediaType::MUSIC_ALBUM) {
        auto* tracksLabel = new brls::Label();
        tracksLabel->setText("Tracks");
        tracksLabel->setFontSize(20);
        tracksLabel->setMarginBottom(10);
        m_mainContent->addView(tracksLabel);

        // Wrap track list in its own ScrollingFrame so only tracks scroll
        auto* trackScroll = new brls::ScrollingFrame();
        trackScroll->setGrow(1.0f);
        trackScroll->setMinHeight(200);

        m_trackListBox = new brls::Box();
        m_trackListBox->setAxis(brls::Axis::COLUMN);
        m_trackListBox->setJustifyContent(brls::JustifyContent::FLEX_START);
        m_trackListBox->setAlignItems(brls::AlignItems::STRETCH);

        trackScroll->setContentView(m_trackListBox);
        m_mainContent->addView(trackScroll);
    }

    // Music categories container for artists - scrollable below fixed header
    if (m_item.mediaType == MediaType::MUSIC_ARTIST) {
        auto* categoriesScroll = new brls::ScrollingFrame();
        categoriesScroll->setGrow(1.0f);

        m_musicCategoriesBox = new brls::Box();
        m_musicCategoriesBox->setAxis(brls::Axis::COLUMN);

        categoriesScroll->setContentView(m_musicCategoriesBox);
        m_mainContent->addView(categoriesScroll);
    }

    if (m_item.mediaType == MediaType::MUSIC_ALBUM ||
        m_item.mediaType == MediaType::MUSIC_ARTIST ||
        m_item.mediaType == MediaType::SHOW ||
        m_item.mediaType == MediaType::SEASON) {
        // Top info is fixed, only media content below scrolls in its own container
        m_mainContent->setGrow(1.0f);
        this->addView(m_mainContent);
    } else {
        m_scrollView->setContentView(m_mainContent);
        this->addView(m_scrollView);
    }

    // Load full details
    loadDetails();
}

MediaDetailView::~MediaDetailView() {
    if (m_alive) {
        m_alive->store(false);
    }
    brls::Logger::debug("MediaDetailView: Destroyed");
}

namespace {
// Direction-B movie-detail palette (the app's neutral surfaces + gold accent).
namespace dbpal {
    inline NVGcolor surface()    { return nvgRGB(56, 56, 56); }
    inline NVGcolor surface2()   { return nvgRGB(64, 64, 64); }
    inline NVGcolor surface3()   { return nvgRGB(73, 73, 73); }
    inline NVGcolor line()       { return nvgRGB(71, 71, 71); }
    inline NVGcolor text()       { return nvgRGB(255, 255, 255); }
    inline NVGcolor muted()      { return nvgRGB(180, 180, 186); }
    inline NVGcolor summaryFg()  { return nvgRGB(220, 220, 224); }
    inline NVGcolor gold()       { return nvgRGB(229, 160, 13); }
    inline NVGcolor goldBright() { return nvgRGB(255, 194, 61); }
    inline NVGcolor goldInk()    { return nvgRGB(36, 28, 8); }
}
} // namespace

// Direction B — poster-left classic. A fixed left column (poster + audio/subs)
// and a scrolling right column (title, meta, actions, summary, cast, recommended).
// Reuses every existing widget, handler and the async loaders unchanged; this is
// purely layout + styling for the movie / standalone-item path.
void MediaDetailView::buildMovieLayout() {
    const bool portrait = platform::isPortrait();
    const auto& ic = platform::getImageConstraints();

    // ---------------- Poster ----------------
    auto* posterContainer = new brls::Box();
    posterContainer->setAxis(brls::Axis::COLUMN);
    posterContainer->setWidth(300);
    posterContainer->setHeight(450);
    posterContainer->setCornerRadius(13);
    posterContainer->setBackgroundColor(dbpal::surface3());   // placeholder until art loads
    // Poster is purely decorative — not a focus target. Play is the entry point.

    m_posterImage = new brls::Image();
    m_posterImage->setWidth(300);
    m_posterImage->setHeight(450);
    m_posterImage->setCornerRadius(13);
    m_posterImage->setScalingType(brls::ImageScalingType::FILL);
    m_posterImage->setVisibility(brls::Visibility::INVISIBLE);
    posterContainer->addView(m_posterImage);

    // ---------------- Stream selector buttons (audio / subtitles) ----------------
    // Compact secondary buttons that live on the action row beside Download. The
    // leading icon conveys the type, so the label is just the selected track
    // (filled in by updateStreamRowLabels); the full list opens in the picker.
    auto makeStreamBtn = [](const std::string& label, const std::string& iconRes,
                            brls::Image** iconOut = nullptr) -> brls::Button* {
        auto* b = new brls::Button();
        b->setText(label);
        b->setHeight(44);
        b->setPadding(0, 16, 0, 40);
        b->setCornerRadius(8);
        b->setTextColor(dbpal::text());
        b->setBackgroundColor(dbpal::surface2());
        b->setMarginRight(12);
        auto* icn = new brls::Image();
        icn->setImageFromRes(iconRes);
        icn->setWidth(20);
        icn->setHeight(20);
        icn->setScalingType(brls::ImageScalingType::FIT);
        icn->setPositionType(brls::PositionType::ABSOLUTE);
        icn->setPositionLeft(13);
        icn->setPositionTop(12);
        b->addView(icn);
        if (iconOut) *iconOut = icn;   // keep a handle so the glyph can be swapped later
        return b;
    };

    // Audio icon starts on the generic 2.0 glyph; updateStreamRowLabels swaps it
    // to the selected track's surround-sound layout when the stream list loads.
    m_audioRow = makeStreamBtn("Audio", "icons/surround-sound-2-0.png", &m_audioIcon);
    // The surround-sound glyph carries channel digits, so size it a touch larger
    // than the plain action-button icons for legibility (re-centred in the 44px button).
    if (m_audioIcon) {
        m_audioIcon->setWidth(24);
        m_audioIcon->setHeight(24);
        m_audioIcon->setPositionTop(10);
    }
    m_audioRow->setVisibility(brls::Visibility::GONE);   // revealed by loadStreams
    m_audioRow->registerClickAction([this](brls::View*) { showAudioPicker(); return true; });
    m_audioRow->addGestureRecognizer(new brls::TapGestureRecognizer(m_audioRow));

    m_subtitleRow = makeStreamBtn("Subtitles", "icons/subtitles.png");
    m_subtitleRow->setVisibility(brls::Visibility::GONE);   // revealed by loadStreams
    m_subtitleRow->registerClickAction([this](brls::View*) { showSubtitlePicker(); return true; });
    m_subtitleRow->addGestureRecognizer(new brls::TapGestureRecognizer(m_subtitleRow));

    // ---------------- Title ----------------
    m_titleLabel = new brls::Label();
    m_titleLabel->setText(m_item.title);
    m_titleLabel->setFontSize(34);
    m_titleLabel->setTextColor(dbpal::text());
    m_titleLabel->setMarginBottom(10);

    // ---------------- Metadata row ----------------
    auto* metaBox = new brls::Box();
    metaBox->setAxis(brls::Axis::ROW);
    metaBox->setAlignItems(brls::AlignItems::CENTER);
    metaBox->setMarginBottom(16);

    if (m_item.rating > 0.0f) {
        auto* starImg = new brls::Image();
        starImg->setImageFromRes("icons/mini_star.png");
        starImg->setWidth(16);
        starImg->setHeight(16);
        starImg->setMarginRight(5);
        metaBox->addView(starImg);
        auto* rl = new brls::Label();
        char rb[16];
        snprintf(rb, sizeof(rb), "%.1f", m_item.rating);
        rl->setText(rb);
        rl->setFontSize(15);
        rl->setTextColor(dbpal::goldBright());
        rl->setMarginRight(16);
        metaBox->addView(rl);
    }
    if (m_item.year > 0) {
        m_yearLabel = new brls::Label();
        m_yearLabel->setText(std::to_string(m_item.year));
        m_yearLabel->setFontSize(15);
        m_yearLabel->setTextColor(dbpal::muted());
        m_yearLabel->setMarginRight(16);
        metaBox->addView(m_yearLabel);
    }
    if (!m_item.contentRating.empty()) {
        auto* pill = new brls::Box();
        pill->setHeight(24);
        pill->setCornerRadius(5);
        pill->setPadding(0, 9, 0, 9);
        pill->setJustifyContent(brls::JustifyContent::CENTER);
        pill->setAlignItems(brls::AlignItems::CENTER);
        pill->setBackgroundColor(dbpal::surface3());
        pill->setMarginRight(16);
        m_ratingLabel = new brls::Label();
        m_ratingLabel->setText(m_item.contentRating);
        m_ratingLabel->setFontSize(13);
        m_ratingLabel->setTextColor(dbpal::muted());
        pill->addView(m_ratingLabel);
        metaBox->addView(pill);
    }
    if (m_item.duration > 0) {
        m_durationLabel = new brls::Label();
        int minutes = m_item.duration / 60000;
        m_durationLabel->setText(std::to_string(minutes) + " min");
        m_durationLabel->setFontSize(15);
        m_durationLabel->setTextColor(dbpal::muted());
        metaBox->addView(m_durationLabel);
    }

    // ---------------- Action row ----------------
    auto* actionRow = new brls::Box();
    actionRow->setAxis(brls::Axis::ROW);
    actionRow->setAlignItems(brls::AlignItems::CENTER);
    actionRow->setMarginBottom(18);

    // Play / Resume (gold fill, ink text) — Resume label when there's a saved position.
    const bool resume = m_item.viewOffset > 0;
    m_playButton = new brls::Button();
    m_playButton->setText(resume ? "Resume" : "Play");
    m_playButton->setHeight(44);
    m_playButton->setPadding(0, 22, 0, 42);
    m_playButton->setCornerRadius(8);
    m_playButton->setTextColor(dbpal::goldInk());        // label first; applyStyle() resets bg
    m_playButton->setBackgroundColor(dbpal::gold());
    m_playButton->setMarginRight(12);
    m_playButton->registerClickAction([this, resume](brls::View*) { onPlay(resume); return true; });
    m_playButton->addGestureRecognizer(new brls::TapGestureRecognizer(m_playButton));
    {
        auto* pIcon = new brls::Image();
        pIcon->setImageFromRes("icons/play.png");
        pIcon->setWidth(18);
        pIcon->setHeight(18);
        pIcon->setScalingType(brls::ImageScalingType::FIT);
        pIcon->setPositionType(brls::PositionType::ABSOLUTE);
        pIcon->setPositionLeft(15);
        pIcon->setPositionTop(13);
        m_playButton->addView(pIcon);
    }
    actionRow->addView(m_playButton);
    m_resumeButton = nullptr;  // single gold button handles both play and resume

    // Mark-watched (secondary, labelled). onToggleWatched updates both the text
    // and the check icon, so it stays a labelled button (not round) and we keep
    // m_markWatchedIcon directly so the toggle can swap filled / outline.
    m_markWatchedButton = new brls::Button();
    m_markWatchedButton->setText(m_item.watched ? "Mark Unwatched" : "Mark Watched");
    m_markWatchedButton->setHeight(44);
    m_markWatchedButton->setPadding(0, 16, 0, 40);
    m_markWatchedButton->setCornerRadius(8);
    m_markWatchedButton->setTextColor(dbpal::text());
    m_markWatchedButton->setBackgroundColor(dbpal::surface2());
    m_markWatchedButton->setMarginRight(12);
    m_markWatchedButton->registerClickAction([this](brls::View*) { onToggleWatched(); return true; });
    m_markWatchedButton->addGestureRecognizer(new brls::TapGestureRecognizer(m_markWatchedButton));
    m_markWatchedIcon = new brls::Image();
    m_markWatchedIcon->setImageFromRes(m_item.watched ? "icons/check-circle.png"
                                                       : "icons/check-circle-outline.png");
    m_markWatchedIcon->setWidth(20);
    m_markWatchedIcon->setHeight(20);
    m_markWatchedIcon->setScalingType(brls::ImageScalingType::FIT);
    m_markWatchedIcon->setPositionType(brls::PositionType::ABSOLUTE);
    m_markWatchedIcon->setPositionLeft(13);
    m_markWatchedIcon->setPositionTop(12);
    m_markWatchedButton->addView(m_markWatchedIcon);
    actionRow->addView(m_markWatchedButton);

    // Download (secondary, labelled — keeps the granular download-state text).
    m_downloadButton = new brls::Button();
    {
        DownloadItem dlCheck;
        if (DownloadsManager::getInstance().getDownloadCopy(m_item.ratingKey, dlCheck)) {
            switch (dlCheck.state) {
                case DownloadState::COMPLETED:   m_downloadButton->setText("Downloaded");      break;
                case DownloadState::TRANSCODING: m_downloadButton->setText("Transcoding...");  break;
                case DownloadState::DOWNLOADING: m_downloadButton->setText("Downloading...");  break;
                case DownloadState::QUEUED:      m_downloadButton->setText("Queued");          break;
                case DownloadState::PAUSED:      m_downloadButton->setText("Paused");          break;
                case DownloadState::FAILED:      m_downloadButton->setText("Retry Download");  break;
                default:                         m_downloadButton->setText("Download");        break;
            }
        } else {
            m_downloadButton->setText("Download");
        }
    }
    m_downloadButton->setHeight(44);
    m_downloadButton->setPadding(0, 16, 0, 40);
    m_downloadButton->setCornerRadius(8);
    m_downloadButton->setTextColor(dbpal::text());
    m_downloadButton->setBackgroundColor(dbpal::surface2());
    m_downloadButton->setMarginRight(12);
    m_downloadButton->registerClickAction([this](brls::View*) { onDownload(); return true; });
    m_downloadButton->addGestureRecognizer(new brls::TapGestureRecognizer(m_downloadButton));
    m_downloadIcon = new brls::Image();
    m_downloadIcon->setImageFromRes("icons/download.png");
    m_downloadIcon->setWidth(20);
    m_downloadIcon->setHeight(20);
    m_downloadIcon->setScalingType(brls::ImageScalingType::FIT);
    m_downloadIcon->setPositionType(brls::PositionType::ABSOLUTE);
    m_downloadIcon->setPositionLeft(13);
    m_downloadIcon->setPositionTop(12);
    m_downloadButton->addView(m_downloadIcon);
    actionRow->addView(m_downloadButton);

    // Audio + Subtitles selectors now live on the action row (the round
    // settings / More button was removed). They reveal themselves once
    // loadStreams() resolves the track list.
    actionRow->addView(m_audioRow);
    actionRow->addView(m_subtitleRow);

    // ---------------- Summary ----------------
    // The right column scrolls, so the summary is a plain wrapping paragraph (no
    // inner scroll / clip). m_summaryScroll stays null — loadDetails guards on it.
    m_summaryScroll = nullptr;
    m_fullDescription = m_item.summary;
    m_summaryLabel = new brls::Label();
    m_summaryLabel->setText(m_fullDescription);
    m_summaryLabel->setFontSize(15);
    m_summaryLabel->setTextColor(dbpal::summaryFg());
    m_summaryLabel->setWidth(portrait ? 560.0f : 620.0f);
    m_summaryLabel->setMarginBottom(22);
    m_summaryLabel->setFocusable(false);

    // ---------------- Extras / Cast / Recommended rows (populated async) ----------------
    auto sectionLabel = [](const std::string& fallback) -> brls::Label* {
        auto* l = new brls::Label();
        l->setText(fallback);
        l->setFontSize(19);
        l->setTextColor(dbpal::text());
        l->setMarginTop(6);
        l->setMarginBottom(10);
        l->setVisibility(brls::Visibility::GONE);  // load* makes it visible when populated
        return l;
    };
    auto rowScroll = [](int height) -> brls::HScrollingFrame* {
        auto* s = new brls::HScrollingFrame();
        s->setHeight(height);
        s->setMarginBottom(16);
        s->setVisibility(brls::Visibility::GONE);
        return s;
    };

    m_extrasLabel  = sectionLabel("Extras");
    m_extrasScroll = rowScroll(ic.landscapeRowHeight);
    m_extrasBox    = new brls::Box();
    m_extrasBox->setAxis(brls::Axis::ROW);
    m_extrasScroll->setContentView(m_extrasBox);

    m_peopleLabel  = sectionLabel("Cast & Crew");
    m_peopleScroll = rowScroll(ic.squareRowHeight);
    m_peopleBox    = new brls::Box();
    m_peopleBox->setAxis(brls::Axis::ROW);
    m_peopleScroll->setContentView(m_peopleBox);

    m_recommendationsLabel  = sectionLabel("More Like This");
    m_recommendationsScroll = rowScroll(ic.homeRowHeight);
    m_recommendationsBox    = new brls::Box();
    m_recommendationsBox->setAxis(brls::Axis::ROW);
    m_recommendationsScroll->setContentView(m_recommendationsBox);

    // ---------------- Assemble ----------------
    if (!portrait) {
        // Two columns: fixed left (poster) and a right column whose header
        // (title / meta / actions / summary) is fixed while only the Extras /
        // Cast & Crew / Recommended rows scroll.
        auto* leftCol = new brls::Box();
        leftCol->setAxis(brls::Axis::COLUMN);
        leftCol->setWidth(344);             // 300 poster + 44 left padding
        leftCol->setPadding(46, 0, 46, 44);
        leftCol->addView(posterContainer);   // poster only; streams moved to the action row

        // Fixed header — stays put while the rows below scroll.
        auto* rightCol = new brls::Box();
        rightCol->setAxis(brls::Axis::COLUMN);
        rightCol->setGrow(1.0f);
        rightCol->setPadding(46, 44, 46, 44);
        rightCol->addView(m_titleLabel);
        rightCol->addView(metaBox);
        rightCol->addView(actionRow);
        rightCol->addView(m_summaryLabel);

        // Only these three rows scroll (vertically). Each row is itself an
        // HScrollingFrame, so they keep scrolling horizontally within this box.
        auto* rowsBox = new brls::Box();
        rowsBox->setAxis(brls::Axis::COLUMN);
        rowsBox->addView(m_extrasLabel);
        rowsBox->addView(m_extrasScroll);
        rowsBox->addView(m_peopleLabel);
        rowsBox->addView(m_peopleScroll);
        rowsBox->addView(m_recommendationsLabel);
        rowsBox->addView(m_recommendationsScroll);

        auto* rowsScroll = new brls::ScrollingFrame();
        rowsScroll->setGrow(1.0f);            // fills the height left under the header
        rowsScroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
        rowsScroll->setContentView(rowsBox);
        rightCol->addView(rowsScroll);

        auto* rootRow = new brls::Box();
        rootRow->setAxis(brls::Axis::ROW);
        rootRow->setGrow(1.0f);
        rootRow->addView(leftCol);
        rootRow->addView(rightCol);
        rootRow->setDefaultFocusedIndex(1);   // default focus into the right column → Play
        this->addView(rootRow);
    } else {
        // Single column: poster on top, then the info block (actions carry the streams).
        auto* col = new brls::Box();
        col->setAxis(brls::Axis::COLUMN);
        col->setAlignItems(brls::AlignItems::CENTER);
        col->setPadding(28, 24, 28, 24);
        col->addView(posterContainer);

        auto* block = new brls::Box();
        block->setAxis(brls::Axis::COLUMN);
        block->setMarginTop(20);
        block->addView(m_titleLabel);
        block->addView(metaBox);
        block->addView(actionRow);   // actionRow already carries audio + subtitles
        block->addView(m_summaryLabel);
        block->addView(m_extrasLabel);
        block->addView(m_extrasScroll);
        block->addView(m_peopleLabel);
        block->addView(m_peopleScroll);
        block->addView(m_recommendationsLabel);
        block->addView(m_recommendationsScroll);
        col->addView(block);

        auto* scroll = new brls::ScrollingFrame();
        scroll->setGrow(1.0f);
        scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
        scroll->setContentView(col);
        this->addView(scroll);
    }
}

brls::HScrollingFrame* MediaDetailView::createMediaRow(const std::string& title, brls::Box** contentOut) {
    auto* label = new brls::Label();
    label->setText(title);
    label->setFontSize(20);
    label->setMarginBottom(10);
    label->setMarginTop(15);
    m_musicCategoriesBox->addView(label);

    auto* scrollFrame = new brls::HScrollingFrame();
    // Music artist detail page: "Albums", "Related Artists", etc. rows of
    // square covers. Height scales per platform so album art isn't clipped.
    scrollFrame->setHeight(platform::getImageConstraints().squareRowHeight);
    scrollFrame->setMarginBottom(10);

    auto* content = new brls::Box();
    content->setAxis(brls::Axis::ROW);
    content->setJustifyContent(brls::JustifyContent::FLEX_START);

    scrollFrame->setContentView(content);
    m_musicCategoriesBox->addView(scrollFrame);

    if (contentOut) {
        *contentOut = content;
    }

    return scrollFrame;
}

brls::View* MediaDetailView::create() {
    return nullptr; // Factory not used
}

void MediaDetailView::loadDetails() {
    std::string ratingKey = m_item.ratingKey;
    std::string thumbPath = m_item.thumb;
    MediaType mediaType = m_item.mediaType;

    // Load full details asynchronously to avoid blocking the UI thread
    asyncRun([this, ratingKey, thumbPath, mediaType]() {
        PlexClient& client = PlexClient::getInstance();

        // Fetch full details on background thread
        MediaItem fullItem;
        bool detailsLoaded = client.fetchMediaDetails(ratingKey, fullItem);

        // Update UI on main thread
        brls::sync([this, detailsLoaded, fullItem, thumbPath, mediaType]() {
            if (detailsLoaded) {
                m_item = fullItem;

                // Update UI with full details
                if (m_titleLabel && !m_item.title.empty()) {
                    m_titleLabel->setText(m_item.title);
                }

                if (m_summaryLabel && !m_item.summary.empty()) {
                    m_fullDescription = m_item.summary;
                    m_summaryLabel->setText(m_fullDescription);
                    if (m_summaryScroll) {
                        m_summaryScroll->setVisibility(brls::Visibility::VISIBLE);
                    }
                }

                // Update download button state now that we have the part path
                if (m_downloadButton && !m_item.partPath.empty()) {
                    DownloadItem dlCheck;
                    if (DownloadsManager::getInstance().getDownloadCopy(m_item.ratingKey, dlCheck)) {
                        switch (dlCheck.state) {
                            case DownloadState::COMPLETED:
                                m_downloadButton->setText("Downloaded");
                                break;
                            case DownloadState::DOWNLOADING:
                                m_downloadButton->setText("Downloading...");
                                break;
                            case DownloadState::QUEUED:
                                m_downloadButton->setText("Queued");
                                break;
                            case DownloadState::PAUSED:
                                m_downloadButton->setText("Paused");
                                break;
                            case DownloadState::FAILED:
                                m_downloadButton->setText("Retry Download");
                                break;
                            default:
                                m_downloadButton->setText("Download");
                                break;
                        }
                    } else {
                        m_downloadButton->setText("Download");
                    }
                    brls::Logger::debug("loadDetails: partPath available, download enabled");
                }
            }

            // Load thumbnail with appropriate aspect ratio
            std::string thumb = detailsLoaded ? m_item.thumb : thumbPath;
            if (m_posterImage && !thumb.empty()) {
                bool isMusic = (m_item.mediaType == MediaType::MUSIC_ARTIST ||
                                m_item.mediaType == MediaType::MUSIC_ALBUM ||
                                m_item.mediaType == MediaType::MUSIC_TRACK);

                const auto& reqIc = platform::getImageConstraints();
                int width = isMusic ? reqIc.squareRequestSize : reqIc.detailPosterRequestWidth;
                int height = isMusic ? reqIc.squareRequestSize : reqIc.detailPosterRequestHeight;

                PlexClient& client = PlexClient::getInstance();
                std::string url = client.getThumbnailUrl(thumb, width, height);
                ImageLoader::loadAsync(url, [](brls::Image* image) {
                    image->setVisibility(brls::Visibility::VISIBLE);
                }, m_posterImage, m_alive);
            }

            // Update description if full details loaded
            if (!m_item.summary.empty() && m_summaryLabel) {
                m_fullDescription = m_item.summary;
                m_summaryLabel->setText(m_fullDescription);
                if (m_summaryScroll) {
                    m_summaryScroll->setVisibility(brls::Visibility::VISIBLE);
                }
            }

            // Load children if applicable
            if (m_item.mediaType == MediaType::MUSIC_ARTIST) {
                loadMusicCategories();
            } else if (m_item.mediaType == MediaType::MUSIC_ALBUM) {
                loadTrackList();
            } else {
                loadChildren();
                // Load extras (trailers, featurettes) for movies and shows
                if (m_item.mediaType == MediaType::MOVIE ||
                    m_item.mediaType == MediaType::SHOW) {
                    loadExtras();
                }
                // Movies and shows also get cast & crew + recommended rows.
                if (m_item.mediaType == MediaType::MOVIE ||
                    m_item.mediaType == MediaType::SHOW) {
                    loadPeople();
                    loadRecommendations();
                }
            }

            // Movies: pull the audio + subtitle stream list so the
            // AUDIO / SUBTITLES rows can populate themselves and so
            // Play uses whichever tracks the user picks here.
            if (m_item.mediaType == MediaType::MOVIE) {
                loadStreams();
            }
        });
    });
}

void MediaDetailView::loadChildren() {
    if (!m_childrenBox) return;

    PlexClient& client = PlexClient::getInstance();

    if (client.fetchChildren(m_item.ratingKey, m_children)) {
        // Skip single season: if show has exactly one season and setting is enabled,
        // fetch episodes directly and display them instead of the season
        AppSettings& settings = Application::getInstance().getSettings();
        if (m_item.mediaType == MediaType::SHOW &&
            settings.skipSingleSeason &&
            m_children.size() == 1 &&
            m_children[0].mediaType == MediaType::SEASON) {

            MediaItem singleSeason = m_children[0];
            std::vector<MediaItem> episodes;
            if (client.fetchChildren(singleSeason.ratingKey, episodes) && !episodes.empty()) {
                m_children = episodes;

                // Update label and scroll height for episodes (landscape cells)
                if (m_childrenLabel) m_childrenLabel->setText("Episodes");
                if (m_childrenScroll) {
                    m_childrenScroll->setHeight(
                        platform::getImageConstraints().landscapeRowHeight);
                }

                m_childrenBox->clearViews();
                for (const auto& child : m_children) {
                    auto* cell = new MediaItemCell();
                    cell->setItem(child);
                    cell->setMarginRight(10);

                    cell->registerClickAction([child](brls::View* view) {
                        Application::getInstance().pushPlayerActivity(child.ratingKey);
                        return true;
                    });
                    cell->addGestureRecognizer(new brls::TapGestureRecognizer(cell));
                                // Register START button context menu for child items
                    MediaItem capturedChild = child;
                    if (child.mediaType == MediaType::EPISODE) {
                        cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
                            
                            [capturedChild](brls::View* view) {
                                brls::Application::notify("START pressed");
                                showEpisodeContextMenu(capturedChild);
                                return true;
                            });
                    }
                    cell->addGestureRecognizer(new LongPressGestureRecognizer(
                        cell, [capturedChild](LongPressGestureStatus status) {
                            if (status.state == brls::GestureState::START &&
                                capturedChild.mediaType == MediaType::EPISODE) {
                                showEpisodeContextMenu(capturedChild);
                            }
                            
                        }));
                    m_childrenBox->addView(cell);
                }

                // Set up focus transfer: UP from children goes to description or first child
                setupChildrenFocusTransfer();
                return;
            }
        }

        m_childrenBox->clearViews();

        for (const auto& child : m_children) {
            auto* cell = new MediaItemCell();
            cell->setItem(child);
            cell->setMarginRight(10);

            MediaItem capturedChild = child;

            if (child.mediaType == MediaType::EPISODE) {
                cell->registerClickAction([child](brls::View* view) {
                    Application::getInstance().pushPlayerActivity(child.ratingKey);
                    return true;
                });
                cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
                    [capturedChild](brls::View* view) {
                        showEpisodeContextMenu(capturedChild);
                        return true;
                    });
            } else {
                cell->registerClickAction([this, child](brls::View* view) {
                    auto* detailView = new MediaDetailView(child);
                    brls::Application::pushActivity(new brls::Activity(detailView));
                    return true;
                });
                if (child.mediaType == MediaType::SEASON) {
                    cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
                        [capturedChild](brls::View* view) {
                            showSeasonContextMenuStatic(capturedChild);
                            return true;
                        });
                }
            }
            cell->addGestureRecognizer(new brls::TapGestureRecognizer(cell));

            cell->addGestureRecognizer(new LongPressGestureRecognizer(
                cell, [capturedChild](LongPressGestureStatus status) {
                    if (status.state != brls::GestureState::START) return;
                    if (capturedChild.mediaType == MediaType::EPISODE) {
                        showEpisodeContextMenu(capturedChild);
                    } else if (capturedChild.mediaType == MediaType::SEASON) {
                        showSeasonContextMenuStatic(capturedChild);
                    }
                }));

            m_childrenBox->addView(cell);
        }

        // Set up focus transfer: UP from children goes to description or first child
        setupChildrenFocusTransfer();
    }
}

void MediaDetailView::loadExtras() {
    if (!m_extrasBox) return;

    std::string ratingKey = m_item.ratingKey;

    asyncRun([this, ratingKey]() {
        PlexClient& client = PlexClient::getInstance();

        std::vector<MediaItem> extras;
        bool ok = client.fetchExtras(ratingKey, extras);

        brls::sync([this, ok, extras]() {
            if (!m_alive || !m_alive->load()) return;
            if (!ok || extras.empty()) return;

            m_extrasBox->clearViews();

            // Show the pre-created label and scroll frame
            if (m_extrasLabel) {
                m_extrasLabel->setText("Extras (" + std::to_string(extras.size()) + ")");
                m_extrasLabel->setVisibility(brls::Visibility::VISIBLE);
            }
            if (m_extrasScroll) {
                m_extrasScroll->setVisibility(brls::Visibility::VISIBLE);
            }

            for (const auto& extra : extras) {
                auto* cell = new MediaItemCell();
                cell->setItem(extra);
                cell->setMarginRight(10);

                cell->registerClickAction([extra](brls::View* view) {
                    // Play the extra directly
                    Application::getInstance().pushPlayerActivity(extra.ratingKey);
                    return true;
                });
                cell->addGestureRecognizer(new brls::TapGestureRecognizer(cell));

                m_extrasBox->addView(cell);
            }

            brls::Logger::info("Loaded {} extras into UI", extras.size());

            // Re-run focus setup so extras get proper UP/DOWN navigation
            setupChildrenFocusTransfer();
        });
    });
}

void MediaDetailView::buildPeopleAndRecommendedRows(brls::Box* parent) {
    if (!parent) return;
    const auto& ic = platform::getImageConstraints();

    // Cast & crew
    m_peopleLabel = new brls::Label();
    m_peopleLabel->setText("Cast & Crew");
    m_peopleLabel->setFontSize(20);
    m_peopleLabel->setMarginBottom(10);
    m_peopleLabel->setMarginTop(15);
    m_peopleLabel->setVisibility(brls::Visibility::GONE);
    parent->addView(m_peopleLabel);

    m_peopleScroll = new brls::HScrollingFrame();
    m_peopleScroll->setHeight(ic.squareRowHeight);
    m_peopleScroll->setMarginBottom(20);
    m_peopleScroll->setVisibility(brls::Visibility::GONE);
    m_peopleBox = new brls::Box();
    m_peopleBox->setAxis(brls::Axis::ROW);
    m_peopleBox->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_peopleScroll->setContentView(m_peopleBox);
    parent->addView(m_peopleScroll);

    // Recommended / related
    m_recommendationsLabel = new brls::Label();
    m_recommendationsLabel->setText("Recommended");
    m_recommendationsLabel->setFontSize(20);
    m_recommendationsLabel->setMarginBottom(10);
    m_recommendationsLabel->setMarginTop(15);
    m_recommendationsLabel->setVisibility(brls::Visibility::GONE);
    parent->addView(m_recommendationsLabel);

    m_recommendationsScroll = new brls::HScrollingFrame();
    m_recommendationsScroll->setHeight(ic.homeRowHeight);
    m_recommendationsScroll->setMarginBottom(20);
    m_recommendationsScroll->setVisibility(brls::Visibility::GONE);
    m_recommendationsBox = new brls::Box();
    m_recommendationsBox->setAxis(brls::Axis::ROW);
    m_recommendationsBox->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_recommendationsScroll->setContentView(m_recommendationsBox);
    parent->addView(m_recommendationsScroll);
}

void MediaDetailView::loadPeople() {
    // Cast & crew come straight off the already-fetched detail metadata
    // (m_item.cast was populated by fetchMediaDetails), so just build the row.
    if (!m_peopleBox || !m_alive || !m_alive->load()) return;

    const std::vector<MediaItem::Person>& cast = m_item.cast;
    if (cast.empty()) return;

    m_peopleBox->clearViews();
    if (m_peopleLabel) {
        m_peopleLabel->setText("Cast & Crew");
        m_peopleLabel->setVisibility(brls::Visibility::VISIBLE);
    }
    if (m_peopleScroll) m_peopleScroll->setVisibility(brls::Visibility::VISIBLE);

    PlexClient& client = PlexClient::getInstance();

    for (const auto& person : cast) {
        // A simple non-recycled cell: headshot + name + character/job.
        auto* cell = new brls::Box();
        cell->setAxis(brls::Axis::COLUMN);
        cell->setAlignItems(brls::AlignItems::CENTER);
        cell->setJustifyContent(brls::JustifyContent::FLEX_START);
        cell->setWidth(130);
        cell->setMarginRight(12);
        cell->setPadding(5);
        // Focusable (no action) so D-pad users can move onto the cast row and
        // it scrolls into view; touch users just scroll past it.
        cell->setFocusable(true);
        cell->setCornerRadius(8);

        auto* photo = new brls::Image();
        photo->setWidth(110);
        photo->setHeight(140);
        photo->setCornerRadius(8);
        photo->setScalingType(brls::ImageScalingType::FILL);
        cell->addView(photo);

        auto* name = new brls::Label();
        name->setText(person.tag);
        name->setFontSize(14);
        name->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        name->setMarginTop(6);
        cell->addView(name);

        if (!person.role.empty()) {
            auto* role = new brls::Label();
            role->setText(person.role);
            role->setFontSize(12);
            role->setTextColor(nvgRGB(0xA8, 0xA6, 0xB4));
            role->setHorizontalAlign(brls::HorizontalAlign::CENTER);
            cell->addView(role);
        }

        if (!person.thumb.empty()) {
            std::string url = client.getThumbnailUrl(person.thumb, 220, 280);
            ImageLoader::loadAsync(url, [](brls::Image* img) {
                img->setVisibility(brls::Visibility::VISIBLE);
            }, photo, m_alive);
        }

        // Tapping a cast/crew member browses their other titles in this library.
        if (!person.filter.empty() && !m_item.librarySectionKey.empty()) {
            std::string pname  = person.tag;
            std::string sect   = m_item.librarySectionKey;
            std::string filt   = person.filter;
            std::string curKey = m_item.ratingKey;
            std::string pthumb = person.thumb;
            cell->registerClickAction([pname, sect, filt, curKey, pthumb](brls::View*) {
                showPersonResults(pname, sect, filt, curKey, pthumb);
                return true;
            });
            cell->addGestureRecognizer(new brls::TapGestureRecognizer(cell));
        }

        m_peopleBox->addView(cell);
    }

    brls::Logger::info("Loaded {} cast/crew into UI", cast.size());
}

void MediaDetailView::loadRecommendations() {
    if (!m_recommendationsBox) return;

    std::string ratingKey = m_item.ratingKey;

    asyncRun([this, ratingKey]() {
        PlexClient& client = PlexClient::getInstance();

        std::vector<MediaItem> related;
        bool ok = client.fetchRelated(ratingKey, related);

        brls::sync([this, ok, related]() {
            if (!m_alive || !m_alive->load()) return;
            if (!ok || related.empty()) return;

            m_recommendationsBox->clearViews();
            if (m_recommendationsLabel) {
                m_recommendationsLabel->setText("Recommended");
                m_recommendationsLabel->setVisibility(brls::Visibility::VISIBLE);
            }
            if (m_recommendationsScroll)
                m_recommendationsScroll->setVisibility(brls::Visibility::VISIBLE);

            for (const auto& rec : related) {
                auto* cell = new MediaItemCell();
                cell->setItem(rec);
                cell->setMarginRight(10);
                // Tapping a recommendation opens its own detail page.
                cell->registerClickAction([rec](brls::View* view) {
                    auto* detailView = new MediaDetailView(rec);
                    brls::Application::pushActivity(new brls::Activity(detailView));
                    return true;
                });
                cell->addGestureRecognizer(new brls::TapGestureRecognizer(cell));
                m_recommendationsBox->addView(cell);
            }

            brls::Logger::info("Loaded {} recommendations into UI", related.size());
            setupChildrenFocusTransfer();
        });
    });
}

namespace {

// Person-results palette (the app's neutral surfaces + gold accent). Defined
// locally because the file's other palette namespaces live further down.
namespace personui {
    inline NVGcolor bg()         { return nvgRGB(45, 45, 45); }
    inline NVGcolor surface()    { return nvgRGB(56, 56, 56); }
    inline NVGcolor surface3()   { return nvgRGB(73, 73, 73); }
    inline NVGcolor text()       { return nvgRGB(255, 255, 255); }
    inline NVGcolor muted()      { return nvgRGB(180, 180, 186); }
    inline NVGcolor dim()        { return nvgRGB(138, 138, 144); }
    inline NVGcolor gold()       { return nvgRGB(229, 160, 13); }
    inline NVGcolor goldBright() { return nvgRGB(255, 194, 61); }
    inline NVGcolor goldInk()    { return nvgRGB(36, 28, 8); }
}

// Hero banner background: a soft diagonal accent wash fading to the page bg,
// plus bottom + left scrims so the avatar / name read clearly. Children (back
// chip, avatar, labels) are drawn on top by the base Box::draw.
class PersonHeroBg : public brls::Box {
  public:
    void draw(NVGcontext* vg, float x, float y, float w, float h,
              brls::Style style, brls::FrameContext* ctx) override {
        NVGpaint wash = nvgLinearGradient(vg, x, y, x + w, y + h,
                                          nvgRGB(74, 60, 30), nvgRGB(34, 34, 34));
        nvgBeginPath(vg);
        nvgRect(vg, x, y, w, h);
        nvgFillPaint(vg, wash);
        nvgFill(vg);

        NVGpaint bottom = nvgLinearGradient(vg, x, y + h * 0.35f, x, y + h,
                                            nvgRGBA(45, 45, 45, 0), nvgRGBA(45, 45, 45, 235));
        nvgBeginPath(vg);
        nvgRect(vg, x, y, w, h);
        nvgFillPaint(vg, bottom);
        nvgFill(vg);

        NVGpaint left = nvgLinearGradient(vg, x, y, x + w * 0.6f, y,
                                          nvgRGBA(45, 45, 45, 170), nvgRGBA(45, 45, 45, 0));
        nvgBeginPath(vg);
        nvgRect(vg, x, y, w, h);
        nvgFillPaint(vg, left);
        nvgFill(vg);

        brls::Box::draw(vg, x, y, w, h, style, ctx);
    }
};

// Root container owning an alive flag, so the async avatar image load and the
// orientation listener can no-op once the screen is dismissed.
class PersonResultsScreen : public brls::Box {
  public:
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    ~PersonResultsScreen() override { if (alive) alive->store(false); }
};

// Credit kind label from a Plex section filter ("actor=123" -> "Actor").
std::string creditKindFromFilter(const std::string& filter) {
    size_t eq = filter.find('=');
    std::string t = (eq == std::string::npos) ? filter : filter.substr(0, eq);
    if (t == "actor")    return "Actor";
    if (t == "director") return "Director";
    if (t == "writer")   return "Writer";
    if (!t.empty()) { t[0] = (t[0] >= 'a' && t[0] <= 'z') ? (char)(t[0] - 32) : t[0]; return t; }
    return "";
}

std::string initialsOf(const std::string& name) {
    std::string out;
    bool prevSpace = true;
    for (char c : name) {
        if (c == ' ' || c == '\t') { prevSpace = true; continue; }
        if (prevSpace && out.size() < 2)
            out += (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        prevSpace = false;
    }
    if (out.empty() && !name.empty())
        out = std::string(1, (name[0] >= 'a' && name[0] <= 'z') ? (char)(name[0] - 32) : name[0]);
    return out;
}

} // namespace

void MediaDetailView::showPersonResults(const std::string& personName,
                                        const std::string& sectionKey,
                                        const std::string& filter,
                                        const std::string& excludeRatingKey,
                                        const std::string& personThumb) {
    if (sectionKey.empty() || filter.empty()) {
        brls::Application::notify("No filmography available");
        return;
    }
    brls::Application::notify("Finding titles for " + personName);

    const std::string creditKind = creditKindFromFilter(filter);

    // Fetch first, then build + push the populated screen. Keeping the fetch
    // ahead of the view means the only async-after-view work is the avatar load,
    // which is guarded by the screen's alive flag.
    asyncRun([personName, sectionKey, filter, excludeRatingKey, personThumb, creditKind]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;
        client.fetchByPersonFilter(sectionKey, filter, items);

        // Drop the title we came from so "other" really means other. If the
        // person is only credited on this one title, the list goes empty and we
        // just notify instead of opening a grid of the same thing.
        if (!excludeRatingKey.empty()) {
            std::vector<MediaItem> others;
            others.reserve(items.size());
            for (const auto& m : items) {
                if (m.ratingKey != excludeRatingKey) others.push_back(m);
            }
            items.swap(others);
        }

        // Decorate each result with its role-badge text. The section listing
        // rarely carries a per-title character for actors, so we prefix the
        // ones it does ("as {character}") and otherwise fall back to the
        // (constant, correct) crew role for director / writer credits.
        for (auto& m : items) {
            if (!m.character.empty())
                m.character = "as " + m.character;
            else if (creditKind == "Director" || creditKind == "Writer")
                m.character = creditKind;
        }

        int nMovies = 0, nShows = 0;
        for (const auto& m : items) {
            if (m.mediaType == MediaType::MOVIE)      nMovies++;
            else if (m.mediaType == MediaType::SHOW)  nShows++;
        }

        brls::sync([personName, personThumb, creditKind, items, nMovies, nShows]() {
            if (items.empty()) {
                brls::Application::notify("No other titles for " + personName);
                return;
            }
            const int total = (int)items.size();

            auto* root = new PersonResultsScreen();
            root->setAxis(brls::Axis::COLUMN);
            root->setGrow(1.0f);
            root->setBackgroundColor(personui::bg());
            root->registerAction("Back", brls::ControllerButton::BUTTON_B,
                [](brls::View*) { brls::Application::popActivity(); return true; },
                false, false, brls::Sound::SOUND_BACK);

            // ---------------- Hero header ----------------
            auto* hero = new PersonHeroBg();
            hero->setAxis(brls::Axis::COLUMN);
            hero->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
            hero->setPadding(18, 30, 16, 30);

            // Back chip (top-left)
            auto* backChip = new brls::Box();
            backChip->setAxis(brls::Axis::ROW);
            backChip->setAlignItems(brls::AlignItems::CENTER);
            backChip->setHeight(38);
            backChip->setCornerRadius(12);
            backChip->setPadding(0, 16, 0, 13);
            backChip->setBackgroundColor(personui::surface());
            backChip->setBorderThickness(1.0f);
            backChip->setBorderColor(nvgRGB(71, 71, 71));
            backChip->setFocusable(true);
            auto* backArrow = new brls::Label();
            backArrow->setText("‹");          // chevron, matching the reference
            backArrow->setFontSize(22);
            backArrow->setTextColor(personui::muted());
            backArrow->setMarginRight(7);
            backChip->addView(backArrow);
            auto* backTxt = new brls::Label();
            backTxt->setText("Back");
            backTxt->setFontSize(15);
            backTxt->setTextColor(personui::muted());
            backChip->addView(backTxt);
            backChip->registerClickAction(
                [](brls::View*) { brls::Application::popActivity(); return true; });
            backChip->addGestureRecognizer(new brls::TapGestureRecognizer(backChip));
            hero->addView(backChip);

            // Identity row: avatar + info (alignment/axis set by applyResponsive)
            auto* idRow = new brls::Box();

            brls::View* avatar = nullptr;
            if (!personThumb.empty()) {
                auto* img = new brls::Image();
                img->setScalingType(brls::ImageScalingType::FILL);
                std::string url =
                    PlexClient::getInstance().getThumbnailUrl(personThumb, 240, 240);
                ImageLoader::loadAsync(url, [](brls::Image* im) {
                    im->setVisibility(brls::Visibility::VISIBLE);
                }, img, root->alive);
                avatar = img;
            } else {
                auto* circle = new brls::Box();
                circle->setJustifyContent(brls::JustifyContent::CENTER);
                circle->setAlignItems(brls::AlignItems::CENTER);
                circle->setBackgroundColor(personui::surface3());
                auto* initialsLbl = new brls::Label();
                initialsLbl->setText(initialsOf(personName));
                initialsLbl->setFontSize(40);
                initialsLbl->setTextColor(personui::goldBright());
                circle->addView(initialsLbl);
                avatar = circle;
            }
            idRow->addView(avatar);

            auto* info = new brls::Box();
            info->setAxis(brls::Axis::COLUMN);

            auto* eyebrow = new brls::Label();
            eyebrow->setText("APPEARS IN YOUR LIBRARY");
            eyebrow->setFontSize(12);
            eyebrow->setTextColor(personui::goldBright());
            info->addView(eyebrow);

            auto* nameLbl = new brls::Label();
            nameLbl->setText(personName);
            nameLbl->setTextColor(personui::text());
            nameLbl->setMarginTop(2);
            info->addView(nameLbl);

            std::string sub = creditKind;
            if (!sub.empty()) sub += "   ·   ";
            sub += std::to_string(total) + (total == 1 ? " title" : " titles");
            auto* subLbl = new brls::Label();
            subLbl->setText(sub);
            subLbl->setFontSize(14);
            subLbl->setTextColor(personui::muted());
            subLbl->setMarginTop(6);
            info->addView(subLbl);

            idRow->addView(info);
            hero->addView(idRow);
            root->addView(hero);

            // Responsive hero layout (applied now + on orientation change).
            auto applyResponsive = [hero, idRow, avatar, info, nameLbl](bool p) {
                // Portrait stacks avatar over the info column, so it needs more
                // height than the side-by-side landscape hero.
                hero->setHeight(p ? 300.0f : 200.0f);
                idRow->setAxis(p ? brls::Axis::COLUMN : brls::Axis::ROW);
                idRow->setAlignItems(p ? brls::AlignItems::CENTER : brls::AlignItems::FLEX_END);
                avatar->setWidth(p ? 96.0f : 120.0f);
                avatar->setHeight(p ? 96.0f : 120.0f);
                avatar->setCornerRadius(p ? 48.0f : 60.0f);
                info->setMarginLeft(p ? 0.0f : 24.0f);
                info->setMarginTop(p ? 10.0f : 0.0f);
                info->setAlignItems(p ? brls::AlignItems::CENTER : brls::AlignItems::FLEX_START);
                nameLbl->setFontSize(p ? 30.0f : 38.0f);
            };
            applyResponsive(platform::isPortrait());

            std::weak_ptr<std::atomic<bool>> aliveWeak = root->alive;
            platform::onOrientationChanged([aliveWeak, applyResponsive]() {
                auto a = aliveWeak.lock();
                if (!a || !a->load()) return;
                applyResponsive(platform::isPortrait());
            });

            // ---------------- Body: section row + poster grid ----------------
            auto* body = new brls::Box();
            body->setAxis(brls::Axis::COLUMN);
            body->setGrow(1.0f);
            body->setPadding(18, 30, 0, 30);

            auto* sectionRow = new brls::Box();
            sectionRow->setAxis(brls::Axis::ROW);
            sectionRow->setAlignItems(brls::AlignItems::CENTER);
            sectionRow->setMarginBottom(14);

            auto* sectionTitle = new brls::Label();
            sectionTitle->setText((nMovies > 0 && nShows > 0) ? "Movies & Shows"
                                  : (nShows > 0 ? "Shows" : "Movies"));
            sectionTitle->setFontSize(18);
            sectionTitle->setTextColor(personui::text());
            sectionRow->addView(sectionTitle);

            auto* sectionCount = new brls::Label();
            sectionCount->setText("   " + std::to_string(total));
            sectionCount->setFontSize(18);
            sectionCount->setTextColor(personui::dim());
            sectionRow->addView(sectionCount);

            auto* spacer = new brls::Box();
            spacer->setGrow(1.0f);
            sectionRow->addView(spacer);

            auto* grid = new RecyclingGrid();
            grid->setGrow(1.0f);
            grid->setOnItemSelected([](const MediaItem& sel) {
                auto* detailView = new MediaDetailView(sel);
                brls::Application::pushActivity(new brls::Activity(detailView));
            });

            // Filter chips only make sense when the results mix movies + shows.
            if (nMovies > 0 && nShows > 0) {
                auto* chipRow = new brls::Box();
                chipRow->setAxis(brls::Axis::ROW);
                chipRow->setAlignItems(brls::AlignItems::CENTER);

                auto chipBoxes  = std::make_shared<std::vector<brls::Box*>>();
                auto chipLabels = std::make_shared<std::vector<brls::Label*>>();
                auto chipTypes  = std::make_shared<std::vector<int>>();

                auto makeChip = [&](const std::string& label, int count, int type) {
                    auto* chip = new brls::Box();
                    chip->setAxis(brls::Axis::ROW);
                    chip->setAlignItems(brls::AlignItems::CENTER);
                    chip->setHeight(34);
                    chip->setCornerRadius(17);
                    chip->setPadding(0, 14, 0, 14);
                    chip->setMarginLeft(8);
                    chip->setFocusable(true);
                    chip->setBackgroundColor(personui::surface3());
                    auto* lbl = new brls::Label();
                    lbl->setText(label + "  " + std::to_string(count));
                    lbl->setFontSize(14);
                    lbl->setTextColor(personui::muted());
                    chip->addView(lbl);
                    chipRow->addView(chip);
                    chipBoxes->push_back(chip);
                    chipLabels->push_back(lbl);
                    chipTypes->push_back(type);
                };
                makeChip("All", total, 0);
                makeChip("Movies", nMovies, 1);
                makeChip("Shows", nShows, 2);

                // Narrow the already-fetched list (no new request) + restyle chips:
                // gold fill + ink text on the active one, neutral on the rest.
                auto applyFilter = [grid, items, chipBoxes, chipLabels, chipTypes](int type) {
                    for (size_t i = 0; i < chipBoxes->size(); i++) {
                        bool active = ((*chipTypes)[i] == type);
                        (*chipBoxes)[i]->setBackgroundColor(active ? personui::gold()
                                                                   : personui::surface3());
                        (*chipLabels)[i]->setTextColor(active ? personui::goldInk()
                                                              : personui::muted());
                    }
                    if (type == 0) { grid->setDataSource(items); return; }
                    std::vector<MediaItem> filtered;
                    for (const auto& m : items) {
                        if ((type == 1 && m.mediaType == MediaType::MOVIE) ||
                            (type == 2 && m.mediaType == MediaType::SHOW))
                            filtered.push_back(m);
                    }
                    grid->setDataSource(filtered);
                };

                for (size_t i = 0; i < chipBoxes->size(); i++) {
                    int t = (*chipTypes)[i];
                    brls::Box* cb = (*chipBoxes)[i];
                    cb->registerClickAction(
                        [applyFilter, t](brls::View*) { applyFilter(t); return true; });
                    cb->addGestureRecognizer(new brls::TapGestureRecognizer(cb));
                }

                sectionRow->addView(chipRow);
                applyFilter(0);  // default to "All" (active chip + full grid)
            } else {
                grid->setDataSource(items);
            }

            body->addView(sectionRow);
            body->addView(grid);
            root->addView(body);

            brls::Application::pushActivity(new brls::Activity(root));
            brls::Application::giveFocus(grid);
        });
    });
}

void MediaDetailView::loadMusicCategories() {
    if (!m_musicCategoriesBox) return;

    asyncRun([this]() {
        PlexClient& client = PlexClient::getInstance();

        // Fetch music videos (extras) for this artist
        std::vector<MediaItem> musicVideos;
        std::vector<MediaItem> allExtras;
        if (client.fetchExtras(m_item.ratingKey, allExtras)) {
            for (const auto& extra : allExtras) {
                // Music videos are clips with subtype "musicVideo" or just clips from artists
                std::string subtype = extra.subtype;
                for (char& c : subtype) c = tolower(c);
                if (extra.mediaType == MediaType::CLIP || subtype == "musicvideo") {
                    musicVideos.push_back(extra);
                }
            }
            brls::Logger::info("Artist: Found {} music videos from {} extras", musicVideos.size(), allExtras.size());
        }

        // Use the hubs API which returns albums pre-grouped by type
        // (Albums, Singles & EPs, Compilations, Appears On, etc.)
        std::vector<Hub> hubs;
        bool useHubs = client.fetchArtistHubs(m_item.ratingKey, hubs);

        if (useHubs && !hubs.empty()) {
            brls::Logger::info("Artist hubs: {} categories", hubs.size());

            // Filter hubs to only album hubs (skip track hubs like "Most Popular Tracks")
            std::vector<Hub> albumHubs;
            for (const auto& hub : hubs) {
                bool hasAlbums = false;
                for (const auto& item : hub.items) {
                    if (item.mediaType == MediaType::MUSIC_ALBUM) {
                        hasAlbums = true;
                        break;
                    }
                }
                if (hasAlbums) {
                    albumHubs.push_back(hub);
                }
            }

            brls::Logger::info("Artist hubs: {} album categories from {} total hubs", albumHubs.size(), hubs.size());

            // Collect all album items from all album hubs, then group by subtype
            std::vector<MediaItem> allAlbumItems;
            for (const auto& hub : albumHubs) {
                for (const auto& item : hub.items) {
                    if (item.mediaType == MediaType::MUSIC_ALBUM) {
                        allAlbumItems.push_back(item);
                    }
                }
            }

            brls::sync([this, allAlbumItems, musicVideos]() {
                m_musicCategoriesBox->clearViews();

                // Group albums by subtype
                std::vector<MediaItem> albums, singles, eps, compilations, soundtracks, other;
                for (const auto& item : allAlbumItems) {
                    std::string subtype = item.subtype;
                    for (char& c : subtype) c = tolower(c);

                    if (subtype == "single") {
                        singles.push_back(item);
                    } else if (subtype == "ep") {
                        eps.push_back(item);
                    } else if (subtype == "compilation") {
                        compilations.push_back(item);
                    } else if (subtype == "soundtrack") {
                        soundtracks.push_back(item);
                    } else if (subtype == "album" || subtype.empty()) {
                        albums.push_back(item);
                    } else {
                        other.push_back(item);
                    }
                }

                auto addCategory = [this](const std::string& title, const std::vector<MediaItem>& items) {
                    if (items.empty()) return;

                    brls::Box* content = nullptr;
                    createMediaRow(title + " (" + std::to_string(items.size()) + ")", &content);

                    for (const auto& item : items) {
                        auto* cell = new MediaItemCell();
                        cell->setItem(item);
                        cell->setMarginRight(10);

                        MediaItem capturedItem = item;
                        cell->registerClickAction([this, capturedItem](brls::View* view) {
                            auto* detailView = new MediaDetailView(capturedItem);
                            brls::Application::pushActivity(new brls::Activity(detailView));
                            return true;
                        });
                        cell->addGestureRecognizer(new brls::TapGestureRecognizer(cell));

                        cell->registerAction("Options", brls::ControllerButton::BUTTON_START, [this, capturedItem](brls::View* view) {
                            showAlbumContextMenu(capturedItem);
                            return true;
                        });
                        cell->addGestureRecognizer(new LongPressGestureRecognizer(
                            cell, [this, capturedItem](LongPressGestureStatus status) {
                                if (status.state == brls::GestureState::START) {
                                    showAlbumContextMenu(capturedItem);
                                }
                            }));

                        content->addView(cell);
                    }
                };

                addCategory("Albums", albums);
                addCategory("Singles", singles);
                addCategory("EPs", eps);
                addCategory("Compilations", compilations);
                addCategory("Soundtracks", soundtracks);
                addCategory("Other", other);

                // Add music videos row
                if (!musicVideos.empty()) {
                    brls::Box* mvContent = nullptr;
                    createMediaRow("Music Videos (" + std::to_string(musicVideos.size()) + ")", &mvContent);

                    for (const auto& mv : musicVideos) {
                        auto* cell = new MediaItemCell();
                        cell->setItem(mv);
                        cell->setMarginRight(10);

                        MediaItem capturedMv = mv;
                        cell->registerClickAction([capturedMv](brls::View* view) {
                            Application::getInstance().pushPlayerActivity(capturedMv.ratingKey);
                            return true;
                        });
                        cell->addGestureRecognizer(new brls::TapGestureRecognizer(cell));

                        mvContent->addView(cell);
                    }
                }

                // Land focus on the first category row once it's loaded. The
                // description is display-only (non-focusable), so it can't
                // hold focus — without this, focus would be stranded.
                if (m_musicCategoriesBox && !m_musicCategoriesBox->getChildren().empty()) {
                    // Find first focusable view in the categories
                    for (auto* child : m_musicCategoriesBox->getChildren()) {
                        auto* hScroll = dynamic_cast<brls::HScrollingFrame*>(child);
                        if (hScroll) {
                            brls::Application::giveFocus(hScroll);
                            break;
                        }
                    }
                }
            });
            return;
        }

        // Fallback: use children endpoint and group by subtype
        brls::Logger::info("Artist hubs unavailable, falling back to children grouping");

        std::vector<MediaItem> allAlbums;
        if (!client.fetchChildren(m_item.ratingKey, allAlbums)) {
            brls::Logger::error("Failed to fetch albums for artist");
            return;
        }

        std::vector<MediaItem> albums;
        std::vector<MediaItem> singles;
        std::vector<MediaItem> eps;
        std::vector<MediaItem> compilations;
        std::vector<MediaItem> soundtracks;
        std::vector<MediaItem> other;

        for (const auto& album : allAlbums) {
            std::string subtype = album.subtype;
            for (char& c : subtype) c = tolower(c);

            if (subtype == "single") {
                singles.push_back(album);
            } else if (subtype == "ep") {
                eps.push_back(album);
            } else if (subtype == "compilation") {
                compilations.push_back(album);
            } else if (subtype == "soundtrack") {
                soundtracks.push_back(album);
            } else if (subtype == "album" || subtype.empty()) {
                albums.push_back(album);
            } else {
                other.push_back(album);
            }
        }

        brls::sync([this, albums, singles, eps, compilations, soundtracks, other, musicVideos]() {
            m_musicCategoriesBox->clearViews();

            auto addCategory = [this](const std::string& title, const std::vector<MediaItem>& items) {
                if (items.empty()) return;

                brls::Box* content = nullptr;
                createMediaRow(title + " (" + std::to_string(items.size()) + ")", &content);

                for (const auto& item : items) {
                    auto* cell = new MediaItemCell();
                    cell->setItem(item);
                    cell->setMarginRight(10);

                    MediaItem capturedItem = item;
                    cell->registerClickAction([this, capturedItem](brls::View* view) {
                        auto* detailView = new MediaDetailView(capturedItem);
                        brls::Application::pushActivity(new brls::Activity(detailView));
                        return true;
                    });
                    cell->addGestureRecognizer(new brls::TapGestureRecognizer(cell));

                    cell->registerAction("Options", brls::ControllerButton::BUTTON_START, [this, capturedItem](brls::View* view) {
                        showAlbumContextMenu(capturedItem);
                        return true;
                    });
                    cell->addGestureRecognizer(new LongPressGestureRecognizer(
                        cell, [this, capturedItem](LongPressGestureStatus status) {
                            if (status.state == brls::GestureState::START) {
                                showAlbumContextMenu(capturedItem);
                            }
                        }));

                    content->addView(cell);
                }
            };

            addCategory("Albums", albums);
            addCategory("Singles", singles);
            addCategory("EPs", eps);
            addCategory("Compilations", compilations);
            addCategory("Soundtracks", soundtracks);
            addCategory("Other", other);

            // Add music videos row
            if (!musicVideos.empty()) {
                brls::Box* mvContent = nullptr;
                createMediaRow("Music Videos (" + std::to_string(musicVideos.size()) + ")", &mvContent);

                for (const auto& mv : musicVideos) {
                    auto* cell = new MediaItemCell();
                    cell->setItem(mv);
                    cell->setMarginRight(10);

                    MediaItem capturedMv = mv;
                    cell->registerClickAction([capturedMv](brls::View* view) {
                        Application::getInstance().pushPlayerActivity(capturedMv.ratingKey);
                        return true;
                    });
                    cell->addGestureRecognizer(new brls::TapGestureRecognizer(cell));

                    mvContent->addView(cell);
                }
            }

            // Land focus on the first category row once it's loaded. The
            // description is display-only (non-focusable), so it can't hold
            // focus — without this, focus would be stranded.
            if (m_musicCategoriesBox && !m_musicCategoriesBox->getChildren().empty()) {
                for (auto* child : m_musicCategoriesBox->getChildren()) {
                    auto* hScroll = dynamic_cast<brls::HScrollingFrame*>(child);
                    if (hScroll) {
                        brls::Application::giveFocus(hScroll);
                        break;
                    }
                }
            }
        });
    });
}

void MediaDetailView::onPlay(bool resume) {
    // Start playback
    if (m_item.mediaType == MediaType::MOVIE ||
        m_item.mediaType == MediaType::EPISODE ||
        m_item.mediaType == MediaType::MUSIC_TRACK) {

        // Check if this item is downloaded locally - play from local file if available
        DownloadItem dlItem;
        bool isLocal = DownloadsManager::getInstance().getDownloadCopy(m_item.ratingKey, dlItem)
                       && dlItem.state == DownloadState::COMPLETED;
        Application::getInstance().pushPlayerActivity(m_item.ratingKey, isLocal);
    }
    // For shows/seasons/albums, play the first child item
    else if (m_item.mediaType == MediaType::SHOW ||
             m_item.mediaType == MediaType::SEASON ||
             m_item.mediaType == MediaType::MUSIC_ALBUM) {

        // If we already have children loaded, play the first one
        if (!m_children.empty()) {
            // For shows, the first child is a season - need to get its first episode
            if (m_item.mediaType == MediaType::SHOW) {
                PlexClient& client = PlexClient::getInstance();
                std::vector<MediaItem> episodes;
                if (client.fetchChildren(m_children[0].ratingKey, episodes) && !episodes.empty()) {
                    Application::getInstance().pushPlayerActivity(episodes[0].ratingKey);
                }
            } else {
                // For seasons/albums, first child is directly playable
                Application::getInstance().pushPlayerActivity(m_children[0].ratingKey);
            }
        } else {
            // Fetch children and play first one
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> children;
            if (client.fetchChildren(m_item.ratingKey, children) && !children.empty()) {
                if (m_item.mediaType == MediaType::SHOW) {
                    // First child is a season, get its episodes
                    std::vector<MediaItem> episodes;
                    if (client.fetchChildren(children[0].ratingKey, episodes) && !episodes.empty()) {
                        Application::getInstance().pushPlayerActivity(episodes[0].ratingKey);
                    }
                } else {
                    Application::getInstance().pushPlayerActivity(children[0].ratingKey);
                }
            }
        }
    }
}

void MediaDetailView::onDownload() {
    // Check if already in download queue
    DownloadItem existingDl;
    if (DownloadsManager::getInstance().getDownloadCopy(m_item.ratingKey, existingDl)) {
        switch (existingDl.state) {
            case DownloadState::COMPLETED:
                brls::Application::notify("Already downloaded");
                return;
            case DownloadState::DOWNLOADING:
                brls::Application::notify("Already downloading");
                return;
            case DownloadState::QUEUED:
                brls::Application::notify("Already queued for download");
                return;
            case DownloadState::PAUSED:
                // Resume paused downloads
                brls::Application::notify("Resuming download...");
                DownloadsManager::getInstance().resumeIncompleteDownloads();
                DownloadsManager::getInstance().startDownloads();
                if (m_downloadButton) m_downloadButton->setText("Downloading...");
                return;
            case DownloadState::FAILED:
                // Allow retry - fall through to re-queue
                brls::Logger::info("Retrying failed download: {}", m_item.title);
                DownloadsManager::getInstance().deleteDownload(m_item.ratingKey);
                break;
            default:
                break;
        }
    }

    // Check if we have the part path (need to fetch full details first)
    if (m_item.partPath.empty()) {
        brls::Application::notify("Loading media info...");

        // Try to load details now
        PlexClient& client = PlexClient::getInstance();
        MediaItem fullItem;
        if (client.fetchMediaDetails(m_item.ratingKey, fullItem) && !fullItem.partPath.empty()) {
            m_item = fullItem;
            brls::Logger::debug("onDownload: Loaded partPath={}", m_item.partPath);
        } else {
            brls::Logger::debug("onDownload: partPath is still empty, cannot download");
            brls::Application::notify("Unable to download - media info not available");
            return;
        }
    }

    // Determine media type and parent info
    std::string mediaType;
    if (m_item.mediaType == MediaType::MOVIE) {
        mediaType = "movie";
    } else if (m_item.mediaType == MediaType::MUSIC_TRACK) {
        mediaType = "track";
    } else {
        mediaType = "episode";
    }

    std::string parentTitle = "";
    int seasonNum = 0;
    int episodeNum = 0;

    if (m_item.mediaType == MediaType::EPISODE) {
        parentTitle = m_item.grandparentTitle;  // Show name
        seasonNum = m_item.parentIndex;  // Season number
        episodeNum = m_item.index;       // Episode number
    } else if (m_item.mediaType == MediaType::MUSIC_TRACK) {
        parentTitle = m_item.grandparentTitle;  // Artist name
    }

    // Queue the download (pass thumb for cover art on music tracks)
    bool queued = DownloadsManager::getInstance().queueDownload(
        m_item.ratingKey,
        m_item.title,
        m_item.partPath,
        m_item.duration,
        mediaType,
        parentTitle,
        seasonNum,
        episodeNum,
        m_item.thumb
    );

    if (queued) {
        if (m_downloadButton) {
            m_downloadButton->setText("Queued");
        }

        // Just notify the user and add to queue - no progress dialog
        brls::Application::notify("Added to download queue: " + m_item.title);

        // Start downloading in background
        DownloadsManager::getInstance().startDownloads();
    } else {
        brls::Application::notify("Failed to queue download");
    }
}

void MediaDetailView::showDownloadOptions() {
    // Create dialog with download options
    auto* dialog = new brls::Dialog("Download Options");

    auto* optionsBox = new brls::Box();
    optionsBox->setAxis(brls::Axis::COLUMN);
    optionsBox->setPadding(20);

    // Helper to create touch-friendly dialog buttons
    auto addDialogButton = [&optionsBox](const std::string& text, std::function<bool(brls::View*)> action) {
        auto* btn = new brls::Button();
        btn->setText(text);
        btn->setHeight(44);
        btn->setMarginBottom(10);
        btn->registerClickAction(action);
        btn->addGestureRecognizer(new brls::TapGestureRecognizer(btn));
        optionsBox->addView(btn);
    };

    // Different options based on media type
    if (m_item.mediaType == MediaType::SHOW) {
        addDialogButton("Download All Episodes", [this, dialog](brls::View*) {
            dialog->dismiss(); downloadAll(); return true;
        });
        addDialogButton("Download Unwatched", [this, dialog](brls::View*) {
            dialog->dismiss(); downloadUnwatched(); return true;
        });
        addDialogButton("Download Next 5 Unwatched", [this, dialog](brls::View*) {
            dialog->dismiss(); downloadUnwatched(5); return true;
        });
    } else if (m_item.mediaType == MediaType::SEASON) {
        addDialogButton("Download All Episodes", [this, dialog](brls::View*) {
            dialog->dismiss(); downloadAll(); return true;
        });
        addDialogButton("Download Unwatched", [this, dialog](brls::View*) {
            dialog->dismiss(); downloadUnwatched(); return true;
        });
        addDialogButton("Download Next 3 Unwatched", [this, dialog](brls::View*) {
            dialog->dismiss(); downloadUnwatched(3); return true;
        });
    } else if (m_item.mediaType == MediaType::MUSIC_ALBUM) {
        addDialogButton("Download Album", [this, dialog](brls::View*) {
            dialog->dismiss(); downloadAll(); return true;
        });
    } else if (m_item.mediaType == MediaType::MUSIC_ARTIST) {
        addDialogButton("Download All Albums", [this, dialog](brls::View*) {
            dialog->dismiss(); downloadAll(); return true;
        });
    }

    addDialogButton("Cancel", [dialog](brls::View*) {
        dialog->dismiss(); return true;
    });

    dialog->addView(optionsBox);
    dialog->registerAction("Back", brls::ControllerButton::BUTTON_B, [dialog](brls::View*) {
        dialog->dismiss();
        return true;
    });
    brls::Application::pushActivity(new brls::Activity(dialog));
}

void MediaDetailView::downloadAll() {
    // Show progress dialog
    auto* progressDialog = new ProgressDialog("Preparing Downloads");
    progressDialog->setStatus("Fetching content list...");
    progressDialog->show();

    std::string ratingKey = m_item.ratingKey;
    MediaType mediaType = m_item.mediaType;
    std::string parentTitle = m_item.title;
    std::string itemThumb = m_item.thumb;

    asyncRun([this, progressDialog, ratingKey, mediaType, parentTitle, itemThumb]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;
        int queued = 0;
        int skipped = 0;

        if (mediaType == MediaType::SHOW) {
            // Get all seasons first, then all episodes
            std::vector<MediaItem> seasons;
            if (client.fetchChildren(ratingKey, seasons)) {
                for (const auto& season : seasons) {
                    std::vector<MediaItem> episodes;
                    if (client.fetchChildren(season.ratingKey, episodes)) {
                        for (auto& ep : episodes) {
                            items.push_back(ep);
                        }
                    }
                }
            }
        } else if (mediaType == MediaType::SEASON || mediaType == MediaType::MUSIC_ALBUM) {
            // Get direct children (episodes or tracks)
            client.fetchChildren(ratingKey, items);
        } else if (mediaType == MediaType::MUSIC_ARTIST) {
            // Get all albums, then all tracks
            std::vector<MediaItem> albums;
            if (client.fetchChildren(ratingKey, albums)) {
                for (const auto& album : albums) {
                    std::vector<MediaItem> tracks;
                    if (client.fetchChildren(album.ratingKey, tracks)) {
                        for (auto& track : tracks) {
                            items.push_back(track);
                        }
                    }
                }
            }
        }

        size_t itemCount = items.size();
        brls::sync([progressDialog, itemCount]() {
            progressDialog->setStatus("Found " + std::to_string(itemCount) + " items");
            progressDialog->setProgress(0.1f);
        });

        // If no items found, dismiss dialog
        if (items.empty()) {
            brls::sync([progressDialog]() {
                progressDialog->setStatus("No items found to download");
                brls::delay(1500, [progressDialog]() {
                    progressDialog->dismiss();
                });
            });
            return;
        }

        // Determine grouping based on media type
        DownloadGroupType dlGroupType = DownloadGroupType::NONE;
        if (mediaType == MediaType::MUSIC_ALBUM) dlGroupType = DownloadGroupType::ALBUM;
        else if (mediaType == MediaType::MUSIC_ARTIST) dlGroupType = DownloadGroupType::ARTIST;
        else if (mediaType == MediaType::SHOW) dlGroupType = DownloadGroupType::SHOW;
        else if (mediaType == MediaType::SEASON) dlGroupType = DownloadGroupType::SHOW;

        // Queue each item for download
        auto& mgr = DownloadsManager::getInstance();
        for (size_t i = 0; i < items.size(); i++) {
            const auto& item = items[i];

            // Skip items already downloaded or in queue
            if (mgr.isDownloaded(item.ratingKey) ||
                mgr.getDownload(item.ratingKey) != nullptr) {
                skipped++;
                size_t currentIndex = i;
                brls::sync([progressDialog, currentIndex, itemCount, queued, skipped]() {
                    progressDialog->setStatus("Queued " + std::to_string(queued) + " of " +
                                             std::to_string(itemCount) +
                                             (skipped > 0 ? " (" + std::to_string(skipped) + " skipped)" : ""));
                    progressDialog->setProgress(0.1f + 0.9f * static_cast<float>(currentIndex + 1) / itemCount);
                });
                continue;
            }

            // Get full details to get the part path
            MediaItem fullItem;
            if (client.fetchMediaDetails(item.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                std::string itemMediaType = "episode";
                if (fullItem.mediaType == MediaType::MOVIE) itemMediaType = "movie";
                else if (fullItem.mediaType == MediaType::MUSIC_TRACK) itemMediaType = "track";

                // For music tracks, pass thumb for cover art download
                std::string trackThumb = (itemMediaType == "track") ? fullItem.thumb : "";

                if (mgr.queueDownload(
                    fullItem.ratingKey,
                    fullItem.title,
                    fullItem.partPath,
                    fullItem.duration,
                    itemMediaType,
                    parentTitle,
                    fullItem.parentIndex,
                    fullItem.index,
                    trackThumb,
                    dlGroupType,
                    ratingKey,
                    parentTitle,
                    itemThumb,
                    fullItem.parentTitle  // album title for tracks
                )) {
                    queued++;
                }
            }

            size_t currentIndex = i;
            brls::sync([progressDialog, currentIndex, itemCount, queued, skipped]() {
                progressDialog->setStatus("Queued " + std::to_string(queued) + " of " +
                                         std::to_string(itemCount) +
                                         (skipped > 0 ? " (" + std::to_string(skipped) + " skipped)" : ""));
                progressDialog->setProgress(0.1f + 0.9f * static_cast<float>(currentIndex + 1) / itemCount);
            });
        }

        // Start downloads
        mgr.startDownloads();

        brls::sync([progressDialog, queued, skipped]() {
            std::string msg = "Queued " + std::to_string(queued) + " downloads";
            if (skipped > 0) {
                msg += " (" + std::to_string(skipped) + " already downloaded)";
            }
            progressDialog->setStatus(msg);
            brls::delay(1500, [progressDialog]() {
                progressDialog->dismiss();
            });
        });
    });
}

void MediaDetailView::downloadUnwatched(int maxCount) {
    // Show progress dialog
    auto* progressDialog = new ProgressDialog("Preparing Downloads");
    progressDialog->setStatus("Fetching unwatched content...");
    progressDialog->show();

    std::string ratingKey = m_item.ratingKey;
    MediaType mediaType = m_item.mediaType;
    std::string parentTitle = m_item.title;
    std::string itemThumb = m_item.thumb;

    asyncRun([this, progressDialog, ratingKey, mediaType, parentTitle, itemThumb, maxCount]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> unwatchedItems;
        int queued = 0;
        int skipped = 0;

        if (mediaType == MediaType::SHOW) {
            // Get all seasons first, then unwatched episodes
            std::vector<MediaItem> seasons;
            if (client.fetchChildren(ratingKey, seasons)) {
                for (const auto& season : seasons) {
                    std::vector<MediaItem> episodes;
                    if (client.fetchChildren(season.ratingKey, episodes)) {
                        for (auto& ep : episodes) {
                            if (!ep.watched && ep.viewOffset == 0) {
                                unwatchedItems.push_back(ep);
                                if (maxCount > 0 && (int)unwatchedItems.size() >= maxCount) {
                                    break;
                                }
                            }
                        }
                    }
                    if (maxCount > 0 && (int)unwatchedItems.size() >= maxCount) {
                        break;
                    }
                }
            }
        } else if (mediaType == MediaType::SEASON) {
            // Get unwatched episodes in this season
            std::vector<MediaItem> episodes;
            if (client.fetchChildren(ratingKey, episodes)) {
                for (auto& ep : episodes) {
                    if (!ep.watched && ep.viewOffset == 0) {
                        unwatchedItems.push_back(ep);
                        if (maxCount > 0 && (int)unwatchedItems.size() >= maxCount) {
                            break;
                        }
                    }
                }
            }
        }

        size_t itemCount = unwatchedItems.size();
        brls::sync([progressDialog, itemCount]() {
            progressDialog->setStatus("Found " + std::to_string(itemCount) + " unwatched");
            progressDialog->setProgress(0.1f);
        });

        // If no items found, dismiss dialog
        if (unwatchedItems.empty()) {
            brls::sync([progressDialog]() {
                progressDialog->setStatus("No unwatched items found");
                brls::delay(1500, [progressDialog]() {
                    progressDialog->dismiss();
                });
            });
            return;
        }

        // Determine grouping
        DownloadGroupType dlGroupType = DownloadGroupType::SHOW;

        // Queue each unwatched item for download
        auto& mgr = DownloadsManager::getInstance();
        for (size_t i = 0; i < unwatchedItems.size(); i++) {
            const auto& item = unwatchedItems[i];

            // Skip items already downloaded or in queue
            if (mgr.isDownloaded(item.ratingKey) ||
                mgr.getDownload(item.ratingKey) != nullptr) {
                skipped++;
                size_t currentIndex = i;
                brls::sync([progressDialog, currentIndex, itemCount, queued, skipped]() {
                    progressDialog->setStatus("Queued " + std::to_string(queued) + " of " +
                                             std::to_string(itemCount) +
                                             (skipped > 0 ? " (" + std::to_string(skipped) + " skipped)" : ""));
                    progressDialog->setProgress(0.1f + 0.9f * static_cast<float>(currentIndex + 1) / itemCount);
                });
                continue;
            }

            // Get full details to get the part path
            MediaItem fullItem;
            if (client.fetchMediaDetails(item.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                std::string itemMediaType = "episode";

                if (mgr.queueDownload(
                    fullItem.ratingKey,
                    fullItem.title,
                    fullItem.partPath,
                    fullItem.duration,
                    itemMediaType,
                    parentTitle,
                    fullItem.parentIndex,
                    fullItem.index,
                    "",
                    dlGroupType,
                    ratingKey,
                    parentTitle,
                    itemThumb
                )) {
                    queued++;
                }
            }

            size_t currentIndex = i;
            brls::sync([progressDialog, currentIndex, itemCount, queued, skipped]() {
                progressDialog->setStatus("Queued " + std::to_string(queued) + " of " +
                                         std::to_string(itemCount) +
                                         (skipped > 0 ? " (" + std::to_string(skipped) + " skipped)" : ""));
                progressDialog->setProgress(0.1f + 0.9f * static_cast<float>(currentIndex + 1) / itemCount);
            });
        }

        // Start downloads
        mgr.startDownloads();

        brls::sync([progressDialog, queued, skipped]() {
            std::string msg = "Queued " + std::to_string(queued) + " downloads";
            if (skipped > 0) {
                msg += " (" + std::to_string(skipped) + " already downloaded)";
            }
            progressDialog->setStatus(msg);
            brls::delay(1500, [progressDialog]() {
                progressDialog->dismiss();
            });
        });
    });
}

void MediaDetailView::loadTrackList() {
    if (!m_trackListBox) return;

    asyncRun([this]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> tracks;

        if (!client.fetchChildren(m_item.ratingKey, tracks)) {
            brls::Logger::error("Failed to fetch tracks for album");
            return;
        }

        std::weak_ptr<std::atomic<bool>> aliveWeak = m_alive;

        brls::sync([this, tracks, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !alive->load()) return;

            m_trackListBox->clearViews();
            m_children = tracks;

            for (size_t i = 0; i < tracks.size(); i++) {
                const auto& track = tracks[i];

                // Create a row for each track (like Suwayomi ChapterCell)
                auto* row = new brls::Box();
                row->setAxis(brls::Axis::ROW);
                row->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
                row->setAlignItems(brls::AlignItems::CENTER);
                row->setHeight(56);
                row->setPadding(10, 16, 10, 16);
                row->setMarginBottom(4);
                row->setCornerRadius(8);
                row->setBackgroundColor(nvgRGBA(50, 50, 60, 200));
                row->setFocusable(true);

                // Left side: track number + title
                auto* leftBox = new brls::Box();
                leftBox->setAxis(brls::Axis::ROW);
                leftBox->setAlignItems(brls::AlignItems::CENTER);
                leftBox->setGrow(1.0f);

                auto* trackNum = new brls::Label();
                trackNum->setFontSize(14);
                trackNum->setMarginRight(12);
                trackNum->setTextColor(nvgRGBA(150, 150, 150, 255));
                if (track.index > 0) {
                    trackNum->setText(std::to_string(track.index));
                } else {
                    trackNum->setText(std::to_string(i + 1));
                }
                leftBox->addView(trackNum);

                auto* titleLabel = new brls::Label();
                titleLabel->setFontSize(14);
                titleLabel->setText(track.title);
                leftBox->addView(titleLabel);

                row->addView(leftBox);

                // Right side: button hint + duration
                auto* rightSide = new brls::Box();
                rightSide->setAxis(brls::Axis::ROW);
                rightSide->setAlignItems(brls::AlignItems::CENTER);

                auto* hintIcon = new brls::Image();
                std::string hintPath = HintIcons::getResPath(brls::BUTTON_X);
                if (!hintPath.empty()) {
                    hintIcon->setImageFromRes(hintPath);
                }
                hintIcon->setWidth(16);
                hintIcon->setHeight(16);
                hintIcon->setMarginRight(2);
                hintIcon->setVisibility(brls::Visibility::INVISIBLE);
                // Refresh the icon if the input source flips (desktop / android).
                // Guard with m_alive so a flip after this view is destroyed
                // doesn't dereference the freed brls::Image.
                std::weak_ptr<std::atomic<bool>> aliveWeak = m_alive;
                HintIcons::onSourceChanged([aliveWeak, hintIcon]() {
                    auto a = aliveWeak.lock();
                    if (!a || !a->load()) return;
                    std::string p = HintIcons::getResPath(brls::BUTTON_X);
                    if (!p.empty()) hintIcon->setImageFromRes(p);
                });
                rightSide->addView(hintIcon);

                auto* hintLabel = new brls::Label();
                hintLabel->setFontSize(10);
                hintLabel->setTextColor(nvgRGBA(150, 150, 180, 180));
                hintLabel->setText("DL");
                hintLabel->setMarginRight(10);
                hintLabel->setVisibility(brls::Visibility::INVISIBLE);
                rightSide->addView(hintLabel);

                // Show hint on focus, hide previous (like Suwayomi chapter icon pattern)
                brls::Image* capturedHintIcon = hintIcon;
                brls::Label* capturedHintLabel = hintLabel;
                row->getFocusEvent()->subscribe([this, capturedHintIcon, capturedHintLabel](brls::View*) {
                    // Hide previously focused hint
                    if (m_currentFocusedHint && m_currentFocusedHint != capturedHintIcon) {
                        m_currentFocusedHint->setVisibility(brls::Visibility::INVISIBLE);
                    }
                    if (m_currentFocusedHintLabel && m_currentFocusedHintLabel != capturedHintLabel) {
                        m_currentFocusedHintLabel->setVisibility(brls::Visibility::INVISIBLE);
                    }
                    // Show current hint
                    capturedHintIcon->setVisibility(brls::Visibility::VISIBLE);
                    capturedHintLabel->setVisibility(brls::Visibility::VISIBLE);
                    m_currentFocusedHint = capturedHintIcon;
                    m_currentFocusedHintLabel = capturedHintLabel;
                });

                if (track.duration > 0) {
                    auto* durLabel = new brls::Label();
                    durLabel->setFontSize(12);
                    durLabel->setTextColor(nvgRGBA(150, 150, 150, 255));
                    int totalSec = track.duration / 1000;
                    int min = totalSec / 60;
                    int sec = totalSec % 60;
                    char durStr[16];
                    snprintf(durStr, sizeof(durStr), "%d:%02d", min, sec);
                    durLabel->setText(durStr);
                    rightSide->addView(durLabel);
                }

                row->addView(rightSide);

                // Click to perform default track action
                MediaItem capturedTrack = track;
                row->registerClickAction([this, capturedTrack, i](brls::View* view) {
                    performTrackAction(capturedTrack, i);
                    return true;
                });
                row->addGestureRecognizer(new brls::TapGestureRecognizer(row));

                // START button always shows track action dialog
                row->registerAction("Options", brls::ControllerButton::BUTTON_START, [this, capturedTrack, i](brls::View* view) {
                    showTrackActionDialog(capturedTrack, i);
                    return true;
                });
                row->addGestureRecognizer(new LongPressGestureRecognizer(
                    row, [this, capturedTrack, i](LongPressGestureStatus status) {
                        if (status.state == brls::GestureState::START) {
                            showTrackActionDialog(capturedTrack, i);
                        }
                    }));

                // Square button (X on PS Vita) adds to download queue
                row->registerAction("Download", brls::ControllerButton::BUTTON_X, [this, capturedTrack](brls::View* view) {
                    // Create a temporary copy to download
                    MediaItem dlItem = capturedTrack;
                    if (DownloadsManager::getInstance().isDownloaded(dlItem.ratingKey)) {
                        brls::Application::notify("Already downloaded");
                    } else {
                        // Fetch full details for partPath
                        asyncRun([this, dlItem]() {
                            PlexClient& client = PlexClient::getInstance();
                            MediaItem fullItem;
                            if (client.fetchMediaDetails(dlItem.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                                bool queued = DownloadsManager::getInstance().queueDownload(
                                    fullItem.ratingKey, fullItem.title, fullItem.partPath,
                                    fullItem.duration, "track",
                                    fullItem.grandparentTitle, 0, fullItem.index,
                                    fullItem.thumb);
                                brls::sync([queued, fullItem]() {
                                    if (queued) {
                                        DownloadsManager::getInstance().startDownloads();
                                        brls::Application::notify("Downloading: " + fullItem.title);
                                    } else {
                                        brls::Application::notify("Failed to queue download");
                                    }
                                });
                            } else {
                                brls::sync([]() {
                                    brls::Application::notify("Could not get download info");
                                });
                            }
                        });
                    }
                    return true;
                });

                m_trackListBox->addView(row);
            }

            // Set up focus transfer for album track list. Only the
            // FIRST track gets a custom UP route — every other track
            // keeps default vertical nav (UP -> previous track), so
            // pressing UP from track 5 actually goes to track 4
            // instead of jumping to the description (the Android TV
            // bug: the for-loop below used to override UP on EVERY
            // child, breaking track-to-track navigation entirely).
            if (!m_trackListBox->getChildren().empty()) {
                brls::View* firstTrack = m_trackListBox->getChildren().front();

                if (m_summaryLabel && m_summaryLabel->isFocusable() && !m_fullDescription.empty()) {
                    // Description exists: DOWN from description goes to first track
                    m_summaryLabel->setCustomNavigationRoute(brls::FocusDirection::DOWN, firstTrack);
                    // UP from the first track goes to description; tracks
                    // 2..N keep default behaviour (UP -> previous track).
                    firstTrack->setCustomNavigationRoute(brls::FocusDirection::UP, m_summaryLabel);
                } else {
                    // No description: transfer focus to first track to avoid focus errors
                    brls::Application::giveFocus(firstTrack);

                    // UP from the first track goes to play button if it exists
                    if (m_playButton) {
                        firstTrack->setCustomNavigationRoute(brls::FocusDirection::UP, m_playButton);
                        m_playButton->setCustomNavigationRoute(brls::FocusDirection::DOWN, firstTrack);
                    }
                    if (m_downloadButton) {
                        m_downloadButton->setCustomNavigationRoute(brls::FocusDirection::DOWN, firstTrack);
                    }
                }
            }
        });
    });
}

void MediaDetailView::performTrackAction(const MediaItem& track, size_t trackIndex) {
    TrackDefaultAction action = Application::getInstance().getSettings().trackDefaultAction;

    if (action == TrackDefaultAction::ASK_EACH_TIME) {
        showTrackActionDialog(track, trackIndex);
        return;
    }

    MusicQueue& queue = MusicQueue::getInstance();

    switch (action) {
        case TrackDefaultAction::PLAY_NEXT:
            // Add after current track in queue
            if (queue.isEmpty()) {
                // No queue yet - start with just this track
                std::vector<MediaItem> single = {track};
                auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
                brls::Application::pushActivity(playerActivity);
            } else {
                queue.insertTrackAfterCurrent(track);
                brls::Application::notify("Playing next: " + track.title);
            }
            break;

        case TrackDefaultAction::PLAY_NOW_REPLACE:
            // Replace current track and play
            if (queue.isEmpty()) {
                std::vector<MediaItem> single = {track};
                auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
                brls::Application::pushActivity(playerActivity);
            } else {
                queue.insertTrackAfterCurrent(track);
                if (queue.playNext()) {
                    brls::Application::notify("Now playing: " + track.title);
                }
            }
            break;

        case TrackDefaultAction::ADD_TO_BOTTOM:
            // Add to end of queue
            if (queue.isEmpty()) {
                std::vector<MediaItem> single = {track};
                auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
                brls::Application::pushActivity(playerActivity);
            } else {
                queue.addTrack(track);
                brls::Application::notify("Added to queue: " + track.title);
            }
            break;

        case TrackDefaultAction::PLAY_NOW_CLEAR:
        default:
            // Clear queue and play just this track
            {
                std::vector<MediaItem> single = {track};
                auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
                brls::Application::pushActivity(playerActivity);
            }
            break;
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Options popover (artboard "D4a")
//
//  A compact panel anchored to the focused cell, replacing the old centered
//  brls::Dialog + vertical brls::Button stack used by every show* context
//  menu. Presentation only: each menu translates its former buttons into
//  OptionRow entries and hands the vector to showOptionsPopover().
// ─────────────────────────────────────────────────────────────────────────
namespace {

// Palette literals scoped to this component (matches artboard "D4a").
namespace popcol {
    // Match the app shell: panel = sidebar/sheet surface (#323232),
    // line = the sidebar separator hairline. Keeps the popover reading as
    // the same neutral surface as the background and sidebar, not a tint.
    inline NVGcolor panel()     { return nvgRGB(50, 50, 50); }
    inline NVGcolor line()      { return nvgRGB(67, 67, 74); }
    inline NVGcolor text()      { return nvgRGB(255, 255, 255); }
    inline NVGcolor muted()     { return nvgRGB(0xA8, 0xA6, 0xB4); }
    inline NVGcolor dim()       { return nvgRGB(0x80, 0x7E, 0x8C); }
    inline NVGcolor gold()      { return nvgRGB(0xE5, 0xA0, 0x0D); }
    inline NVGcolor goldBright(){ return nvgRGB(0xFF, 0xC2, 0x3D); }
    inline NVGcolor goldInk()   { return nvgRGB(0x24, 0x1C, 0x08); }
    inline NVGcolor scrim()     { return nvgRGBA(10, 9, 14, 128); }
    inline NVGcolor goldInkSub(){ return nvgRGBA(36, 28, 8, 168); }  // sub on gold (~.66)
}

// Translucent host so the underlying detail screen shows through the scrim.
class PopoverActivity : public brls::Activity {
public:
    explicit PopoverActivity(brls::Box* content) : brls::Activity(content) {}
    bool isTranslucent() override { return true; }
};

// The three glyphs the user standardised on (download / restart / close)
// are drawn as exact MDI vector paths (24x24 viewBox) rather than relying
// on whichever PNG is in resources/icons — crisp at any size and tintable.
// nanovg fills with nonzero winding, so download's separate bar + arrow
// sub-paths render correctly. Mirrors login_activity's drawStrokeGlyph.
enum class MdiGlyph { Download, Restart, Close, ClosedCaption, Web };

class MdiGlyphIcon : public brls::Box {
public:
    MdiGlyphIcon(MdiGlyph g, NVGcolor color) : m_glyph(g), m_color(color) {}
    void draw(NVGcontext* vg, float x, float y, float w, float h,
              brls::Style style, brls::FrameContext* ctx) override {
        brls::Box::draw(vg, x, y, w, h, style, ctx);
        const float side = (w < h) ? w : h;
        const float gx = x + (w - side) * 0.5f;
        const float gy = y + (h - side) * 0.5f;
        const float s  = side / 24.0f;
        auto X = [=](float v) { return gx + v * s; };
        auto Y = [=](float v) { return gy + v * s; };
        if (m_glyph == MdiGlyph::Web) {
            // Line-art globe: mdi-web's filled body uses elliptical arcs nanovg
            // can't express directly, so stroke an equivalent globe (outer
            // circle + meridian ellipse + parallels) tinted to m_color.
            nvgBeginPath(vg);
            nvgCircle(vg, X(12), Y(12), 10.0f * s);
            nvgEllipse(vg, X(12), Y(12), 4.0f * s, 10.0f * s);
            nvgMoveTo(vg, X(2.5f), Y(12));  nvgLineTo(vg, X(21.5f), Y(12));
            nvgMoveTo(vg, X(4.5f), Y(8));   nvgLineTo(vg, X(19.5f), Y(8));
            nvgMoveTo(vg, X(4.5f), Y(16));  nvgLineTo(vg, X(19.5f), Y(16));
            nvgStrokeColor(vg, m_color);
            nvgStrokeWidth(vg, 1.5f * s);
            nvgStroke(vg);
            return;
        }
        nvgBeginPath(vg);
        switch (m_glyph) {
            case MdiGlyph::Close:
                nvgMoveTo(vg, X(19), Y(6.41f));   nvgLineTo(vg, X(17.59f), Y(5));
                nvgLineTo(vg, X(12), Y(10.59f));  nvgLineTo(vg, X(6.41f), Y(5));
                nvgLineTo(vg, X(5), Y(6.41f));    nvgLineTo(vg, X(10.59f), Y(12));
                nvgLineTo(vg, X(5), Y(17.59f));   nvgLineTo(vg, X(6.41f), Y(19));
                nvgLineTo(vg, X(12), Y(13.41f));  nvgLineTo(vg, X(17.59f), Y(19));
                nvgLineTo(vg, X(19), Y(17.59f));  nvgLineTo(vg, X(13.41f), Y(12));
                nvgClosePath(vg);
                break;
            case MdiGlyph::Download:
                nvgMoveTo(vg, X(5), Y(20));  nvgLineTo(vg, X(19), Y(20));
                nvgLineTo(vg, X(19), Y(18)); nvgLineTo(vg, X(5), Y(18));
                nvgClosePath(vg);
                nvgMoveTo(vg, X(19), Y(9));  nvgLineTo(vg, X(15), Y(9));
                nvgLineTo(vg, X(15), Y(3));  nvgLineTo(vg, X(9), Y(3));
                nvgLineTo(vg, X(9), Y(9));   nvgLineTo(vg, X(5), Y(9));
                nvgLineTo(vg, X(12), Y(16)); nvgLineTo(vg, X(19), Y(9));
                nvgClosePath(vg);
                break;
            case MdiGlyph::Restart:
                nvgMoveTo(vg, X(12), Y(4));
                nvgBezierTo(vg, X(14.1f), Y(4),     X(16.1f), Y(4.8f),  X(17.6f), Y(6.3f));
                nvgBezierTo(vg, X(20.7f), Y(9.4f),  X(20.7f), Y(14.5f), X(17.6f), Y(17.6f));
                nvgBezierTo(vg, X(15.8f), Y(19.5f), X(13.3f), Y(20.2f), X(10.9f), Y(19.9f));
                nvgLineTo(vg, X(11.4f), Y(17.9f));
                nvgBezierTo(vg, X(13.1f), Y(18.1f), X(14.9f), Y(17.5f), X(16.2f), Y(16.2f));
                nvgBezierTo(vg, X(18.5f), Y(13.9f), X(18.5f), Y(10.1f), X(16.2f), Y(7.7f));
                nvgBezierTo(vg, X(15.1f), Y(6.6f),  X(13.5f), Y(6),     X(12),    Y(6));
                nvgLineTo(vg, X(12), Y(10.6f));
                nvgLineTo(vg, X(7),  Y(5.6f));
                nvgLineTo(vg, X(12), Y(0.6f));
                nvgClosePath(vg);
                nvgMoveTo(vg, X(6.3f), Y(17.6f));
                nvgBezierTo(vg, X(3.7f), Y(15),    X(3.3f), Y(11),    X(5.1f), Y(7.9f));
                nvgLineTo(vg, X(6.6f), Y(9.4f));
                nvgBezierTo(vg, X(5.5f), Y(11.6f), X(5.9f), Y(14.4f), X(7.8f), Y(16.2f));
                nvgBezierTo(vg, X(8.3f), Y(16.7f), X(8.9f), Y(17.1f), X(9.6f), Y(17.4f));
                nvgLineTo(vg, X(9), Y(19.4f));
                nvgBezierTo(vg, X(8), Y(19),       X(7.1f), Y(18.4f), X(6.3f), Y(17.6f));
                nvgClosePath(vg);
                break;
            case MdiGlyph::ClosedCaption:
                // Outer rounded frame (solid).
                nvgMoveTo(vg, X(5), Y(4));
                nvgBezierTo(vg, X(4.45f), Y(4),     X(4),     Y(4.18f),  X(3.59f), Y(4.57f));
                nvgBezierTo(vg, X(3.2f),  Y(4.96f),  X(3),     Y(5.44f),  X(3),     Y(6));
                nvgLineTo(vg, X(3), Y(18));
                nvgBezierTo(vg, X(3),     Y(18.56f), X(3.2f),  Y(19.04f), X(3.59f), Y(19.43f));
                nvgBezierTo(vg, X(4),     Y(19.82f), X(4.45f), Y(20),     X(5),     Y(20));
                nvgLineTo(vg, X(19), Y(20));
                nvgBezierTo(vg, X(19.5f), Y(20),     X(20),    Y(19.81f), X(20.39f),Y(19.41f));
                nvgBezierTo(vg, X(20.8f), Y(19),     X(21),    Y(18.53f), X(21),    Y(18));
                nvgLineTo(vg, X(21), Y(6));
                nvgBezierTo(vg, X(21),    Y(5.47f),  X(20.8f), Y(5),      X(20.39f),Y(4.59f));
                nvgBezierTo(vg, X(20),    Y(4.19f),  X(19.5f), Y(4),      X(19),    Y(4));
                nvgLineTo(vg, X(5), Y(4));
                nvgClosePath(vg);
                nvgPathWinding(vg, NVG_SOLID);
                // Inner screen (hole).
                nvgMoveTo(vg, X(4.5f), Y(5.5f));
                nvgLineTo(vg, X(19.5f), Y(5.5f));
                nvgLineTo(vg, X(19.5f), Y(18.5f));
                nvgLineTo(vg, X(4.5f), Y(18.5f));
                nvgLineTo(vg, X(4.5f), Y(5.5f));
                nvgClosePath(vg);
                nvgPathWinding(vg, NVG_HOLE);
                // Left "C".
                nvgMoveTo(vg, X(7), Y(9));
                nvgBezierTo(vg, X(6.7f),  Y(9),      X(6.47f), Y(9.09f),  X(6.28f), Y(9.28f));
                nvgBezierTo(vg, X(6.09f), Y(9.47f),  X(6),     Y(9.7f),   X(6),     Y(10));
                nvgLineTo(vg, X(6), Y(14));
                nvgBezierTo(vg, X(6),     Y(14.3f),  X(6.09f), Y(14.53f), X(6.28f), Y(14.72f));
                nvgBezierTo(vg, X(6.47f), Y(14.91f), X(6.7f),  Y(15),     X(7),     Y(15));
                nvgLineTo(vg, X(10), Y(15));
                nvgBezierTo(vg, X(10.27f),Y(15),     X(10.5f), Y(14.91f), X(10.71f),Y(14.72f));
                nvgBezierTo(vg, X(10.91f),Y(14.53f), X(11),    Y(14.3f),  X(11),    Y(14));
                nvgLineTo(vg, X(11), Y(13));
                nvgLineTo(vg, X(9.5f), Y(13));
                nvgLineTo(vg, X(9.5f), Y(13.5f));
                nvgLineTo(vg, X(7.5f), Y(13.5f));
                nvgLineTo(vg, X(7.5f), Y(10.5f));
                nvgLineTo(vg, X(9.5f), Y(10.5f));
                nvgLineTo(vg, X(9.5f), Y(11));
                nvgLineTo(vg, X(11), Y(11));
                nvgLineTo(vg, X(11), Y(10));
                nvgBezierTo(vg, X(11),    Y(9.7f),   X(10.91f),Y(9.47f),  X(10.71f),Y(9.28f));
                nvgBezierTo(vg, X(10.5f), Y(9.09f),  X(10.27f),Y(9),      X(10),    Y(9));
                nvgLineTo(vg, X(7), Y(9));
                nvgClosePath(vg);
                nvgPathWinding(vg, NVG_SOLID);
                // Right "C".
                nvgMoveTo(vg, X(14), Y(9));
                nvgBezierTo(vg, X(13.73f),Y(9),      X(13.5f), Y(9.09f),  X(13.29f),Y(9.28f));
                nvgBezierTo(vg, X(13.09f),Y(9.47f),  X(13),    Y(9.7f),   X(13),    Y(10));
                nvgLineTo(vg, X(13), Y(14));
                nvgBezierTo(vg, X(13),    Y(14.3f),  X(13.09f),Y(14.53f), X(13.29f),Y(14.72f));
                nvgBezierTo(vg, X(13.5f), Y(14.91f), X(13.73f),Y(15),     X(14),    Y(15));
                nvgLineTo(vg, X(17), Y(15));
                nvgBezierTo(vg, X(17.3f), Y(15),     X(17.53f),Y(14.91f), X(17.72f),Y(14.72f));
                nvgBezierTo(vg, X(17.91f),Y(14.53f), X(18),    Y(14.3f),  X(18),    Y(14));
                nvgLineTo(vg, X(18), Y(13));
                nvgLineTo(vg, X(16.5f), Y(13));
                nvgLineTo(vg, X(16.5f), Y(13.5f));
                nvgLineTo(vg, X(14.5f), Y(13.5f));
                nvgLineTo(vg, X(14.5f), Y(10.5f));
                nvgLineTo(vg, X(16.5f), Y(10.5f));
                nvgLineTo(vg, X(16.5f), Y(11));
                nvgLineTo(vg, X(18), Y(11));
                nvgLineTo(vg, X(18), Y(10));
                nvgBezierTo(vg, X(18),    Y(9.7f),   X(17.91f),Y(9.47f),  X(17.72f),Y(9.28f));
                nvgBezierTo(vg, X(17.53f),Y(9.09f),  X(17.3f), Y(9),      X(17),    Y(9));
                nvgLineTo(vg, X(14), Y(9));
                nvgClosePath(vg);
                nvgPathWinding(vg, NVG_SOLID);
                break;
        }
        nvgFillColor(vg, m_color);
        nvgFill(vg);
    }
private:
    MdiGlyph m_glyph;
    NVGcolor m_color;
};

}  // namespace

void MediaDetailView::showOptionsPopover(brls::View* anchor,
                                         const std::string& contextLine,
                                         const std::string& title,
                                         std::vector<OptionRow> rows) {
    namespace pc = popcol;

    // The episode menu passes an "S{n} · E{n}" context line; every other menu
    // passes an uppercase type word ("MOVIE", "SEASON 2", …) or "". The middot
    // marks the episodic case, which gets a gold (rather than dim) context line.
    const bool episodic = contextLine.find("\xC2\xB7") != std::string::npos;

    // ── Geometry ────────────────────────────────────────────────────────
    const float screenW = platform::viewportWidth();
    const float screenH = platform::viewportHeight();
    const float kPopoverW = 320.0f;
    const float kMargin   = 40.0f;
    const bool  bottomSheet =
        platform::isPortrait() || (kPopoverW + 2.0f * kMargin) > screenW || anchor == nullptr;

    // ── Scrim (full-screen) ─────────────────────────────────────────────
    auto* scrim = new brls::Box();
    scrim->setAxis(brls::Axis::COLUMN);
    scrim->setWidthPercentage(100.0f);
    scrim->setHeightPercentage(100.0f);
    scrim->setBackgroundColor(pc::scrim());
    // Bottom sheet docks to the bottom edge; anchored popover is positioned
    // absolutely so the scrim itself just needs to fill the screen.
    if (bottomSheet) {
        scrim->setJustifyContent(brls::JustifyContent::FLEX_END);
        scrim->setAlignItems(brls::AlignItems::STRETCH);
    }
    scrim->addGestureRecognizer(new brls::TapGestureRecognizer(scrim,
        []() { brls::Application::popActivity(); }));

    // ── Popover panel ───────────────────────────────────────────────────
    auto* panel = new brls::Box();
    panel->setAxis(brls::Axis::COLUMN);
    panel->setBackgroundColor(pc::panel());
    panel->setBorderColor(pc::line());
    panel->setBorderThickness(1.0f);
    panel->setShadowType(brls::ShadowType::GENERIC);
    panel->setPadding(8.0f, 8.0f, 8.0f, 8.0f);

    if (bottomSheet) {
        panel->setCornerRadius(14.0f);
        panel->setWidthPercentage(100.0f);
    } else {
        panel->setCornerRadius(14.0f);
        panel->setWidth(kPopoverW);
        panel->setPositionType(brls::PositionType::ABSOLUTE);

        const float ax = anchor->getX();
        const float ay = anchor->getY();
        const float aw = anchor->getWidth();
        const float ah = anchor->getHeight();

        // Horizontal: centre on the cell, then clamp into the screen margins.
        float x = ax + aw * 0.5f - kPopoverW * 0.5f;
        if (x < kMargin) x = kMargin;
        if (x + kPopoverW > screenW - kMargin) x = screenW - kMargin - kPopoverW;
        panel->setPositionLeft(x);

        // Vertical: cells in the lower ~45% open upward, otherwise downward.
        // The panel height isn't known pre-layout, so estimate it (header +
        // rows) to place the bottom edge when opening above the cell.
        const float kRowH = 44.0f, kHeaderH = 56.0f, kGap = 8.0f, kPad = 16.0f;
        const float estH = kHeaderH + kPad + static_cast<float>(rows.size()) * kRowH;
        const bool above = (ay + ah * 0.5f) > screenH * 0.55f;
        float y = above ? (ay - kGap - estH) : (ay + ah + kGap);
        if (y < kMargin) y = kMargin;
        if (y + estH > screenH - kMargin) y = screenH - kMargin - estH;
        if (y < kMargin) y = kMargin;
        panel->setPositionTop(y);
    }

    // ── Header (context line + title) ───────────────────────────────────
    // borealis only supports a uniform border, so the bottom rule under the
    // header is a separate 1px divider box rather than a per-side border.
    auto* header = new brls::Box();
    header->setAxis(brls::Axis::COLUMN);
    header->setPadding(8.0f, 10.0f, 11.0f, 10.0f);

    if (!contextLine.empty()) {
        auto* ctx = new brls::Label();
        ctx->setText(contextLine);
        ctx->setFontSize(11.0f);
        // Gold for episodic context ("S1 · E2"), dim otherwise.
        ctx->setTextColor(episodic ? pc::gold() : pc::dim());
        ctx->setSingleLine(true);
        ctx->setMarginBottom(2.0f);
        header->addView(ctx);
    }
    auto* titleLabel = new brls::Label();
    titleLabel->setText(title);
    titleLabel->setFontSize(16.0f);
    titleLabel->setTextColor(pc::text());
    titleLabel->setSingleLine(true);
    header->addView(titleLabel);
    panel->addView(header);

    auto* divider = new brls::Box();
    divider->setHeight(1.0f);
    divider->setAlignSelf(brls::AlignSelf::STRETCH);
    divider->setBackgroundColor(pc::line());
    divider->setMarginBottom(6.0f);
    panel->addView(divider);

    // ── Rows ────────────────────────────────────────────────────────────
    brls::View* defaultFocus = nullptr;
    brls::View* firstRow      = nullptr;
    for (auto& r : rows) {
        OptionRow row = r;  // copy into the row closure

        auto* rowBox = new brls::Box();
        rowBox->setAxis(brls::Axis::ROW);
        rowBox->setAlignItems(brls::AlignItems::CENTER);
        rowBox->setHeight(44.0f);
        rowBox->setPadding(0.0f, 12.0f, 0.0f, 12.0f);
        rowBox->setCornerRadius(9.0f);
        rowBox->setFocusable(true);
        rowBox->setHighlightCornerRadius(9.0f);

        // Leading icon. download/restart/close are drawn as exact MDI
        // vectors (tinted to match the row); everything else uses its PNG.
        brls::View* iconView;
        NVGcolor iconColor = pc::text();
        if (row.icon == "download.png") {
            iconView = new MdiGlyphIcon(MdiGlyph::Download, iconColor);
        } else if (row.icon == "refresh.png") {
            iconView = new MdiGlyphIcon(MdiGlyph::Restart, iconColor);
        } else if (row.icon == "cross.png") {
            iconView = new MdiGlyphIcon(MdiGlyph::Close, iconColor);
        } else {
            auto* img = new brls::Image();
            if (!row.icon.empty()) img->setImageFromRes("icons/" + row.icon);
            img->setScalingType(brls::ImageScalingType::FIT);
            iconView = img;
        }
        iconView->setWidth(20.0f);
        iconView->setHeight(20.0f);
        iconView->setMarginRight(11.0f);
        rowBox->addView(iconView);

        // Label.
        auto* lbl = new brls::Label();
        lbl->setText(row.label);
        lbl->setFontSize(15.0f);
        lbl->setSingleLine(true);
        lbl->setGrow(1.0f);
        if (row.danger) lbl->setTextColor(pc::muted());
        else            lbl->setTextColor(pc::text());
        rowBox->addView(lbl);

        // Trailing mono sub-value.
        if (!row.sub.empty()) {
            auto* sub = new brls::Label();
            sub->setText(row.sub);
            sub->setFontSize(12.0f);
            sub->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
            sub->setSingleLine(true);
            sub->setMarginLeft(8.0f);
            sub->setTextColor(pc::dim());
            rowBox->addView(sub);
        }

        // Activate: dismiss the popover first (preserving the old
        // dialog->dismiss() ordering), then run the verbatim action body.
        auto act = row.action;
        auto onActivate = [act](brls::View* v) -> bool {
            brls::Application::popActivity(brls::TransitionAnimation::FADE,
                [act, v]() { if (act) act(v); });
            return true;
        };
        rowBox->registerClickAction(onActivate);
        rowBox->addGestureRecognizer(new brls::TapGestureRecognizer(rowBox));

        panel->addView(rowBox);
        if (!firstRow) firstRow = rowBox;
        if (row.primary && !defaultFocus) defaultFocus = rowBox;
    }
    if (!defaultFocus) defaultFocus = firstRow;

    scrim->addView(panel);

    scrim->registerAction("Back", brls::ControllerButton::BUTTON_B,
        [](brls::View*) { brls::Application::popActivity(); return true; });

    brls::Application::pushActivity(new PopoverActivity(scrim));
    if (defaultFocus) brls::Application::giveFocus(defaultFocus);
}

void MediaDetailView::showTrackActionDialog(const MediaItem& track, size_t trackIndex) {
    brls::View* anchor = brls::Application::getCurrentFocus();

    MediaItem capturedTrack = track;
    size_t capturedIndex = trackIndex;

    std::vector<OptionRow> rows;

    rows.push_back({ "play.png", "Play Now (Clear Queue)", "", true, false,
        [this, capturedTrack](brls::View*) {
        // Play only this single track
        std::vector<MediaItem> single = {capturedTrack};
        auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
        brls::Application::pushActivity(playerActivity);
        return true;
    }});

    rows.push_back({ "skip-next.png", "Play Next", "", false, false,
        [this, capturedTrack](brls::View*) {
        MusicQueue& queue = MusicQueue::getInstance();
        if (queue.isEmpty()) {
            // Start new queue with just this track
            std::vector<MediaItem> single = {capturedTrack};
            auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
            brls::Application::pushActivity(playerActivity);
        } else {
            queue.insertTrackAfterCurrent(capturedTrack);
            brls::Application::notify("Playing next: " + capturedTrack.title);
        }
        return true;
    }});

    rows.push_back({ "format-list-group.png", "Add to Bottom of Queue", "", false, false,
        [this, capturedTrack](brls::View*) {
        MusicQueue& queue = MusicQueue::getInstance();
        if (queue.isEmpty()) {
            std::vector<MediaItem> single = {capturedTrack};
            auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
            brls::Application::pushActivity(playerActivity);
        } else {
            queue.addTrack(capturedTrack);
            brls::Application::notify("Added to queue: " + capturedTrack.title);
        }
        return true;
    }});

    rows.push_back({ "book-multiple.png", "Add to Playlist", "", false, false,
        [this, capturedTrack, anchor](brls::View*) {
        asyncRun([this, capturedTrack, anchor]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<Playlist> playlists;
            client.fetchMusicPlaylists(playlists);

            brls::sync([this, playlists, capturedTrack, anchor]() {
                auto alive = m_alive;
                if (!alive || !alive->load()) return;

                std::vector<OptionRow> plRows;

                plRows.push_back({ "book-multiple.png", "+ New Playlist", "", false, false,
                    [capturedTrack](brls::View*) {
                    brls::Application::getImeManager()->openForText([capturedTrack](std::string name) {
                        if (name.empty()) return;
                        asyncRun([name, capturedTrack]() {
                            PlexClient& client = PlexClient::getInstance();
                            std::vector<std::string> keys = {capturedTrack.ratingKey};
                            Playlist result;
                            if (client.createPlaylistWithItems(name, keys, result)) {
                                brls::sync([name]() {
                                    brls::Application::notify("Created playlist: " + name);
                                });
                            }
                        });
                    }, "New Playlist", "Enter playlist name", 128, "");
                    return true;
                }});

                for (const auto& pl : playlists) {
                    if (pl.smart) continue;
                    Playlist capturedPl = pl;
                    plRows.push_back({ "format-list-group.png", pl.title, "", false, false,
                        [capturedPl, capturedTrack](brls::View*) {
                        asyncRun([capturedPl, capturedTrack]() {
                            PlexClient& client = PlexClient::getInstance();
                            std::vector<std::string> keys = {capturedTrack.ratingKey};
                            if (client.addToPlaylist(capturedPl.ratingKey, keys)) {
                                brls::sync([capturedPl]() {
                                    brls::Application::notify("Added to " + capturedPl.title);
                                });
                            }
                        });
                        return true;
                    }});
                }

                plRows.push_back({ "cross.png", "Cancel", "", false, true,
                    [](brls::View*) {
                    return true;
                }});

                MediaDetailView::showOptionsPopover(anchor, "TRACK", "Add to Playlist", std::move(plRows));
            });
        });
        return true;
    }});

    rows.push_back({ "cross.png", "Cancel", "", false, true,
        [](brls::View*) {
        return true;
    }});

    showOptionsPopover(anchor, "TRACK", track.title, std::move(rows));
}

void MediaDetailView::showAlbumContextMenu(const MediaItem& album) {
    brls::View* anchor = brls::Application::getCurrentFocus();

    MediaItem capturedAlbum = album;

    std::vector<OptionRow> rows;

    rows.push_back({ "play.png", "Play Now (Clear Queue)", "", true, false,
        [this, capturedAlbum](brls::View*) {
        // Fetch album tracks and play
        asyncRun([this, capturedAlbum]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> tracks;
            if (client.fetchChildren(capturedAlbum.ratingKey, tracks) && !tracks.empty()) {
                brls::sync([tracks]() {
                    auto* playerActivity = PlayerActivity::createWithQueue(tracks, 0);
                    brls::Application::pushActivity(playerActivity);
                });
            }
        });
        return true;
    }});

    rows.push_back({ "skip-next.png", "Play Next", "", false, false,
        [this, capturedAlbum](brls::View*) {
        asyncRun([capturedAlbum]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> tracks;
            if (client.fetchChildren(capturedAlbum.ratingKey, tracks)) {
                brls::sync([tracks]() {
                    MusicQueue& queue = MusicQueue::getInstance();
                    if (queue.isEmpty()) {
                        auto* playerActivity = PlayerActivity::createWithQueue(tracks, 0);
                        brls::Application::pushActivity(playerActivity);
                    } else {
                        // Insert all tracks after current
                        for (int i = (int)tracks.size() - 1; i >= 0; i--) {
                            queue.insertTrackAfterCurrent(tracks[i]);
                        }
                        brls::Application::notify("Album queued next");
                    }
                });
            }
        });
        return true;
    }});

    rows.push_back({ "format-list-group.png", "Add to Bottom of Queue", "", false, false,
        [this, capturedAlbum](brls::View*) {
        asyncRun([capturedAlbum]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> tracks;
            if (client.fetchChildren(capturedAlbum.ratingKey, tracks)) {
                brls::sync([tracks]() {
                    MusicQueue& queue = MusicQueue::getInstance();
                    if (queue.isEmpty()) {
                        auto* playerActivity = PlayerActivity::createWithQueue(tracks, 0);
                        brls::Application::pushActivity(playerActivity);
                    } else {
                        queue.addTracks(tracks);
                        brls::Application::notify("Album added to queue");
                    }
                });
            }
        });
        return true;
    }});

    rows.push_back({ "book-multiple.png", "Add to Playlist", "", false, false,
        [this, capturedAlbum, anchor](brls::View*) {
        // Fetch audio playlists and let user pick one
        asyncRun([this, capturedAlbum, anchor]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<Playlist> playlists;
            client.fetchMusicPlaylists(playlists);

            // Also fetch album tracks to get their ratingKeys
            std::vector<MediaItem> tracks;
            client.fetchChildren(capturedAlbum.ratingKey, tracks);

            brls::sync([this, playlists, tracks, capturedAlbum, anchor]() {
                auto alive = m_alive;
                if (!alive || !alive->load()) return;

                if (tracks.empty()) {
                    brls::Application::notify("No tracks found");
                    return;
                }

                std::vector<OptionRow> plRows;

                // Option to create new playlist with this album
                plRows.push_back({ "book-multiple.png", "+ New Playlist", "", false, false,
                    [tracks](brls::View*) {
                    brls::Application::getImeManager()->openForText([tracks](std::string name) {
                        if (name.empty()) return;
                        asyncRun([name, tracks]() {
                            PlexClient& client = PlexClient::getInstance();
                            std::vector<std::string> keys;
                            for (const auto& t : tracks) {
                                keys.push_back(t.ratingKey);
                            }
                            Playlist result;
                            if (client.createPlaylistWithItems(name, keys, result)) {
                                brls::sync([name]() {
                                    brls::Application::notify("Created playlist: " + name);
                                });
                            } else {
                                brls::sync([]() {
                                    brls::Application::notify("Failed to create playlist");
                                });
                            }
                        });
                    }, "New Playlist", "Enter playlist name", 128, "");
                    return true;
                }});

                // Existing playlists
                for (const auto& pl : playlists) {
                    if (pl.smart) continue;  // Can't add to smart playlists
                    Playlist capturedPl = pl;
                    plRows.push_back({ "format-list-group.png", pl.title, "", false, false,
                        [capturedPl, tracks](brls::View*) {
                        asyncRun([capturedPl, tracks]() {
                            PlexClient& client = PlexClient::getInstance();
                            std::vector<std::string> keys;
                            for (const auto& t : tracks) {
                                keys.push_back(t.ratingKey);
                            }
                            if (client.addToPlaylist(capturedPl.ratingKey, keys)) {
                                brls::sync([capturedPl]() {
                                    brls::Application::notify("Added to " + capturedPl.title);
                                });
                            } else {
                                brls::sync([]() {
                                    brls::Application::notify("Failed to add to playlist");
                                });
                            }
                        });
                        return true;
                    }});
                }

                plRows.push_back({ "cross.png", "Cancel", "", false, true,
                    [](brls::View*) {
                    return true;
                }});

                MediaDetailView::showOptionsPopover(anchor, "ALBUM", "Add to Playlist", std::move(plRows));
            });
        });
        return true;
    }});

    rows.push_back({ "download.png", "Download Album", "", false, false,
        [this, capturedAlbum](brls::View*) {
        // Re-use existing download logic
        m_item = capturedAlbum;
        downloadAll();
        return true;
    }});

    rows.push_back({ "cross.png", "Cancel", "", false, true,
        [](brls::View*) {
        return true;
    }});

    showOptionsPopover(anchor, "ALBUM", album.title, std::move(rows));
}

void MediaDetailView::showMovieContextMenu(const MediaItem& movie) {
    showMovieContextMenuStatic(movie);
}

void MediaDetailView::showShowContextMenu(const MediaItem& show) {
    showShowContextMenuStatic(show);
}


void MediaDetailView::showEpisodeContextMenu(const MediaItem& episode) {
    brls::Logger::info("showEpisodeContextMenu called for {}", episode.title);
    brls::View* anchor = brls::Application::getCurrentFocus();

    MediaItem capturedEpisode = episode;
    const bool hasResume = episode.viewOffset > 0;

    std::vector<OptionRow> rows;

    // Restart button (primary only when there's no Resume row)
    rows.push_back({ "refresh.png", "Restart", "", !hasResume, false,
        [capturedEpisode](brls::View*) {
        // Mark as unwatched first to reset progress, then play
        PlexClient::getInstance().markAsUnwatched(capturedEpisode.ratingKey);
        Application::getInstance().pushPlayerActivity(capturedEpisode.ratingKey);
        return true;
    }});

    // Resume button (only if there's a view offset)
    if (episode.viewOffset > 0) {
        int totalSec = episode.viewOffset / 1000;
        int hours = totalSec / 3600;
        int minutes = (totalSec % 3600) / 60;
        char resumeStr[64];
        if (hours > 0) {
            snprintf(resumeStr, sizeof(resumeStr), "Resume from %dh %dm", hours, minutes);
        } else {
            snprintf(resumeStr, sizeof(resumeStr), "Resume from %dm", minutes);
        }
        rows.push_back({ "play.png", resumeStr, "", true, false,
            [capturedEpisode](brls::View*) {
            Application::getInstance().pushPlayerActivity(capturedEpisode.ratingKey);
            return true;
        }});
    }

    // Download
    rows.push_back({ "download.png", "Download", "", false, false,
        [capturedEpisode](brls::View*) {
        if (DownloadsManager::getInstance().isDownloaded(capturedEpisode.ratingKey)) {
            brls::Application::notify("Already downloaded");
            return true;
        }
        // Fetch full details and queue download
        asyncRun([capturedEpisode]() {
            PlexClient& client = PlexClient::getInstance();
            MediaItem fullItem;
            if (client.fetchMediaDetails(capturedEpisode.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                bool queued = DownloadsManager::getInstance().queueDownload(
                    fullItem.ratingKey, fullItem.title, fullItem.partPath,
                    fullItem.duration, "episode", "", 0, 0,
                    fullItem.thumb);
                brls::sync([queued, fullItem]() {
                    if (queued) {
                        DownloadsManager::getInstance().startDownloads();
                        brls::Application::notify("Downloading: " + fullItem.title);
                    } else {
                        brls::Application::notify("Failed to queue download");
                    }
                });
            } else {
                brls::sync([]() {
                    brls::Application::notify("Could not get download info");
                });
            }
        });
        return true;
    }});

    // Mark as watched/unwatched
    if (episode.watched) {
        rows.push_back({ "hide.png", "Mark as Unwatched", "", false, false,
            [capturedEpisode](brls::View*) {
            asyncRun([capturedEpisode]() {
                PlexClient::getInstance().markAsUnwatched(capturedEpisode.ratingKey);
                brls::sync([]() {
                    brls::Application::notify("Marked as unwatched");
                });
            });
            return true;
        }});
    } else {
        rows.push_back({ "check-circle.png", "Mark as Watched", "", false, false,
            [capturedEpisode](brls::View*) {
            asyncRun([capturedEpisode]() {
                PlexClient::getInstance().markAsWatched(capturedEpisode.ratingKey);
                brls::sync([]() {
                    brls::Application::notify("Marked as watched");
                });
            });
            return true;
        }});
    }

    rows.push_back({ "cross.png", "Cancel", "", false, true,
        [](brls::View*) {
        return true;
    }});

    // Context line "S{season} · E{episode}" from the numbers already on the item.
    std::string contextLine;
    if (episode.parentIndex > 0 || episode.index > 0) {
        contextLine = "S" + std::to_string(episode.parentIndex) +
                      " \xC2\xB7 E" + std::to_string(episode.index);
    }
    showOptionsPopover(anchor, contextLine, episode.title, std::move(rows));
}
void MediaDetailView::showMovieContextMenuStatic(const MediaItem& movie) {
    brls::View* anchor = brls::Application::getCurrentFocus();

    MediaItem capturedMovie = movie;
    const bool hasResume = movie.viewOffset > 0;

    std::vector<OptionRow> rows;

    // Restart button (primary only when there's no Resume row)
    rows.push_back({ "refresh.png", "Restart", "", !hasResume, false,
        [capturedMovie](brls::View*) {
        // Mark as unwatched first to reset progress, then play
        PlexClient::getInstance().markAsUnwatched(capturedMovie.ratingKey);
        Application::getInstance().pushPlayerActivity(capturedMovie.ratingKey);
        return true;
    }});

    // Resume button (only if there's a view offset)
    if (movie.viewOffset > 0) {
        int totalSec = movie.viewOffset / 1000;
        int hours = totalSec / 3600;
        int minutes = (totalSec % 3600) / 60;
        char resumeStr[64];
        if (hours > 0) {
            snprintf(resumeStr, sizeof(resumeStr), "Resume from %dh %dm", hours, minutes);
        } else {
            snprintf(resumeStr, sizeof(resumeStr), "Resume from %dm", minutes);
        }
        rows.push_back({ "play.png", resumeStr, "", true, false,
            [capturedMovie](brls::View*) {
            Application::getInstance().pushPlayerActivity(capturedMovie.ratingKey);
            return true;
        }});
    }

    // Download
    rows.push_back({ "download.png", "Download", "", false, false,
        [capturedMovie](brls::View*) {
        if (DownloadsManager::getInstance().isDownloaded(capturedMovie.ratingKey)) {
            brls::Application::notify("Already downloaded");
            return true;
        }
        // Fetch full details and queue download
        asyncRun([capturedMovie]() {
            PlexClient& client = PlexClient::getInstance();
            MediaItem fullItem;
            if (client.fetchMediaDetails(capturedMovie.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                bool queued = DownloadsManager::getInstance().queueDownload(
                    fullItem.ratingKey, fullItem.title, fullItem.partPath,
                    fullItem.duration, "movie", "", 0, 0,
                    fullItem.thumb);
                brls::sync([queued, fullItem]() {
                    if (queued) {
                        DownloadsManager::getInstance().startDownloads();
                        brls::Application::notify("Downloading: " + fullItem.title);
                    } else {
                        brls::Application::notify("Failed to queue download");
                    }
                });
            } else {
                brls::sync([]() {
                    brls::Application::notify("Could not get download info");
                });
            }
        });
        return true;
    }});

    // Mark as watched/unwatched
    if (movie.watched) {
        rows.push_back({ "hide.png", "Mark as Unwatched", "", false, false,
            [capturedMovie](brls::View*) {
            asyncRun([capturedMovie]() {
                PlexClient::getInstance().markAsUnwatched(capturedMovie.ratingKey);
                brls::sync([]() {
                    brls::Application::notify("Marked as unwatched");
                });
            });
            return true;
        }});
    } else {
        rows.push_back({ "check-circle.png", "Mark as Watched", "", false, false,
            [capturedMovie](brls::View*) {
            asyncRun([capturedMovie]() {
                PlexClient::getInstance().markAsWatched(capturedMovie.ratingKey);
                brls::sync([]() {
                    brls::Application::notify("Marked as watched");
                });
            });
            return true;
        }});
    }

    rows.push_back({ "cross.png", "Cancel", "", false, true,
        [](brls::View*) {
        return true;
    }});

    showOptionsPopover(anchor, "MOVIE", movie.title, std::move(rows));
}

void MediaDetailView::showShowContextMenuStatic(const MediaItem& show) {
    brls::View* anchor = brls::Application::getCurrentFocus();

    MediaItem capturedShow = show;
    const bool hasResume = (show.viewOffset > 0 || show.viewedLeafCount > 0);

    std::vector<OptionRow> rows;

    // Restart (play first episode of first season) — primary only when no Resume
    rows.push_back({ "refresh.png", "Restart (S01E01)", "", !hasResume, false,
        [capturedShow](brls::View*) {
        asyncRun([capturedShow]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> seasons;
            if (client.fetchChildren(capturedShow.ratingKey, seasons) && !seasons.empty()) {
                std::vector<MediaItem> episodes;
                if (client.fetchChildren(seasons[0].ratingKey, episodes) && !episodes.empty()) {
                    brls::sync([episodes]() {
                        Application::getInstance().pushPlayerActivity(episodes[0].ratingKey);
                    });
                }
            }
        });
        return true;
    }});

    // Resume (find the next unwatched/in-progress episode)
    if (show.viewOffset > 0 || show.viewedLeafCount > 0) {
        rows.push_back({ "play.png", "Resume", "", true, false,
            [capturedShow](brls::View*) {
            asyncRun([capturedShow]() {
                PlexClient& client = PlexClient::getInstance();
                std::vector<MediaItem> seasons;
                if (!client.fetchChildren(capturedShow.ratingKey, seasons) || seasons.empty()) return;

                // Find the first in-progress or unwatched episode
                for (const auto& season : seasons) {
                    std::vector<MediaItem> episodes;
                    if (!client.fetchChildren(season.ratingKey, episodes)) continue;
                    for (const auto& ep : episodes) {
                        // Episode with viewOffset = in-progress, play it
                        if (ep.viewOffset > 0) {
                            int totalSec = ep.viewOffset / 1000;
                            int minLeft = ((ep.duration - ep.viewOffset) / 1000) / 60;
                            char info[64];
                            snprintf(info, sizeof(info), "S%02dE%02d - %dm left",
                                     ep.parentIndex, ep.index, minLeft);
                            std::string infoStr = info;
                            brls::sync([ep, infoStr]() {
                                brls::Application::notify("Resuming " + infoStr);
                                Application::getInstance().pushPlayerActivity(ep.ratingKey);
                            });
                            return;
                        }
                        // First unwatched episode
                        if (!ep.watched) {
                            char info[64];
                            snprintf(info, sizeof(info), "S%02dE%02d",
                                     ep.parentIndex, ep.index);
                            std::string infoStr = info;
                            brls::sync([ep, infoStr]() {
                                brls::Application::notify("Playing " + infoStr);
                                Application::getInstance().pushPlayerActivity(ep.ratingKey);
                            });
                            return;
                        }
                    }
                }
                // All watched - play first episode
                brls::sync([]() {
                    brls::Application::notify("All episodes watched, restarting");
                });
                std::vector<MediaItem> firstEps;
                if (client.fetchChildren(seasons[0].ratingKey, firstEps) && !firstEps.empty()) {
                    brls::sync([firstEps]() {
                        Application::getInstance().pushPlayerActivity(firstEps[0].ratingKey);
                    });
                }
            });
            return true;
        }});
    }

    // Download options
    rows.push_back({ "download.png", "Download Entire Show", "", false, false,
        [capturedShow](brls::View*) {
        // Use the downloadAll pattern from existing code
        asyncRun([capturedShow]() {
            PlexClient& client = PlexClient::getInstance();
            auto& mgr = DownloadsManager::getInstance();
            std::vector<MediaItem> seasons;
            int queued = 0;
            int skipped = 0;
            if (client.fetchChildren(capturedShow.ratingKey, seasons)) {
                for (const auto& season : seasons) {
                    std::vector<MediaItem> episodes;
                    if (client.fetchChildren(season.ratingKey, episodes)) {
                        for (const auto& ep : episodes) {
                            if (mgr.isDownloaded(ep.ratingKey) ||
                                mgr.getDownload(ep.ratingKey) != nullptr) {
                                skipped++;
                                continue;
                            }
                            MediaItem fullItem;
                            if (client.fetchMediaDetails(ep.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                                if (mgr.queueDownload(
                                    fullItem.ratingKey, fullItem.title, fullItem.partPath,
                                    fullItem.duration, "episode", capturedShow.title,
                                    fullItem.parentIndex, fullItem.index,
                                    fullItem.grandparentThumb.empty() ? capturedShow.thumb : fullItem.grandparentThumb,
                                    DownloadGroupType::SHOW, capturedShow.ratingKey,
                                    capturedShow.title, capturedShow.thumb)) {
                                    queued++;
                                }
                            }
                        }
                    }
                }
            }
            mgr.startDownloads();
            brls::sync([queued, skipped]() {
                std::string msg = "Queued " + std::to_string(queued) + " episodes";
                if (skipped > 0) msg += " (" + std::to_string(skipped) + " already downloaded)";
                brls::Application::notify(msg);
            });
        });
        return true;
    }});

    rows.push_back({ "download.png", "Download All Unwatched", "", false, false,
        [capturedShow](brls::View*) {
        asyncRun([capturedShow]() {
            PlexClient& client = PlexClient::getInstance();
            auto& mgr = DownloadsManager::getInstance();
            std::vector<MediaItem> seasons;
            int queued = 0;
            int skipped = 0;
            if (client.fetchChildren(capturedShow.ratingKey, seasons)) {
                for (const auto& season : seasons) {
                    std::vector<MediaItem> episodes;
                    if (client.fetchChildren(season.ratingKey, episodes)) {
                        for (const auto& ep : episodes) {
                            if (!ep.watched && ep.viewOffset == 0) {
                                if (mgr.isDownloaded(ep.ratingKey) ||
                                    mgr.getDownload(ep.ratingKey) != nullptr) {
                                    skipped++;
                                    continue;
                                }
                                MediaItem fullItem;
                                if (client.fetchMediaDetails(ep.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                                    if (mgr.queueDownload(
                                        fullItem.ratingKey, fullItem.title, fullItem.partPath,
                                        fullItem.duration, "episode", capturedShow.title,
                                        fullItem.parentIndex, fullItem.index,
                                        fullItem.grandparentThumb.empty() ? capturedShow.thumb : fullItem.grandparentThumb,
                                        DownloadGroupType::SHOW, capturedShow.ratingKey,
                                        capturedShow.title, capturedShow.thumb)) {
                                        queued++;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            mgr.startDownloads();
            brls::sync([queued, skipped]() {
                std::string msg = "Queued " + std::to_string(queued) + " unwatched episodes";
                if (skipped > 0) msg += " (" + std::to_string(skipped) + " already downloaded)";
                brls::Application::notify(msg);
            });
        });
        return true;
    }});

    // Per-season download submenu (chevron → sub-popover)
    rows.push_back({ "right.png", "Download by Season...", "", false, false,
        [capturedShow, anchor](brls::View*) {
        // Show a second popover with season options
        asyncRun([capturedShow, anchor]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> seasons;
            if (!client.fetchChildren(capturedShow.ratingKey, seasons) || seasons.empty()) {
                brls::sync([]() {
                    brls::Application::notify("No seasons found");
                });
                return;
            }

            brls::sync([capturedShow, seasons, anchor]() {
                std::vector<OptionRow> seasonRows;

                for (const auto& season : seasons) {
                    MediaItem capturedSeason = season;
                    std::string showTitle = capturedShow.title;
                    std::string showThumb = capturedShow.thumb;
                    std::string showKey = capturedShow.ratingKey;
                    seasonRows.push_back({ "download.png", season.title, "", false, false,
                        [capturedSeason, showTitle, showThumb, showKey](brls::View*) {
                        asyncRun([capturedSeason, showTitle, showThumb, showKey]() {
                            PlexClient& client = PlexClient::getInstance();
                            auto& mgr = DownloadsManager::getInstance();
                            std::vector<MediaItem> episodes;
                            int queued = 0;
                            int skipped = 0;
                            if (client.fetchChildren(capturedSeason.ratingKey, episodes)) {
                                for (const auto& ep : episodes) {
                                    if (mgr.isDownloaded(ep.ratingKey) ||
                                        mgr.getDownload(ep.ratingKey) != nullptr) {
                                        skipped++;
                                        continue;
                                    }
                                    MediaItem fullItem;
                                    if (client.fetchMediaDetails(ep.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                                        if (mgr.queueDownload(
                                            fullItem.ratingKey, fullItem.title, fullItem.partPath,
                                            fullItem.duration, "episode", showTitle,
                                            fullItem.parentIndex, fullItem.index,
                                            fullItem.grandparentThumb.empty() ? showThumb : fullItem.grandparentThumb,
                                            DownloadGroupType::SHOW, showKey,
                                            showTitle, showThumb)) {
                                            queued++;
                                        }
                                    }
                                }
                            }
                            mgr.startDownloads();
                            brls::sync([queued, skipped, capturedSeason]() {
                                std::string msg = "Queued " + std::to_string(queued) +
                                    " episodes from " + capturedSeason.title;
                                if (skipped > 0) msg += " (" + std::to_string(skipped) + " already downloaded)";
                                brls::Application::notify(msg);
                            });
                        });
                        return true;
                    }});
                }

                seasonRows.push_back({ "cross.png", "Cancel", "", false, true,
                    [](brls::View*) {
                    return true;
                }});

                MediaDetailView::showOptionsPopover(anchor, "SHOW", "Download Season", std::move(seasonRows));
            });
        });
        return true;
    }});

    // Mark as watched/unwatched
    if (show.watched || (show.leafCount > 0 && show.viewedLeafCount == show.leafCount)) {
        rows.push_back({ "hide.png", "Mark as Unwatched", "", false, false,
            [capturedShow](brls::View*) {
            asyncRun([capturedShow]() {
                PlexClient::getInstance().markAsUnwatched(capturedShow.ratingKey);
                brls::sync([]() {
                    brls::Application::notify("Marked as unwatched");
                });
            });
            return true;
        }});
    } else {
        rows.push_back({ "check-circle.png", "Mark as Watched", "", false, false,
            [capturedShow](brls::View*) {
            asyncRun([capturedShow]() {
                PlexClient::getInstance().markAsWatched(capturedShow.ratingKey);
                brls::sync([]() {
                    brls::Application::notify("Marked as watched");
                });
            });
            return true;
        }});
    }

    rows.push_back({ "cross.png", "Cancel", "", false, true,
        [](brls::View*) {
        return true;
    }});

    showOptionsPopover(anchor, "SHOW", show.title, std::move(rows));
}

void MediaDetailView::showSeasonContextMenuStatic(const MediaItem& season) {
    brls::View* anchor = brls::Application::getCurrentFocus();

    MediaItem capturedSeason = season;

    std::vector<OptionRow> rows;

    // Resume (find next in-progress or unwatched episode)
    rows.push_back({ "play.png", "Resume", "", true, false,
        [capturedSeason](brls::View*) {
        asyncRun([capturedSeason]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> episodes;
            if (!client.fetchChildren(capturedSeason.ratingKey, episodes) || episodes.empty()) {
                brls::sync([]() { brls::Application::notify("No episodes found"); });
                return;
            }
            // Find in-progress episode first
            for (const auto& ep : episodes) {
                if (ep.viewOffset > 0) {
                    int minLeft = ((ep.duration - ep.viewOffset) / 1000) / 60;
                    char info[64];
                    snprintf(info, sizeof(info), "S%02dE%02d - %dm left",
                             ep.parentIndex, ep.index, minLeft);
                    std::string infoStr = info;
                    brls::sync([ep, infoStr]() {
                        brls::Application::notify("Resuming " + infoStr);
                        Application::getInstance().pushPlayerActivity(ep.ratingKey);
                    });
                    return;
                }
            }
            // Find first unwatched
            for (const auto& ep : episodes) {
                if (!ep.watched) {
                    char info[64];
                    snprintf(info, sizeof(info), "S%02dE%02d", ep.parentIndex, ep.index);
                    std::string infoStr = info;
                    brls::sync([ep, infoStr]() {
                        brls::Application::notify("Playing " + infoStr);
                        Application::getInstance().pushPlayerActivity(ep.ratingKey);
                    });
                    return;
                }
            }
            // All watched - play first
            brls::sync([episodes]() {
                brls::Application::notify("All watched, restarting season");
                Application::getInstance().pushPlayerActivity(episodes[0].ratingKey);
            });
        });
        return true;
    }});

    // Download whole season
    rows.push_back({ "download.png", "Download Whole Season", "", false, false,
        [capturedSeason](brls::View*) {
        asyncRun([capturedSeason]() {
            PlexClient& client = PlexClient::getInstance();
            auto& mgr = DownloadsManager::getInstance();
            std::vector<MediaItem> episodes;
            int queued = 0;
            int skipped = 0;
            std::string showTitle = capturedSeason.parentTitle.empty() ? capturedSeason.title : capturedSeason.parentTitle;
            if (client.fetchChildren(capturedSeason.ratingKey, episodes)) {
                for (const auto& ep : episodes) {
                    if (mgr.isDownloaded(ep.ratingKey) ||
                        mgr.getDownload(ep.ratingKey) != nullptr) {
                        skipped++;
                        continue;
                    }
                    MediaItem fullItem;
                    if (client.fetchMediaDetails(ep.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                        if (mgr.queueDownload(
                            fullItem.ratingKey, fullItem.title, fullItem.partPath,
                            fullItem.duration, "episode", showTitle,
                            fullItem.parentIndex, fullItem.index,
                            fullItem.grandparentThumb.empty() ? capturedSeason.thumb : fullItem.grandparentThumb,
                            DownloadGroupType::SHOW, capturedSeason.parentRatingKey.empty() ? capturedSeason.ratingKey : capturedSeason.parentRatingKey,
                            showTitle, capturedSeason.parentThumb.empty() ? capturedSeason.thumb : capturedSeason.parentThumb)) {
                            queued++;
                        }
                    }
                }
            }
            mgr.startDownloads();
            brls::sync([queued, skipped]() {
                std::string msg = "Queued " + std::to_string(queued) + " episodes";
                if (skipped > 0) msg += " (" + std::to_string(skipped) + " already downloaded)";
                brls::Application::notify(msg);
            });
        });
        return true;
    }});

    // Download all unwatched
    rows.push_back({ "download.png", "Download All Unwatched", "", false, false,
        [capturedSeason](brls::View*) {
        asyncRun([capturedSeason]() {
            PlexClient& client = PlexClient::getInstance();
            auto& mgr = DownloadsManager::getInstance();
            std::vector<MediaItem> episodes;
            int queued = 0;
            int skipped = 0;
            std::string showTitle = capturedSeason.parentTitle.empty() ? capturedSeason.title : capturedSeason.parentTitle;
            if (client.fetchChildren(capturedSeason.ratingKey, episodes)) {
                for (const auto& ep : episodes) {
                    if (!ep.watched && ep.viewOffset == 0) {
                        if (mgr.isDownloaded(ep.ratingKey) ||
                            mgr.getDownload(ep.ratingKey) != nullptr) {
                            skipped++;
                            continue;
                        }
                        MediaItem fullItem;
                        if (client.fetchMediaDetails(ep.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                            if (mgr.queueDownload(
                                fullItem.ratingKey, fullItem.title, fullItem.partPath,
                                fullItem.duration, "episode", showTitle,
                                fullItem.parentIndex, fullItem.index,
                                fullItem.grandparentThumb.empty() ? capturedSeason.thumb : fullItem.grandparentThumb,
                                DownloadGroupType::SHOW, capturedSeason.parentRatingKey.empty() ? capturedSeason.ratingKey : capturedSeason.parentRatingKey,
                                showTitle, capturedSeason.parentThumb.empty() ? capturedSeason.thumb : capturedSeason.parentThumb)) {
                                queued++;
                            }
                        }
                    }
                }
            }
            mgr.startDownloads();
            brls::sync([queued, skipped]() {
                std::string msg = "Queued " + std::to_string(queued) + " unwatched episodes";
                if (skipped > 0) msg += " (" + std::to_string(skipped) + " already downloaded)";
                brls::Application::notify(msg);
            });
        });
        return true;
    }});

    // Mark as watched/unwatched
    if (season.watched || (season.leafCount > 0 && season.viewedLeafCount == season.leafCount)) {
        rows.push_back({ "hide.png", "Mark as Unwatched", "", false, false,
            [capturedSeason](brls::View*) {
            asyncRun([capturedSeason]() {
                PlexClient::getInstance().markAsUnwatched(capturedSeason.ratingKey);
                brls::sync([]() {
                    brls::Application::notify("Marked as unwatched");
                });
            });
            return true;
        }});
    } else {
        rows.push_back({ "check-circle.png", "Mark as Watched", "", false, false,
            [capturedSeason](brls::View*) {
            asyncRun([capturedSeason]() {
                PlexClient::getInstance().markAsWatched(capturedSeason.ratingKey);
                brls::sync([]() {
                    brls::Application::notify("Marked as watched");
                });
            });
            return true;
        }});
    }

    rows.push_back({ "cross.png", "Cancel", "", false, true,
        [](brls::View*) {
        return true;
    }});

    std::string contextLine = season.index > 0 ? ("SEASON " + std::to_string(season.index)) : "SEASON";
    showOptionsPopover(anchor, contextLine, season.title, std::move(rows));
}

void MediaDetailView::showArtistContextMenuStatic(const MediaItem& artist) {
    brls::View* anchor = brls::Application::getCurrentFocus();

    MediaItem capturedArtist = artist;

    std::vector<OptionRow> rows;

    // Shuffle Artist - add all tracks to queue in random order
    rows.push_back({ "shuffle-variant.png", "Shuffle Artist", "", false, false,
        [capturedArtist](brls::View*) {
        asyncRun([capturedArtist]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> albums;
            std::vector<MediaItem> allTracks;

            if (client.fetchChildren(capturedArtist.ratingKey, albums)) {
                for (const auto& album : albums) {
                    std::vector<MediaItem> tracks;
                    if (client.fetchChildren(album.ratingKey, tracks)) {
                        for (auto& track : tracks) {
                            allTracks.push_back(track);
                        }
                    }
                }
            }

            if (allTracks.empty()) {
                brls::sync([]() { brls::Application::notify("No tracks found"); });
                return;
            }

            // Shuffle
            srand((unsigned)time(nullptr));
            for (size_t i = allTracks.size() - 1; i > 0; i--) {
                size_t j = rand() % (i + 1);
                std::swap(allTracks[i], allTracks[j]);
            }

            brls::sync([allTracks]() {
                auto* playerActivity = PlayerActivity::createWithQueue(allTracks, 0);
                brls::Application::pushActivity(playerActivity);
            });
        });
        return true;
    }});

    // Play All (in order)
    rows.push_back({ "play.png", "Play All", "", true, false,
        [capturedArtist](brls::View*) {
        asyncRun([capturedArtist]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> albums;
            std::vector<MediaItem> allTracks;

            if (client.fetchChildren(capturedArtist.ratingKey, albums)) {
                for (const auto& album : albums) {
                    std::vector<MediaItem> tracks;
                    if (client.fetchChildren(album.ratingKey, tracks)) {
                        for (auto& track : tracks) {
                            allTracks.push_back(track);
                        }
                    }
                }
            }

            if (allTracks.empty()) {
                brls::sync([]() { brls::Application::notify("No tracks found"); });
                return;
            }

            brls::sync([allTracks]() {
                auto* playerActivity = PlayerActivity::createWithQueue(allTracks, 0);
                brls::Application::pushActivity(playerActivity);
            });
        });
        return true;
    }});

    // Download Artist
    rows.push_back({ "download.png", "Download Artist", "", false, false,
        [capturedArtist](brls::View*) {
        asyncRun([capturedArtist]() {
            PlexClient& client = PlexClient::getInstance();
            auto& mgr = DownloadsManager::getInstance();
            std::vector<MediaItem> albums;
            int queued = 0;
            int skipped = 0;

            if (client.fetchChildren(capturedArtist.ratingKey, albums)) {
                for (const auto& album : albums) {
                    std::vector<MediaItem> tracks;
                    if (client.fetchChildren(album.ratingKey, tracks)) {
                        for (const auto& track : tracks) {
                            if (mgr.isDownloaded(track.ratingKey) ||
                                mgr.getDownload(track.ratingKey) != nullptr) {
                                skipped++;
                                continue;
                            }
                            MediaItem fullItem;
                            if (client.fetchMediaDetails(track.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                                if (mgr.queueDownload(
                                    fullItem.ratingKey, fullItem.title, fullItem.partPath,
                                    fullItem.duration, "track",
                                    capturedArtist.title, 0, fullItem.index,
                                    fullItem.thumb,
                                    DownloadGroupType::ARTIST, capturedArtist.ratingKey,
                                    capturedArtist.title, capturedArtist.thumb,
                                    album.title)) {
                                    queued++;
                                }
                            }
                        }
                    }
                }
            }

            mgr.startDownloads();
            brls::sync([queued, skipped]() {
                std::string msg = "Queued " + std::to_string(queued) + " tracks";
                if (skipped > 0) msg += " (" + std::to_string(skipped) + " already downloaded)";
                brls::Application::notify(msg);
            });
        });
        return true;
    }});

    rows.push_back({ "cross.png", "Cancel", "", false, true,
        [](brls::View*) {
        return true;
    }});

    showOptionsPopover(anchor, "ARTIST", artist.title, std::move(rows));
}

void MediaDetailView::showAlbumContextMenuStatic(const MediaItem& album) {
    brls::View* anchor = brls::Application::getCurrentFocus();

    MediaItem capturedAlbum = album;

    std::vector<OptionRow> rows;

    rows.push_back({ "play.png", "Play Now (Clear Queue)", "", true, false,
        [capturedAlbum](brls::View*) {
        asyncRun([capturedAlbum]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> tracks;
            if (client.fetchChildren(capturedAlbum.ratingKey, tracks) && !tracks.empty()) {
                brls::sync([tracks]() {
                    auto* playerActivity = PlayerActivity::createWithQueue(tracks, 0);
                    brls::Application::pushActivity(playerActivity);
                });
            }
        });
        return true;
    }});

    rows.push_back({ "skip-next.png", "Play Next", "", false, false,
        [capturedAlbum](brls::View*) {
        asyncRun([capturedAlbum]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> tracks;
            if (client.fetchChildren(capturedAlbum.ratingKey, tracks)) {
                brls::sync([tracks]() {
                    MusicQueue& queue = MusicQueue::getInstance();
                    if (queue.isEmpty()) {
                        auto* playerActivity = PlayerActivity::createWithQueue(tracks, 0);
                        brls::Application::pushActivity(playerActivity);
                    } else {
                        for (int i = (int)tracks.size() - 1; i >= 0; i--) {
                            queue.insertTrackAfterCurrent(tracks[i]);
                        }
                        brls::Application::notify("Album queued next");
                    }
                });
            }
        });
        return true;
    }});

    rows.push_back({ "format-list-group.png", "Add to Bottom of Queue", "", false, false,
        [capturedAlbum](brls::View*) {
        asyncRun([capturedAlbum]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> tracks;
            if (client.fetchChildren(capturedAlbum.ratingKey, tracks)) {
                brls::sync([tracks]() {
                    MusicQueue& queue = MusicQueue::getInstance();
                    if (queue.isEmpty()) {
                        auto* playerActivity = PlayerActivity::createWithQueue(tracks, 0);
                        brls::Application::pushActivity(playerActivity);
                    } else {
                        queue.addTracks(tracks);
                        brls::Application::notify("Album added to queue");
                    }
                });
            }
        });
        return true;
    }});

    // Download Album — home-menu albums reach this static menu, which had
    // no download action. Queue every track (mirrors Download Artist).
    rows.push_back({ "download.png", "Download Album", "", false, false,
        [capturedAlbum](brls::View*) {
        asyncRun([capturedAlbum]() {
            PlexClient& client = PlexClient::getInstance();
            auto& mgr = DownloadsManager::getInstance();
            std::vector<MediaItem> tracks;
            int queued = 0;
            int skipped = 0;

            if (client.fetchChildren(capturedAlbum.ratingKey, tracks)) {
                for (const auto& track : tracks) {
                    if (mgr.isDownloaded(track.ratingKey) ||
                        mgr.getDownload(track.ratingKey) != nullptr) {
                        skipped++;
                        continue;
                    }
                    MediaItem fullItem;
                    if (client.fetchMediaDetails(track.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                        if (mgr.queueDownload(
                            fullItem.ratingKey, fullItem.title, fullItem.partPath,
                            fullItem.duration, "track",
                            capturedAlbum.title, fullItem.parentIndex, fullItem.index,
                            fullItem.thumb,
                            DownloadGroupType::ALBUM, capturedAlbum.ratingKey,
                            capturedAlbum.title, capturedAlbum.thumb,
                            fullItem.parentTitle)) {
                            queued++;
                        }
                    }
                }
            }

            mgr.startDownloads();
            brls::sync([queued, skipped]() {
                std::string msg = "Queued " + std::to_string(queued) + " tracks";
                if (skipped > 0) msg += " (" + std::to_string(skipped) + " already downloaded)";
                brls::Application::notify(msg);
            });
        });
        return true;
    }});

    rows.push_back({ "cross.png", "Cancel", "", false, true,
        [](brls::View*) {
        return true;
    }});

    showOptionsPopover(anchor, "ALBUM", album.title, std::move(rows));
}

void MediaDetailView::performTrackActionStatic(const MediaItem& track) {
    TrackDefaultAction action = Application::getInstance().getSettings().trackDefaultAction;

    if (action == TrackDefaultAction::ASK_EACH_TIME) {
        // Show a simplified action dialog (no playlist option without MediaDetailView context)
        auto* dialog = new brls::Dialog("Choose Action");

        auto* optionsBox = new brls::Box();
        optionsBox->setAxis(brls::Axis::COLUMN);
        optionsBox->setPadding(20);

        auto addDialogButton = [&optionsBox](const std::string& text, std::function<bool(brls::View*)> action) {
            auto* btn = new brls::Button();
            btn->setText(text);
            btn->setHeight(44);
            btn->setMarginBottom(10);
            btn->registerClickAction(action);
            btn->addGestureRecognizer(new brls::TapGestureRecognizer(btn));
            optionsBox->addView(btn);
        };

        MediaItem capturedTrack = track;

        addDialogButton("Play Now (Clear Queue)", [capturedTrack, dialog](brls::View*) {
            dialog->dismiss();
            std::vector<MediaItem> single = {capturedTrack};
            auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
            brls::Application::pushActivity(playerActivity);
            return true;
        });

        addDialogButton("Play Next", [capturedTrack, dialog](brls::View*) {
            dialog->dismiss();
            MusicQueue& queue = MusicQueue::getInstance();
            if (queue.isEmpty()) {
                std::vector<MediaItem> single = {capturedTrack};
                auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
                brls::Application::pushActivity(playerActivity);
            } else {
                queue.insertTrackAfterCurrent(capturedTrack);
                brls::Application::notify("Playing next: " + capturedTrack.title);
            }
            return true;
        });

        addDialogButton("Add to Bottom of Queue", [capturedTrack, dialog](brls::View*) {
            dialog->dismiss();
            MusicQueue& queue = MusicQueue::getInstance();
            if (queue.isEmpty()) {
                std::vector<MediaItem> single = {capturedTrack};
                auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
                brls::Application::pushActivity(playerActivity);
            } else {
                queue.addTrack(capturedTrack);
                brls::Application::notify("Added to queue: " + capturedTrack.title);
            }
            return true;
        });

        addDialogButton("Cancel", [dialog](brls::View*) {
            dialog->dismiss();
            return true;
        });

        dialog->addView(optionsBox);
        dialog->registerAction("Back", brls::ControllerButton::BUTTON_B, [dialog](brls::View*) {
            dialog->dismiss();
            return true;
        });
        brls::Application::pushActivity(new brls::Activity(dialog));
        return;
    }

    MusicQueue& queue = MusicQueue::getInstance();

    switch (action) {
        case TrackDefaultAction::PLAY_NEXT:
            if (queue.isEmpty()) {
                std::vector<MediaItem> single = {track};
                auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
                brls::Application::pushActivity(playerActivity);
            } else {
                queue.insertTrackAfterCurrent(track);
                brls::Application::notify("Playing next: " + track.title);
            }
            break;

        case TrackDefaultAction::PLAY_NOW_REPLACE:
            if (queue.isEmpty()) {
                std::vector<MediaItem> single = {track};
                auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
                brls::Application::pushActivity(playerActivity);
            } else {
                queue.insertTrackAfterCurrent(track);
                if (queue.playNext()) {
                    brls::Application::notify("Now playing: " + track.title);
                }
            }
            break;

        case TrackDefaultAction::ADD_TO_BOTTOM:
            if (queue.isEmpty()) {
                std::vector<MediaItem> single = {track};
                auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
                brls::Application::pushActivity(playerActivity);
            } else {
                queue.addTrack(track);
                brls::Application::notify("Added to queue: " + track.title);
            }
            break;

        case TrackDefaultAction::PLAY_NOW_CLEAR:
        default:
            {
                std::vector<MediaItem> single = {track};
                auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
                brls::Application::pushActivity(playerActivity);
            }
            break;
    }
}

// Always-on options menu for a single track (START / long-press), as
// opposed to performTrackActionStatic which follows the default-action
// setting (and may just play without a menu). Mirrors the member
// showTrackActionDialog's actions, minus the playlist flow that needs a
// MediaDetailView instance.
void MediaDetailView::showTrackContextMenuStatic(const MediaItem& track) {
    brls::View* anchor = brls::Application::getCurrentFocus();

    MediaItem capturedTrack = track;

    std::vector<OptionRow> rows;

    rows.push_back({ "play.png", "Play Now (Clear Queue)", "", true, false,
        [capturedTrack](brls::View*) {
        std::vector<MediaItem> single = {capturedTrack};
        auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
        brls::Application::pushActivity(playerActivity);
        return true;
    }});

    rows.push_back({ "skip-next.png", "Play Next", "", false, false,
        [capturedTrack](brls::View*) {
        MusicQueue& queue = MusicQueue::getInstance();
        if (queue.isEmpty()) {
            std::vector<MediaItem> single = {capturedTrack};
            auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
            brls::Application::pushActivity(playerActivity);
        } else {
            queue.insertTrackAfterCurrent(capturedTrack);
            brls::Application::notify("Playing next: " + capturedTrack.title);
        }
        return true;
    }});

    rows.push_back({ "format-list-group.png", "Add to Bottom of Queue", "", false, false,
        [capturedTrack](brls::View*) {
        MusicQueue& queue = MusicQueue::getInstance();
        if (queue.isEmpty()) {
            std::vector<MediaItem> single = {capturedTrack};
            auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
            brls::Application::pushActivity(playerActivity);
        } else {
            queue.addTrack(capturedTrack);
            brls::Application::notify("Added to queue: " + capturedTrack.title);
        }
        return true;
    }});

    rows.push_back({ "download.png", "Download", "", false, false,
        [capturedTrack](brls::View*) {
        if (DownloadsManager::getInstance().isDownloaded(capturedTrack.ratingKey)) {
            brls::Application::notify("Already downloaded");
            return true;
        }
        asyncRun([capturedTrack]() {
            PlexClient& client = PlexClient::getInstance();
            MediaItem fullItem;
            if (client.fetchMediaDetails(capturedTrack.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                // Artist as the display prefix ("Artist - Track"); pass 0/0
                // for season/episode so the row isn't labelled "(S01E01)" —
                // a track's disc/track numbers are not TV season/episode.
                bool queued = DownloadsManager::getInstance().queueDownload(
                    fullItem.ratingKey, fullItem.title, fullItem.partPath,
                    fullItem.duration, "track",
                    fullItem.grandparentTitle, 0, 0,
                    fullItem.thumb);
                brls::sync([queued, fullItem]() {
                    if (queued) {
                        DownloadsManager::getInstance().startDownloads();
                        brls::Application::notify("Downloading: " + fullItem.title);
                    } else {
                        brls::Application::notify("Failed to queue download");
                    }
                });
            } else {
                brls::sync([]() {
                    brls::Application::notify("Could not get download info");
                });
            }
        });
        return true;
    }});

    rows.push_back({ "cross.png", "Cancel", "", false, true,
        [](brls::View*) {
        return true;
    }});

    showOptionsPopover(anchor, "TRACK", track.title, std::move(rows));
}

void MediaDetailView::setupChildrenFocusTransfer() {
    bool hasChildren = m_childrenBox && !m_childrenBox->getChildren().empty();
    bool hasExtras = m_extrasBox && !m_extrasBox->getChildren().empty();

    if (!hasChildren && !hasExtras) return;

    // Determine the UP navigation target for children (seasons/episodes):
    // - If description exists, navigate UP to description label
    // - If description is empty, navigate UP to the BOTTOM-most leftBox
    //   button so the user lands on Mark Watched / Download / Resume /
    //   Play rather than jumping to the top of the column.
    brls::View* upTarget = nullptr;
    if (m_summaryLabel && m_summaryLabel->isFocusable() && !m_fullDescription.empty()) {
        upTarget = m_summaryLabel;
    } else {
        upTarget = m_markWatchedButton
            ? (brls::View*)m_markWatchedButton
            : (m_downloadButton
                ? (brls::View*)m_downloadButton
                : (m_resumeButton ? (brls::View*)m_resumeButton
                                  : (brls::View*)m_playButton));
    }

    // Description exists → also bridge the last leftBox button down into
    // the description so the leftBox chain ends in the same place the
    // user would naturally read next.
    if (m_summaryLabel && m_summaryLabel->isFocusable() && !m_fullDescription.empty() && m_markWatchedButton) {
        m_markWatchedButton->setCustomNavigationRoute(
            brls::FocusDirection::DOWN, m_summaryLabel);
    }

    if (hasChildren && upTarget) {
        for (auto* child : m_childrenBox->getChildren()) {
            child->setCustomNavigationRoute(brls::FocusDirection::UP, upTarget);
        }
    }

    // If description exists, set DOWN from description to first child (or extras if no children)
    if (m_summaryLabel && m_summaryLabel->isFocusable() && !m_fullDescription.empty()) {
        brls::View* downFromDesc = nullptr;
        if (hasChildren) {
            downFromDesc = m_childrenBox->getChildren().front();
        } else if (hasExtras) {
            downFromDesc = m_extrasBox->getChildren().front();
        }
        if (downFromDesc) {
            m_summaryLabel->setCustomNavigationRoute(brls::FocusDirection::DOWN, downFromDesc);
        }
    }

    // Set UP/DOWN navigation between children and extras
    if (hasChildren && hasExtras) {
        // DOWN from children goes to first extra
        brls::View* firstExtra = m_extrasBox->getChildren().front();
        for (auto* child : m_childrenBox->getChildren()) {
            child->setCustomNavigationRoute(brls::FocusDirection::DOWN, firstExtra);
        }

        // UP from extras goes to first child (seasons/episodes)
        brls::View* firstChild = m_childrenBox->getChildren().front();
        for (auto* extra : m_extrasBox->getChildren()) {
            extra->setCustomNavigationRoute(brls::FocusDirection::UP, firstChild);
        }
    } else if (hasExtras && !hasChildren) {
        // No children, extras UP goes to description/play button
        if (upTarget) {
            for (auto* extra : m_extrasBox->getChildren()) {
                extra->setCustomNavigationRoute(brls::FocusDirection::UP, upTarget);
            }
        }
    }

    // If description is empty, transfer initial focus to first media item
    brls::View* firstFocusable = nullptr;
    if (hasChildren) {
        firstFocusable = m_childrenBox->getChildren().front();
    } else if (hasExtras) {
        firstFocusable = m_extrasBox->getChildren().front();
    }

    if (firstFocusable) {
        // The description is non-focusable, so it can't bridge the leftBox
        // buttons down to the children/extras. Wire DOWN from the bottom
        // button straight onto the first item. (Play→Resume→Download→Watched
        // were chained in the constructor; only the bottom one needs this.)
        brls::View* lastLeftButton = m_markWatchedButton
            ? m_markWatchedButton
            : (m_downloadButton
                ? m_downloadButton
                : (m_resumeButton ? m_resumeButton : m_playButton));
        if (lastLeftButton) {
            lastLeftButton->setCustomNavigationRoute(
                brls::FocusDirection::DOWN, firstFocusable);
        }
        // Land focus on the first child/extra when there's no description to
        // read OR no left-column button to hold the default focus. The
        // description is display-only (non-focusable), so without this, a
        // button-less layout (e.g. some artist/show headers) would strand
        // focus on it. Layouts with a Play/etc. button keep their default.
        if (m_fullDescription.empty() || !lastLeftButton) {
            brls::Application::giveFocus(firstFocusable);
        }
    }
}

// ─── Mark Watched / Unwatched ──────────────────────────────────────────

void MediaDetailView::onToggleWatched() {
    if (!m_markWatchedButton) return;

    // Optimistic UI update so the press feels instant. If the server
    // call fails we flip the label, icon, and the cached watched bit
    // back to their prior state so the next refresh reflects truth.
    bool wasWatched = m_item.watched;
    m_item.watched = !wasWatched;
    m_markWatchedButton->setText(m_item.watched ? "Mark Unwatched" : "Mark Watched");
    if (m_markWatchedIcon) {
        m_markWatchedIcon->setImageFromRes(m_item.watched
            ? "icons/check-circle.png"
            : "icons/check-circle-outline.png");
    }

    std::string ratingKey = m_item.ratingKey;
    bool target = m_item.watched;  // what we want it to be on the server
    auto alive = m_alive;
    asyncRun([this, alive, ratingKey, target, wasWatched]() {
        PlexClient& client = PlexClient::getInstance();
        bool ok = target ? client.markAsWatched(ratingKey)
                         : client.markAsUnwatched(ratingKey);
        if (!ok) {
            brls::sync([this, alive, wasWatched]() {
                if (!alive->load()) return;
                // Roll the optimistic update back.
                m_item.watched = wasWatched;
                if (m_markWatchedButton) {
                    m_markWatchedButton->setText(m_item.watched ? "Mark Unwatched" : "Mark Watched");
                }
                if (m_markWatchedIcon) {
                    m_markWatchedIcon->setImageFromRes(m_item.watched
                        ? "icons/check-circle.png"
                        : "icons/check-circle-outline.png");
                }
                brls::Application::notify("Couldn't update watched state");
            });
        }
    });
}

// ─── Audio / Subtitle stream pickers ───────────────────────────────────

void MediaDetailView::loadStreams() {
    if (!m_audioRow && !m_subtitleRow) return;  // movie-only, see ctor

    std::string ratingKey = m_item.ratingKey;
    auto alive = m_alive;
    asyncRun([this, alive, ratingKey]() {
        std::vector<PlexStream> streams;
        int partId = 0;
        PlexClient& client = PlexClient::getInstance();
        if (!client.fetchStreams(ratingKey, streams, partId)) return;

        brls::sync([this, alive, streams, partId]() {
            if (!alive->load()) return;
            m_streams = streams;
            m_partId  = partId;
            updateStreamRowLabels();
        });
    });
}

// Pick the MDI surround-sound channel-layout glyph for an audio track. The
// channel count fixes the layout (2.0 / 3.1 / 5.1 / 7.1); an 8-channel track
// whose name reads "Atmos" or "5.1.2" gets the height-channel (5.1.2) glyph
// instead of 7.1. Mono / unknown fall back to the generic 2.0 glyph.
static std::string audioChannelIcon(int channels, const std::string& displayTitle) {
    std::string t = displayTitle;
    for (auto& c : t) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);   // lowercase
    const bool height = t.find("atmos") != std::string::npos ||
                        t.find("5.1.2") != std::string::npos;
    switch (channels) {
        case 2:  return "icons/surround-sound-2-0.png";
        case 4:  return "icons/surround-sound-3-1.png";
        case 6:  return "icons/surround-sound-5-1.png";
        case 8:  return height ? "icons/surround-sound-5-1-2.png"
                               : "icons/surround-sound-7-1.png";
        default: return "icons/surround-sound-2-0.png";
    }
}

void MediaDetailView::updateStreamRowLabels() {
    // Direction B action-row buttons: a leading icon conveys the type, so the
    // label is just the selected track — no "AUDIO:" / "SUBTITLES:" prefix and
    // no search hint. Prefer the bare language (bounded width) over Plex's
    // longer displayTitle so the four-button row never overflows.
    std::string audioLabel    = "Audio";
    std::string subtitleLabel = "Off";
    bool hasAudio = false;
    int  audioChannels = 0;          // selected audio track → surround icon
    std::string audioDisplay;        // selected audio displayTitle (Atmos check)
    for (const auto& s : m_streams) {
        if (s.streamType == 2) {
            hasAudio = true;
            if (s.selected) {
                audioLabel = !s.language.empty() ? s.language
                           : (s.displayTitle.empty() ? "Audio" : s.displayTitle);
                audioChannels = s.channels;
                audioDisplay  = s.displayTitle;
            }
        } else if (s.streamType == 3 || s.streamType == 4) {
            if (s.selected)
                subtitleLabel = !s.language.empty() ? s.language
                              : (s.displayTitle.empty() ? "On" : s.displayTitle);
        }
    }

    if (m_audioRow) {
        m_audioRow->setText(audioLabel);
        if (m_audioIcon)
            m_audioIcon->setImageFromRes(audioChannelIcon(audioChannels, audioDisplay));
        m_audioRow->setVisibility(hasAudio ? brls::Visibility::VISIBLE
                                           : brls::Visibility::GONE);
    }
    if (m_subtitleRow) {
        // Stays visible even with no tracks — it's also the entry point to the
        // online subtitle search inside the picker.
        m_subtitleRow->setText(subtitleLabel);
        m_subtitleRow->setVisibility(brls::Visibility::VISIBLE);
    }
}

// ── Audio & Subtitles picker (merged, tabbed) ───────────────────────
// Palette literals for the picker dialog (artboard tokens). Kept local so
// the dialog can read as its own neutral panel floating over the live page.
namespace pickcol {
    inline NVGcolor dialogBg()  { return nvgRGB(0x2B, 0x2B, 0x2B); }
    inline NVGcolor surface()   { return nvgRGB(0x38, 0x38, 0x38); }
    inline NVGcolor surface2()  { return nvgRGB(0x40, 0x40, 0x40); }
    inline NVGcolor surface3()  { return nvgRGB(0x49, 0x49, 0x49); }
    inline NVGcolor line()      { return nvgRGB(0x47, 0x47, 0x47); }
    inline NVGcolor text()      { return nvgRGB(255, 255, 255); }
    inline NVGcolor muted()     { return nvgRGB(0xB4, 0xB4, 0xBA); }
    inline NVGcolor dim()       { return nvgRGB(0x8A, 0x8A, 0x90); }
    inline NVGcolor gold()      { return nvgRGB(0xE5, 0xA0, 0x0D); }
    inline NVGcolor goldBright(){ return nvgRGB(0xFF, 0xC2, 0x3D); }
    inline NVGcolor goldInk()   { return nvgRGB(0x24, 0x1C, 0x08); }
    inline NVGcolor goldTint()  { return nvgRGBA(229, 160, 13, 41); }  // 16%
    inline NVGcolor scrim()     { return nvgRGBA(0, 0, 0, 115); }       // ~0.45
    inline NVGcolor clear()     { return nvgRGBA(0, 0, 0, 0); }
    inline NVGcolor ok()        { return nvgRGB(62, 207, 142); }        // #3ECF8E
}

enum class StreamBadge { None, Ext, Emb, Forced };

static std::string pkUpper(std::string s) {
    for (auto& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    return s;
}
static std::string pkLower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

// Human label for a stream row: a named track (commentary etc.) wins,
// then the language, then whatever displayTitle Plex handed us.
static std::string streamName(const PlexStream& s) {
    if (!s.title.empty())    return s.title;
    if (!s.language.empty()) return s.language;
    return s.displayTitle.empty() ? "Unknown" : s.displayTitle;
}
// Audio sub-line: "AC3 · 5.1" from codec + channel count.
static std::string audioSubLine(const PlexStream& s) {
    std::string codec = pkUpper(s.codec);
    std::string ch;
    switch (s.channels) {
        case 1:  ch = "Mono";   break;
        case 2:  ch = "Stereo"; break;
        case 6:  ch = "5.1";    break;
        case 8:  ch = "7.1";    break;
        default: if (s.channels > 0) ch = std::to_string(s.channels) + " ch"; break;
    }
    if (codec.empty()) return ch;
    if (ch.empty())    return codec;
    return codec + "  \xC2\xB7  " + ch;
}
// 2-letter language tile text (EN, ES, FR…) from the Plex language code.
static std::string langTile(const PlexStream& s) {
    std::string c = pkLower(s.languageCode);
    if (c.size() == 2) return pkUpper(c);
    struct M { const char* k; const char* v; };
    static const M kMap[] = {
        {"eng","EN"},{"spa","ES"},{"fre","FR"},{"fra","FR"},{"ger","DE"},{"deu","DE"},
        {"ita","IT"},{"por","PT"},{"rus","RU"},{"jpn","JA"},{"kor","KO"},{"chi","ZH"},
        {"zho","ZH"},{"dut","NL"},{"nld","NL"},{"pol","PL"},{"swe","SV"},{"nor","NO"},
        {"dan","DA"},{"fin","FI"},{"ara","AR"},{"heb","HE"},{"hin","HI"},{"tur","TR"},
        {"ell","EL"},{"gre","EL"},{"ces","CS"},{"cze","CS"},{"hun","HU"},{"tha","TH"},
        {"vie","VI"},{"ukr","UK"},{"ron","RO"},{"rum","RO"},{"ind","ID"},
    };
    for (const auto& m : kMap) if (c == m.k) return m.v;
    if (c.size() >= 2)            return pkUpper(c.substr(0, 2));
    if (s.language.size() >= 2)   return pkUpper(s.language.substr(0, 2));
    return "";
}
// Subtitle sub-line ("SRT · Full") — codec plus a content descriptor that
// complements the location badge (EXT/EMB) rather than duplicating it.
static std::string subtitleDescriptor(const PlexStream& s) {
    if (s.forced)          return "Forced";
    if (s.hearingImpaired) return "SDH";
    return "Full";
}
static std::string subtitleSubLine(const PlexStream& s) {
    std::string codec = pkUpper(s.codec);
    std::string desc  = subtitleDescriptor(s);
    if (codec.empty()) return desc;
    return codec + "  \xC2\xB7  " + desc;
}
// Forced wins the badge; otherwise show where the track lives (external
// sidecar vs embedded in the container).
static StreamBadge subtitleBadge(const PlexStream& s) {
    if (s.forced) return StreamBadge::Forced;
    return s.external ? StreamBadge::Ext : StreamBadge::Emb;
}

// One picker row: a 24px check (gold fill + ink tick when selected, else a
// hollow ring), a name + optional codec sub-line, and an optional trailing
// format badge. Focus uses the borealis-native highlight (left as-is); the
// gold check is the *selected* state, kept visually separate from focus.
static brls::Box* makeStreamRow(const std::string& langCode,
                                const std::string& name,
                                const std::string& sub,
                                bool selected,
                                StreamBadge badge,
                                std::function<bool(brls::View*)> onClick) {
    namespace pc = pickcol;
    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setHeight(48.0f);
    row->setCornerRadius(11.0f);
    row->setPaddingLeft(12.0f);
    row->setPaddingRight(12.0f);
    row->setMarginBottom(5.0f);
    row->setFocusable(true);
    row->setHighlightCornerRadius(11.0f);

    // Selection check.
    auto* check = new brls::Box();
    check->setWidth(24.0f);
    check->setHeight(24.0f);
    check->setCornerRadius(12.0f);
    check->setMarginRight(13.0f);
    check->setJustifyContent(brls::JustifyContent::CENTER);
    check->setAlignItems(brls::AlignItems::CENTER);
    if (selected) {
        check->setBackgroundColor(pc::gold());
        auto* tick = new brls::Label();
        tick->setText("\xE2\x9C\x93");  // ✓
        tick->setFontSize(14.0f);
        tick->setTextColor(pc::goldInk());
        check->addView(tick);
    } else {
        check->setBorderColor(pc::surface3());
        check->setBorderThickness(2.0f);
    }
    row->addView(check);

    // Language code tile (EN / ES …).
    if (!langCode.empty()) {
        auto* lt = new brls::Box();
        lt->setWidth(32.0f);
        lt->setHeight(23.0f);
        lt->setCornerRadius(6.0f);
        lt->setBackgroundColor(pc::surface3());
        lt->setJustifyContent(brls::JustifyContent::CENTER);
        lt->setAlignItems(brls::AlignItems::CENTER);
        lt->setMarginRight(12.0f);
        auto* ltl = new brls::Label();
        ltl->setText(langCode);
        ltl->setFontSize(11.0f);
        ltl->setTextColor(pc::muted());
        lt->addView(ltl);
        row->addView(lt);
    }

    // Name + sub-line.
    auto* col = new brls::Box();
    col->setAxis(brls::Axis::COLUMN);
    col->setGrow(1.0f);
    col->setShrink(1.0f);   // yield space so the badge never gets squeezed
    col->setJustifyContent(brls::JustifyContent::CENTER);
    auto* nameLbl = new brls::Label();
    nameLbl->setText(name);
    nameLbl->setFontSize(15.0f);
    nameLbl->setSingleLine(true);
    nameLbl->setTextColor(selected ? pc::goldBright() : pc::text());
    col->addView(nameLbl);
    if (!sub.empty()) {
        auto* subLbl = new brls::Label();
        subLbl->setText(sub);
        subLbl->setFontSize(12.0f);
        subLbl->setSingleLine(true);
        subLbl->setTextColor(pc::dim());
        col->addView(subLbl);
    }
    row->addView(col);

    // Format badge.
    if (badge != StreamBadge::None) {
        NVGcolor bbg = pc::goldTint(), btx = pc::gold();
        std::string btext = "FORCED";
        if (badge == StreamBadge::Ext) { bbg = nvgRGBA(137, 241, 242, 33); btx = nvgRGB(137, 241, 242); btext = "EXT"; }
        else if (badge == StreamBadge::Emb) { bbg = nvgRGBA(62, 207, 142, 36); btx = nvgRGB(62, 207, 142); btext = "EMB"; }
        auto* bdg = new brls::Box();
        bdg->setAxis(brls::Axis::ROW);
        bdg->setJustifyContent(brls::JustifyContent::CENTER);
        bdg->setAlignItems(brls::AlignItems::CENTER);
        bdg->setHeight(22.0f);
        bdg->setMinWidth(34.0f);
        bdg->setShrink(0.0f);   // never squeeze the badge; the name yields instead
        bdg->setCornerRadius(6.0f);
        bdg->setBackgroundColor(bbg);
        bdg->setPaddingLeft(8.0f);
        bdg->setPaddingRight(8.0f);
        bdg->setMarginLeft(8.0f);
        auto* bl = new brls::Label();
        bl->setText(btext);
        bl->setFontSize(10.0f);
        bl->setSingleLine(true);
        bl->setTextColor(btx);
        bdg->addView(bl);
        row->addView(bdg);
    }

    row->registerClickAction(onClick);
    row->addGestureRecognizer(new brls::TapGestureRecognizer(row));
    return row;
}

// Online search-result row: a gold download glyph and the release title +
// "{language} · {provider} · {extra}" sub-line. Distinct from the
// installed-track rows.
static brls::Box* makeResultRow(const std::string& title,
                                const std::string& sub,
                                std::function<bool(brls::View*)> onClick) {
    namespace pc = pickcol;
    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setHeight(54.0f);
    row->setCornerRadius(11.0f);
    row->setPaddingLeft(12.0f);
    row->setPaddingRight(12.0f);
    row->setMarginBottom(5.0f);
    row->setFocusable(true);
    row->setHighlightCornerRadius(11.0f);

    // Gold download glyph (the action).
    auto* dl = new MdiGlyphIcon(MdiGlyph::Download, pc::gold());
    dl->setWidth(22.0f);
    dl->setHeight(22.0f);
    dl->setMarginRight(13.0f);
    row->addView(dl);

    // Release title + meta sub-line.
    auto* col = new brls::Box();
    col->setAxis(brls::Axis::COLUMN);
    col->setGrow(1.0f);
    col->setShrink(1.0f);
    col->setJustifyContent(brls::JustifyContent::CENTER);
    auto* titleLbl = new brls::Label();
    titleLbl->setText(title);
    titleLbl->setFontSize(15.0f);
    titleLbl->setSingleLine(true);
    titleLbl->setTextColor(pc::text());
    col->addView(titleLbl);
    if (!sub.empty()) {
        auto* subLbl = new brls::Label();
        subLbl->setText(sub);
        subLbl->setFontSize(12.0f);
        subLbl->setSingleLine(true);
        subLbl->setTextColor(pc::dim());
        col->addView(subLbl);
    }
    row->addView(col);

    row->registerClickAction(onClick);
    row->addGestureRecognizer(new brls::TapGestureRecognizer(row));
    return row;
}

// Merged audio + subtitle picker. One compact panel floating over the live
// detail page (light scrim, page stays visible, no hint bar), with an
// Audio | Subtitles segmented switch. Reuses the existing stream lists and
// the exact selection / online-search handlers; only the shell changed.
void MediaDetailView::showStreamDialog(int defaultTab) {
    if (m_partId <= 0) return;
    namespace pc = pickcol;

    auto alive = m_alive;

    // Scrim doubles as the dialog-lifetime sentinel: async closures below
    // hold raw pointers to listBox / searchRow, so they capture dlgAlive
    // and bail once the overlay is popped (same idea as the old
    // SubtitleDialog destructor).
    struct PickerScrim : public brls::Box {
        std::shared_ptr<bool> dlgAlive = std::make_shared<bool>(true);
        ~PickerScrim() override { if (dlgAlive) *dlgAlive = false; }
    };
    auto* scrim = new PickerScrim();
    auto dlgAlive = scrim->dlgAlive;
    scrim->setAxis(brls::Axis::COLUMN);
    scrim->setWidthPercentage(100.0f);
    scrim->setHeightPercentage(100.0f);
    scrim->setJustifyContent(brls::JustifyContent::CENTER);
    scrim->setAlignItems(brls::AlignItems::CENTER);
    scrim->setBackgroundColor(pc::scrim());
    scrim->addGestureRecognizer(new brls::TapGestureRecognizer(scrim,
        []() { brls::Application::popActivity(); }));

    // ── Panel shell ─────────────────────────────────────────────────────
    auto* panel = new brls::Box();
    panel->setAxis(brls::Axis::COLUMN);
    panel->setWidth(460.0f);
    panel->setBackgroundColor(pc::dialogBg());
    panel->setBorderColor(pc::line());
    panel->setBorderThickness(1.0f);
    panel->setCornerRadius(18.0f);
    panel->setShadowType(brls::ShadowType::GENERIC);
    // Swallow taps landing on the panel's dead space (header, padding) so
    // they don't bubble to the scrim's tap-to-dismiss. Interactive children
    // (tabs, rows, close chip) keep their own taps.
    panel->addGestureRecognizer(new brls::TapGestureRecognizer(panel, []() {}));

    // ── Header: icon tile + title/sub + close chip ─────────────────────
    auto* header = new brls::Box();
    header->setAxis(brls::Axis::ROW);
    header->setAlignItems(brls::AlignItems::CENTER);
    header->setPadding(20.0f, 22.0f, 14.0f, 22.0f);
    header->setLineColor(pc::line());
    header->setLineBottom(1.0f);

    auto* iconTile = new brls::Box();
    iconTile->setWidth(38.0f);
    iconTile->setHeight(38.0f);
    iconTile->setCornerRadius(10.0f);
    iconTile->setBackgroundColor(pc::goldTint());
    iconTile->setJustifyContent(brls::JustifyContent::CENTER);
    iconTile->setAlignItems(brls::AlignItems::CENTER);
    iconTile->setMarginRight(12.0f);
    auto* iconGlyph = new MdiGlyphIcon(MdiGlyph::ClosedCaption, pc::gold());
    iconGlyph->setWidth(22.0f);
    iconGlyph->setHeight(22.0f);
    iconTile->addView(iconGlyph);
    header->addView(iconTile);

    auto* titleCol = new brls::Box();
    titleCol->setAxis(brls::Axis::COLUMN);
    titleCol->setGrow(1.0f);
    titleCol->setJustifyContent(brls::JustifyContent::CENTER);
    auto* titleLbl = new brls::Label();
    titleLbl->setText("Audio & Subtitles");
    titleLbl->setFontSize(21.0f);
    titleLbl->setTextColor(pc::text());
    titleLbl->setSingleLine(true);
    titleCol->addView(titleLbl);
    if (!m_item.title.empty()) {
        auto* subLbl = new brls::Label();
        subLbl->setText(m_item.title);
        subLbl->setFontSize(12.0f);
        subLbl->setTextColor(pc::dim());
        subLbl->setSingleLine(true);
        titleCol->addView(subLbl);
    }
    header->addView(titleCol);

    auto* closeChip = new brls::Box();
    closeChip->setWidth(32.0f);
    closeChip->setHeight(32.0f);
    closeChip->setCornerRadius(16.0f);
    closeChip->setBackgroundColor(pc::surface3());
    closeChip->setJustifyContent(brls::JustifyContent::CENTER);
    closeChip->setAlignItems(brls::AlignItems::CENTER);
    closeChip->setFocusable(true);
    closeChip->setHighlightCornerRadius(16.0f);
    auto* closeX = new brls::Label();
    closeX->setText("\xE2\x9C\x95");  // ✕
    closeX->setFontSize(13.0f);
    closeX->setTextColor(pc::muted());
    closeChip->addView(closeX);
    closeChip->registerClickAction([](brls::View*) { brls::Application::popActivity(); return true; });
    closeChip->addGestureRecognizer(new brls::TapGestureRecognizer(closeChip));
    header->addView(closeChip);
    panel->addView(header);

    // ── Segmented tab switch ───────────────────────────────────────────
    auto* tabTrack = new brls::Box();
    tabTrack->setAxis(brls::Axis::ROW);
    tabTrack->setBackgroundColor(pc::surface());
    tabTrack->setCornerRadius(12.0f);
    tabTrack->setPadding(6.0f, 6.0f, 6.0f, 6.0f);
    tabTrack->setMarginTop(12.0f);
    tabTrack->setMarginLeft(12.0f);
    tabTrack->setMarginRight(12.0f);

    struct TabHandle { brls::Box* box; brls::Label* label; };
    auto makeTab = [](const std::string& text) -> TabHandle {
        auto* t = new brls::Box();
        t->setAxis(brls::Axis::ROW);
        t->setHeight(38.0f);
        t->setGrow(1.0f);
        t->setCornerRadius(8.0f);
        t->setJustifyContent(brls::JustifyContent::CENTER);
        t->setAlignItems(brls::AlignItems::CENTER);
        t->setFocusable(true);
        t->setHighlightCornerRadius(8.0f);
        auto* l = new brls::Label();
        l->setText(text);
        l->setFontSize(14.0f);
        t->addView(l);
        return { t, l };
    };
    TabHandle audioTab = makeTab("Audio");
    TabHandle subsTab  = makeTab("Subtitles");
    audioTab.box->setMarginRight(4.0f);
    tabTrack->addView(audioTab.box);
    tabTrack->addView(subsTab.box);
    panel->addView(tabTrack);

    auto activeTab = std::make_shared<int>(defaultTab);
    auto styleTabs = [audioTab, subsTab, activeTab]() {
        bool a = (*activeTab == 0);
        audioTab.box->setBackgroundColor(a ? pickcol::gold() : pickcol::clear());
        audioTab.label->setTextColor(a ? pickcol::goldInk() : pickcol::muted());
        subsTab.box->setBackgroundColor(!a ? pickcol::gold() : pickcol::clear());
        subsTab.label->setTextColor(!a ? pickcol::goldInk() : pickcol::muted());
    };

    // ── Body: scrolling list + persistent search row (subs only) ───────
    auto* body = new brls::Box();
    body->setAxis(brls::Axis::COLUMN);
    body->setPadding(12.0f, 12.0f, 12.0f, 12.0f);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setHeight(320.0f);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    auto* listBox = new brls::Box();
    listBox->setAxis(brls::Axis::COLUMN);
    scroll->setContentView(listBox);
    body->addView(scroll);

    // Search row: globe tile + two-line label + chevron, with a gold-tint
    // (dashed-style) border. Lives outside listBox so it survives rebuilds
    // and stays a stable focus-park target; shown only on the Subtitles tab.
    auto* searchRow = new brls::Box();
    searchRow->setAxis(brls::Axis::ROW);
    searchRow->setAlignItems(brls::AlignItems::CENTER);
    searchRow->setHeight(56.0f);
    searchRow->setCornerRadius(12.0f);
    searchRow->setPadding(0.0f, 14.0f, 0.0f, 14.0f);
    searchRow->setMarginTop(8.0f);
    searchRow->setBorderColor(pc::goldTint());
    searchRow->setBorderThickness(1.0f);
    searchRow->setFocusable(true);
    searchRow->setHighlightCornerRadius(12.0f);

    auto* globeTile = new brls::Box();
    globeTile->setWidth(34.0f);
    globeTile->setHeight(34.0f);
    globeTile->setCornerRadius(17.0f);
    globeTile->setBackgroundColor(pc::goldTint());
    globeTile->setJustifyContent(brls::JustifyContent::CENTER);
    globeTile->setAlignItems(brls::AlignItems::CENTER);
    globeTile->setMarginRight(12.0f);
    auto* globe = new MdiGlyphIcon(MdiGlyph::Web, pc::gold());
    globe->setWidth(20.0f);
    globe->setHeight(20.0f);
    globeTile->addView(globe);
    searchRow->addView(globeTile);

    auto* searchCol = new brls::Box();
    searchCol->setAxis(brls::Axis::COLUMN);
    searchCol->setGrow(1.0f);
    searchCol->setJustifyContent(brls::JustifyContent::CENTER);
    auto* searchMainLbl = new brls::Label();
    searchMainLbl->setText("Search online for subtitles\xE2\x80\xA6");
    searchMainLbl->setFontSize(15.0f);
    searchMainLbl->setSingleLine(true);
    searchMainLbl->setTextColor(pc::gold());
    searchCol->addView(searchMainLbl);
    auto* searchSubLbl = new brls::Label();
    searchSubLbl->setText("OpenSubtitles \xC2\xB7 enter a language");
    searchSubLbl->setFontSize(12.0f);
    searchSubLbl->setSingleLine(true);
    searchSubLbl->setTextColor(pc::dim());
    searchCol->addView(searchSubLbl);
    searchRow->addView(searchCol);

    auto* chevron = new brls::Label();
    chevron->setText("\xE2\x80\xBA");  // ›
    chevron->setFontSize(20.0f);
    chevron->setTextColor(pc::dim());
    chevron->setMarginLeft(8.0f);
    searchRow->addView(chevron);

    body->addView(searchRow);
    panel->addView(body);

    // Language driving the results view (shared by the IME callback and the
    // re-labelling helper). resultsMode replaces the old button-text probe.
    auto currentLang = std::make_shared<std::string>(
        Application::getInstance().getSettings().defaultSubtitleLanguage);
    if (currentLang->empty()) *currentLang = "en";
    auto resultsMode = std::make_shared<bool>(false);
    auto setSearchBtnFor = [searchMainLbl, searchSubLbl, currentLang, resultsMode](bool inResults) {
        *resultsMode = inResults;
        if (inResults) {
            searchMainLbl->setText("Change language");
            searchSubLbl->setText("Currently: " + *currentLang);
        } else {
            searchMainLbl->setText("Search online for subtitles\xE2\x80\xA6");
            searchSubLbl->setText("OpenSubtitles \xC2\xB7 enter a language");
        }
    };

    // Closures declared up front so they can reference each other.
    auto buildAudioList     = std::make_shared<std::function<void()>>();
    auto buildInstalledList = std::make_shared<std::function<void()>>();
    auto showResults        = std::make_shared<std::function<void(const std::vector<PlexClient::SubtitleResult>&)>>();
    auto runSearch          = std::make_shared<std::function<void(std::string)>>();
    auto promptLang         = std::make_shared<std::function<void()>>();
    auto selectTab          = std::make_shared<std::function<void(int)>>();

    // Audio tab: one row per audio stream; selecting applies it (same
    // setStreamSelection path) and closes the dialog.
    *buildAudioList = [this, alive, dlgAlive, listBox, audioTab]() {
        if (!alive->load() || !*dlgAlive) return;
        brls::Application::giveFocus(audioTab.box);  // park before clearViews
        listBox->clearViews();
        brls::View* first = nullptr;
        for (const auto& s : m_streams) {
            if (s.streamType != 2) continue;
            int streamId = s.id;
            std::string display = streamName(s);
            auto* rowv = makeStreamRow(langTile(s), display, audioSubLine(s), s.selected, StreamBadge::None,
                [this, alive, streamId, display](brls::View*) {
                    brls::Application::popActivity();
                    int partId = m_partId;
                    asyncRun([this, alive, partId, streamId, display]() {
                        PlexClient::getInstance().setStreamSelection(partId, streamId, -1);
                        brls::sync([this, alive, streamId, display]() {
                            if (!alive->load()) return;
                            for (auto& s2 : m_streams)
                                if (s2.streamType == 2) s2.selected = (s2.id == streamId);
                            updateStreamRowLabels();
                        });
                    });
                    return true;
                });
            listBox->addView(rowv);
            if (!first) first = rowv;
        }
        if (!first) {
            auto* ph = new brls::Label();
            ph->setText("No alternate audio tracks");
            ph->setFontSize(14.0f);
            ph->setTextColor(pickcol::dim());
            ph->setMarginTop(8.0f);
            ph->setMarginLeft(6.0f);
            listBox->addView(ph);
        }
        if (first) brls::Application::giveFocus(first);
    };

    // Subtitles tab: "None" then each installed track. Verbatim selection
    // handlers (setStreamSelection), now rendered as rich rows.
    *buildInstalledList = [this, alive, dlgAlive, listBox, searchRow, setSearchBtnFor]() {
        if (!alive->load() || !*dlgAlive) return;
        setSearchBtnFor(/*resultsMode=*/false);
        brls::Application::giveFocus(searchRow);  // park before clearViews
        listBox->clearViews();

        bool anyOn = false;
        for (const auto& s : m_streams)
            if ((s.streamType == 3 || s.streamType == 4) && s.selected) { anyOn = true; break; }

        auto* noneRow = makeStreamRow("", "None", "Subtitles off", !anyOn, StreamBadge::None,
            [this, alive](brls::View*) {
                brls::Application::popActivity();
                int partId = m_partId;
                asyncRun([this, alive, partId]() {
                    PlexClient::getInstance().setStreamSelection(partId, -1, 0);
                    brls::sync([this, alive]() {
                        if (!alive->load()) return;
                        for (auto& s : m_streams)
                            if (s.streamType == 3 || s.streamType == 4) s.selected = false;
                        updateStreamRowLabels();
                    });
                });
                return true;
            });
        listBox->addView(noneRow);
        brls::View* first = noneRow;

        for (const auto& s : m_streams) {
            if (s.streamType != 3 && s.streamType != 4) continue;
            std::string display = streamName(s);
            int streamId  = s.id;
            bool selected = s.selected;
            StreamBadge badge = subtitleBadge(s);
            listBox->addView(makeStreamRow(langTile(s), display, subtitleSubLine(s), selected, badge,
                [this, alive, streamId, display](brls::View*) {
                    brls::Application::popActivity();
                    int partId = m_partId;
                    asyncRun([this, alive, partId, streamId, display]() {
                        PlexClient::getInstance().setStreamSelection(partId, -1, streamId);
                        brls::sync([this, alive, streamId, display]() {
                            if (!alive->load()) return;
                            for (auto& s : m_streams)
                                if (s.streamType == 3 || s.streamType == 4)
                                    s.selected = (s.id == streamId);
                            updateStreamRowLabels();
                        });
                    });
                    return true;
                }));
        }
        if (first) brls::Application::giveFocus(first);
    };

    // Search-results view inside the same scroll. Picking a result installs
    // it (selectSearchedSubtitle), then refetches streams. Verbatim flow.
    *showResults = [this, alive, dlgAlive, listBox, searchRow, setSearchBtnFor](
            const std::vector<PlexClient::SubtitleResult>& results) {
        if (!alive->load() || !*dlgAlive) return;
        setSearchBtnFor(/*resultsMode=*/true);
        brls::Application::giveFocus(searchRow);
        listBox->clearViews();

        if (results.empty()) {
            auto* none = new brls::Label();
            none->setText("No subtitles found");
            none->setFontSize(14.0f);
            none->setMarginTop(8.0f);
            none->setTextColor(pickcol::dim());
            listBox->addView(none);
            return;
        }

        brls::View* first = nullptr;
        for (const auto& r : results) {
            std::string display   = r.displayTitle.empty() ? r.language : r.displayTitle;
            std::string key       = r.key;
            std::string ratingKey = m_item.ratingKey;

            // Sub-line: language (+ SDH) · provider (· forced signs).
            std::string meta = r.language;
            if (r.hearingImpaired && !meta.empty()) meta += " (SDH)";
            if (!r.provider.empty())
                meta += (meta.empty() ? "" : "  \xC2\xB7  ") + r.provider;
            if (r.forced)
                meta += (meta.empty() ? "" : "  \xC2\xB7  ") + std::string("forced signs");

            auto* rr = makeResultRow(display, meta,
                [this, alive, key, ratingKey, display](brls::View*) {
                    brls::Application::popActivity();
                    int partId = m_partId;
                    asyncRun([this, alive, ratingKey, partId, key, display]() {
                        bool ok = PlexClient::getInstance()
                            .selectSearchedSubtitle(ratingKey, partId, key);
                        std::vector<PlexStream> fresh;
                        int freshPart = partId;
                        PlexClient::getInstance().fetchStreams(ratingKey, fresh, freshPart);
                        brls::sync([this, alive, ok, display, fresh, freshPart]() {
                            if (!alive->load()) return;
                            if (ok) {
                                m_streams = fresh;
                                m_partId  = freshPart;
                                updateStreamRowLabels();
                                brls::Application::notify("Subtitle added: " + display);
                            } else {
                                brls::Application::notify("Couldn't add subtitle");
                            }
                        });
                    });
                    return true;
                });
            listBox->addView(rr);
            if (!first) first = rr;
        }
        if (first) brls::Application::giveFocus(first);
    };

    // Loading-state + asyncRun + handoff glue. Verbatim from before.
    *runSearch = [this, alive, dlgAlive, listBox, searchRow, currentLang, showResults](std::string lang) {
        if (!alive->load() || !*dlgAlive) return;
        if (lang.empty()) lang = "en";
        *currentLang = lang;
        brls::Application::giveFocus(searchRow);
        listBox->clearViews();
        auto* loading = new brls::Label();
        loading->setText("Searching " + lang + "\xE2\x80\xA6");
        loading->setFontSize(14.0f);
        loading->setMarginTop(8.0f);
        loading->setTextColor(pickcol::dim());
        listBox->addView(loading);

        std::string ratingKey = m_item.ratingKey;
        asyncRun([this, alive, dlgAlive, ratingKey, lang, showResults]() {
            std::vector<PlexClient::SubtitleResult> results;
            PlexClient::getInstance().searchSubtitles(ratingKey, lang, results);
            brls::sync([alive, dlgAlive, showResults, results]() {
                if (!alive->load() || !*dlgAlive) return;
                (*showResults)(results);
            });
        });
    };

    *promptLang = [alive, dlgAlive, currentLang, runSearch]() {
        if (!alive->load() || !*dlgAlive) return;
        auto* ime = brls::Application::getImeManager();
        if (!ime) return;
        std::string seed = *currentLang;
        if (seed.empty()) seed = "en";
        ime->openForText([alive, dlgAlive, runSearch](std::string lang) {
            if (!alive->load() || !*dlgAlive) return;
            (*runSearch)(lang);
        }, "Subtitle language (e.g. en, es, fr)", seed, 8, seed);
    };

    // Tab switch: restyle, toggle the search row, rebuild the list.
    *selectTab = [alive, dlgAlive, activeTab, styleTabs, searchRow, resultsMode, buildAudioList, buildInstalledList](int tab) {
        if (!alive->load() || !*dlgAlive) return;
        *activeTab = tab;
        *resultsMode = false;  // leaving any results view; Back now dismisses
        styleTabs();
        if (tab == 0) {
            searchRow->setVisibility(brls::Visibility::GONE);
            (*buildAudioList)();
        } else {
            searchRow->setVisibility(brls::Visibility::VISIBLE);
            (*buildInstalledList)();
        }
    };

    audioTab.box->registerClickAction([selectTab](brls::View*) { (*selectTab)(0); return true; });
    audioTab.box->addGestureRecognizer(new brls::TapGestureRecognizer(audioTab.box));
    subsTab.box->registerClickAction([selectTab](brls::View*) { (*selectTab)(1); return true; });
    subsTab.box->addGestureRecognizer(new brls::TapGestureRecognizer(subsTab.box));

    searchRow->registerClickAction([currentLang, runSearch, promptLang, resultsMode](brls::View*) {
        if (*resultsMode) {
            (*promptLang)();
        } else {
            std::string lang = *currentLang;
            if (lang.empty()) lang = "en";
            (*runSearch)(lang);
        }
        return true;
    });
    searchRow->addGestureRecognizer(new brls::TapGestureRecognizer(searchRow));

    scrim->addView(panel);
    // Back: from the online-results view, return to the installed list;
    // otherwise close the dialog. (No on-screen "Back to installed" row.)
    scrim->registerAction("Back", brls::ControllerButton::BUTTON_B,
        [resultsMode, dlgAlive, buildInstalledList](brls::View*) {
            if (*resultsMode) {
                brls::sync([dlgAlive, buildInstalledList]() {
                    if (!*dlgAlive) return;
                    (*buildInstalledList)();
                });
            } else {
                brls::Application::popActivity();
            }
            return true;
        });

    brls::Application::pushActivity(new PopoverActivity(scrim));
    (*selectTab)(defaultTab == 0 ? 0 : 1);
}

void MediaDetailView::showCenteredChoice(const std::string& title,
                                         const std::string& subtitle,
                                         std::vector<OptionRow> rows) {
    namespace pc = pickcol;

    // Full-screen translucent scrim, centered panel — same shell as the
    // audio/subtitle picker. Tap outside / Back dismisses.
    auto* scrim = new brls::Box();
    scrim->setAxis(brls::Axis::COLUMN);
    scrim->setWidthPercentage(100.0f);
    scrim->setHeightPercentage(100.0f);
    scrim->setJustifyContent(brls::JustifyContent::CENTER);
    scrim->setAlignItems(brls::AlignItems::CENTER);
    scrim->setBackgroundColor(pc::scrim());
    scrim->addGestureRecognizer(new brls::TapGestureRecognizer(scrim,
        []() { brls::Application::popActivity(); }));

    auto* panel = new brls::Box();
    panel->setAxis(brls::Axis::COLUMN);
    panel->setWidth(420.0f);
    panel->setBackgroundColor(pc::dialogBg());
    panel->setBorderColor(pc::line());
    panel->setBorderThickness(1.0f);
    panel->setCornerRadius(18.0f);
    panel->setShadowType(brls::ShadowType::GENERIC);
    panel->setPadding(22.0f, 22.0f, 16.0f, 22.0f);
    // Swallow taps on the panel's dead space so they don't dismiss.
    panel->addGestureRecognizer(new brls::TapGestureRecognizer(panel, []() {}));

    auto* titleLbl = new brls::Label();
    titleLbl->setText(title);
    titleLbl->setFontSize(21.0f);
    titleLbl->setTextColor(pc::text());
    panel->addView(titleLbl);

    if (!subtitle.empty()) {
        auto* subLbl = new brls::Label();
        subLbl->setText(subtitle);
        subLbl->setFontSize(13.0f);
        subLbl->setTextColor(pc::dim());
        subLbl->setMarginTop(3.0f);
        panel->addView(subLbl);
    }

    auto* hair = new brls::Box();
    hair->setHeight(1.0f);
    hair->setBackgroundColor(pc::line());
    hair->setMarginTop(12.0f);
    hair->setMarginBottom(12.0f);
    panel->addView(hair);

    for (auto& r : rows) {
        const bool primary = r.primary;
        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setHeight(50.0f);
        row->setCornerRadius(11.0f);
        row->setPaddingLeft(14.0f);
        row->setPaddingRight(14.0f);
        row->setMarginBottom(8.0f);
        row->setFocusable(true);
        row->setHighlightCornerRadius(11.0f);
        // Uniform neutral rows — no filled "primary" row, which reads as
        // pre-selected at rest. The accent lives in the label colour; focus is
        // the only selection cue.
        row->setBackgroundColor(pc::surface());

        if (!r.icon.empty()) {
            auto* img = new brls::Image();
            img->setWidth(22.0f);
            img->setHeight(22.0f);
            img->setScalingType(brls::ImageScalingType::FIT);
            img->setMarginRight(12.0f);
            img->setImageFromRes("icons/" + r.icon);
            row->addView(img);
        }

        auto* lbl = new brls::Label();
        lbl->setText(r.label);
        lbl->setFontSize(16.0f);
        lbl->setTextColor(primary ? pc::gold() : (r.danger ? pc::muted() : pc::text()));
        lbl->setGrow(1.0f);
        row->addView(lbl);

        auto action = r.action;
        row->registerClickAction([action](brls::View* v) {
            // Dismiss the dialog FIRST so an action that pushes a new activity
            // (e.g. opening the player) lands on a clean stack.
            brls::Application::popActivity();
            if (action) return action(v);
            return true;
        });
        row->addGestureRecognizer(new brls::TapGestureRecognizer(row));
        panel->addView(row);
    }

    scrim->addView(panel);
    scrim->registerAction("Back", brls::ControllerButton::BUTTON_B,
        [](brls::View*) { brls::Application::popActivity(); return true; });
    brls::Application::pushActivity(new PopoverActivity(scrim));
}

void MediaDetailView::showAudioPicker() {
    showStreamDialog(/*defaultTab=*/0);
}

void MediaDetailView::showSubtitlePicker() {
    showStreamDialog(/*defaultTab=*/1);
}

} // namespace vitaplex
