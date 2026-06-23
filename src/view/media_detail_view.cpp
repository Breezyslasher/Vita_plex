/**
 * VitaPlex - Media Detail View implementation
 */

#include "view/media_detail_view.hpp"
#include "view/media_item_cell.hpp"
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

    // Create scrollable content
    m_scrollView = new brls::ScrollingFrame();
    m_scrollView->setGrow(1.0f);

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

    // Play/resume/download buttons (movies only)
    if (m_item.mediaType == MediaType::MOVIE) {
        m_playButton = new brls::Button();
        m_playButton->setText("Play");
        m_playButton->setWidth(200);
        m_playButton->setHeight(44);
        m_playButton->setMarginTop(20);
        m_playButton->registerClickAction([this](brls::View* view) {
            onPlay(false);
            return true;
        });
        m_playButton->addGestureRecognizer(new brls::TapGestureRecognizer(m_playButton));
        leftBox->addView(m_playButton);

        if (m_item.viewOffset > 0) {
            m_resumeButton = new brls::Button();
            m_resumeButton->setText("Resume");
            m_resumeButton->setWidth(200);
            m_resumeButton->setHeight(44);
            m_resumeButton->setMarginTop(10);
            m_resumeButton->registerClickAction([this](brls::View* view) {
                onPlay(true);
                return true;
            });
            m_resumeButton->addGestureRecognizer(new brls::TapGestureRecognizer(m_resumeButton));
            leftBox->addView(m_resumeButton);
        }

        m_downloadButton = new brls::Button();

        // Check current download state
        {
            DownloadItem dlCheck;
            if (DownloadsManager::getInstance().getDownloadCopy(m_item.ratingKey, dlCheck)) {
                switch (dlCheck.state) {
                    case DownloadState::COMPLETED:
                        m_downloadButton->setText("Downloaded");
                        break;
                    case DownloadState::TRANSCODING:
                        m_downloadButton->setText("Transcoding...");
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
        }

        m_downloadButton->setWidth(200);
        m_downloadButton->setHeight(44);
        m_downloadButton->setMarginTop(10);
        // Padding pushes the centred label rightward so it clears the
        // absolutely-positioned icon overlay below. brls::Button doesn't
        // expose its internal label's alignment, so we lean on the
        // centring + asymmetric padding to get the icon-on-left look.
        m_downloadButton->setPaddingLeft(40);
        m_downloadButton->registerClickAction([this](brls::View* view) {
            onDownload();
            return true;
        });
        m_downloadButton->addGestureRecognizer(new brls::TapGestureRecognizer(m_downloadButton));
        leftBox->addView(m_downloadButton);

        // MDI download icon overlayed on the left of the button.
        // (resources/icons/download.png already shipped with the repo.)
        m_downloadIcon = new brls::Image();
        m_downloadIcon->setImageFromRes("icons/download.png");
        m_downloadIcon->setWidth(20);
        m_downloadIcon->setHeight(20);
        m_downloadIcon->setScalingType(brls::ImageScalingType::FIT);
        m_downloadIcon->setPositionType(brls::PositionType::ABSOLUTE);
        m_downloadIcon->setPositionLeft(12);
        m_downloadIcon->setPositionTop(12);
        m_downloadButton->addView(m_downloadIcon);

        // Watched-state toggle. Label is the action the press will
        // perform — i.e. "Mark Watched" when the item is currently
        // unwatched, and vice versa. The button is wired straight to
        // PlexClient::markAsWatched/markAsUnwatched in onToggleWatched.
        m_markWatchedButton = new brls::Button();
        m_markWatchedButton->setText(m_item.watched ? "Mark Unwatched" : "Mark Watched");
        m_markWatchedButton->setWidth(200);
        m_markWatchedButton->setHeight(44);
        m_markWatchedButton->setMarginTop(10);
        m_markWatchedButton->setPaddingLeft(40);
        m_markWatchedButton->registerClickAction([this](brls::View*) {
            onToggleWatched();
            return true;
        });
        m_markWatchedButton->addGestureRecognizer(new brls::TapGestureRecognizer(m_markWatchedButton));
        leftBox->addView(m_markWatchedButton);

        // MDI check-circle icon: filled when item is already watched,
        // outline when unwatched. onToggleWatched() swaps the resource
        // each press to mirror the new state.
        m_markWatchedIcon = new brls::Image();
        m_markWatchedIcon->setImageFromRes(m_item.watched
            ? "icons/check-circle.png"
            : "icons/check-circle-outline.png");
        m_markWatchedIcon->setWidth(20);
        m_markWatchedIcon->setHeight(20);
        m_markWatchedIcon->setScalingType(brls::ImageScalingType::FIT);
        m_markWatchedIcon->setPositionType(brls::PositionType::ABSOLUTE);
        m_markWatchedIcon->setPositionLeft(12);
        m_markWatchedIcon->setPositionTop(12);
        m_markWatchedButton->addView(m_markWatchedIcon);

        // Explicit DOWN / UP routes between the leftBox buttons. Borealis's
        // default spatial focus search jumps over Resume / Download /
        // Mark Watched from Play and lands on whatever sits in rightBox /
        // below, presumably because the leftBox is shorter than the right
        // column and the scan picks the first focusable outside the row.
        // Wiring the chain manually fixes Play→Resume→Download→Watched.
        brls::View* nextDown = m_markWatchedButton;
        m_downloadButton->setCustomNavigationRoute(brls::FocusDirection::DOWN, nextDown);
        m_markWatchedButton->setCustomNavigationRoute(brls::FocusDirection::UP, m_downloadButton);

        nextDown = m_downloadButton;
        if (m_resumeButton) {
            m_resumeButton->setCustomNavigationRoute(brls::FocusDirection::DOWN, nextDown);
            m_downloadButton->setCustomNavigationRoute(brls::FocusDirection::UP, m_resumeButton);
            nextDown = m_resumeButton;
        } else {
            m_downloadButton->setCustomNavigationRoute(brls::FocusDirection::UP, m_playButton);
        }
        m_playButton->setCustomNavigationRoute(brls::FocusDirection::DOWN, nextDown);
        if (m_resumeButton) {
            m_resumeButton->setCustomNavigationRoute(brls::FocusDirection::UP, m_playButton);
        }
    }

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

    m_summaryLabel = new brls::Label();
    m_summaryLabel->setFontSize(16);
    m_summaryLabel->setText(m_fullDescription);
    m_summaryLabel->setFocusable(true);

    m_summaryScroll->setContentView(m_summaryLabel);
    if (m_fullDescription.empty()) {
        m_summaryScroll->setVisibility(brls::Visibility::GONE);
    }
    rightBox->addView(m_summaryScroll);

    // AUDIO / SUBTITLES picker rows. Movies are the only type with a
    // single Part (and therefore a stable selection) at this layer;
    // episodes have their own Part per cell so the picker belongs on
    // the episode row, not this header. The rows start hidden — they
    // appear once loadStreams() populates the list asynchronously.
    if (m_item.mediaType == MediaType::MOVIE) {
        m_audioRow = new brls::Button();
        m_audioRow->setText("AUDIO: Loading…");
        m_audioRow->setHeight(36);
        m_audioRow->setMarginBottom(8);
        m_audioRow->setVisibility(brls::Visibility::GONE);
        m_audioRow->registerClickAction([this](brls::View*) {
            showAudioPicker();
            return true;
        });
        m_audioRow->addGestureRecognizer(new brls::TapGestureRecognizer(m_audioRow));
        rightBox->addView(m_audioRow);

        m_subtitleRow = new brls::Button();
        m_subtitleRow->setText("SUBTITLES: Loading…");
        m_subtitleRow->setHeight(36);
        m_subtitleRow->setMarginBottom(8);
        m_subtitleRow->setVisibility(brls::Visibility::GONE);
        m_subtitleRow->registerClickAction([this](brls::View*) {
            showSubtitlePicker();
            return true;
        });
        m_subtitleRow->addGestureRecognizer(new brls::TapGestureRecognizer(m_subtitleRow));
        rightBox->addView(m_subtitleRow);
    }

    topRow->addView(rightBox);
    m_mainContent->addView(topRow);

    // Combined scrolling container for seasons/episodes + extras
    // This keeps the header/description fixed while only the media rows scroll
    if (m_item.mediaType == MediaType::SHOW ||
        m_item.mediaType == MediaType::SEASON ||
        m_item.mediaType == MediaType::MOVIE) {

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

        // Extras container (trailers, featurettes, etc.) for movies and shows
        if (m_item.mediaType == MediaType::MOVIE ||
            m_item.mediaType == MediaType::SHOW) {

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
        m_item.mediaType == MediaType::SEASON ||
        m_item.mediaType == MediaType::MOVIE) {
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

                // Focus setup: if no description, transfer focus to first category item
                if (m_fullDescription.empty() && m_musicCategoriesBox &&
                    !m_musicCategoriesBox->getChildren().empty()) {
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

            // Focus setup: if no description, transfer focus to first category item
            if (m_fullDescription.empty() && m_musicCategoriesBox &&
                !m_musicCategoriesBox->getChildren().empty()) {
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

                if (m_summaryLabel && !m_fullDescription.empty()) {
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
enum class MdiGlyph { Download, Restart, Close };

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

        if (row.primary) {
            rowBox->setBackgroundColor(pc::gold());
        }

        // Leading icon. download/restart/close are drawn as exact MDI
        // vectors (tinted to match the row); everything else uses its PNG.
        brls::View* iconView;
        NVGcolor iconColor = row.primary ? pc::goldInk() : pc::text();
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
        if (row.primary)      lbl->setTextColor(pc::goldInk());
        else if (row.danger)  lbl->setTextColor(pc::muted());
        else                  lbl->setTextColor(pc::text());
        rowBox->addView(lbl);

        // Trailing mono sub-value.
        if (!row.sub.empty()) {
            auto* sub = new brls::Label();
            sub->setText(row.sub);
            sub->setFontSize(12.0f);
            sub->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
            sub->setSingleLine(true);
            sub->setMarginLeft(8.0f);
            sub->setTextColor(row.primary ? pc::goldInkSub() : pc::dim());
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

        // Brighten the gold fill on focus (nice-to-have); keep the warm halo.
        if (row.primary) {
            brls::Box* rb = rowBox;
            rb->getFocusEvent()->subscribe(
                [rb](brls::View*) { rb->setBackgroundColor(popcol::goldBright()); });
            rb->getFocusLostEvent()->subscribe(
                [rb](brls::View*) { rb->setBackgroundColor(popcol::gold()); });
        }

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
    if (m_summaryLabel && !m_fullDescription.empty()) {
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
    if (m_summaryLabel && !m_fullDescription.empty() && m_markWatchedButton) {
        m_markWatchedButton->setCustomNavigationRoute(
            brls::FocusDirection::DOWN, m_summaryLabel);
    }

    if (hasChildren && upTarget) {
        for (auto* child : m_childrenBox->getChildren()) {
            child->setCustomNavigationRoute(brls::FocusDirection::UP, upTarget);
        }
    }

    // If description exists, set DOWN from description to first child (or extras if no children)
    if (m_summaryLabel && !m_fullDescription.empty()) {
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

    if (m_fullDescription.empty() && firstFocusable) {
        brls::Application::giveFocus(firstFocusable);

        // Bridge the last leftBox button down to the children/extras row.
        // Only the bottom button gets this route — the rest of the chain
        // (Play→Resume→Download→Watched) was wired explicitly in the
        // constructor and we don't want to short-circuit it here.
        brls::View* lastLeftButton = m_markWatchedButton
            ? m_markWatchedButton
            : (m_downloadButton
                ? m_downloadButton
                : (m_resumeButton ? m_resumeButton : m_playButton));
        if (lastLeftButton) {
            lastLeftButton->setCustomNavigationRoute(
                brls::FocusDirection::DOWN, firstFocusable);
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

void MediaDetailView::updateStreamRowLabels() {
    // Find the currently-selected audio + subtitle streams. Plex marks
    // exactly one of each as selected on the server side, so we just
    // read the bool the API gave us — no second round-trip needed.
    std::string audioLabel = "(default)";
    std::string subtitleLabel = "None";
    bool hasAudio = false;
    bool hasSubs  = false;
    for (const auto& s : m_streams) {
        if (s.streamType == 2) {
            hasAudio = true;
            if (s.selected) {
                audioLabel = s.displayTitle.empty() ? s.language : s.displayTitle;
            }
        } else if (s.streamType == 3 || s.streamType == 4) {
            hasSubs = true;
            if (s.selected) {
                subtitleLabel = s.displayTitle.empty() ? s.language : s.displayTitle;
            }
        }
    }

    if (m_audioRow) {
        if (hasAudio) {
            m_audioRow->setText("AUDIO: " + audioLabel);
            m_audioRow->setVisibility(brls::Visibility::VISIBLE);
        } else {
            m_audioRow->setVisibility(brls::Visibility::GONE);
        }
    }
    if (m_subtitleRow) {
        // Always keep the subtitle row visible — even when nothing is
        // installed, the user needs the tap target to open the dialog
        // and run the online subtitle search. Label reads "None" when
        // nothing is on; "Add subtitles" hint when the list is empty
        // entirely.
        if (hasSubs) {
            m_subtitleRow->setText("SUBTITLES: " + subtitleLabel);
        } else {
            m_subtitleRow->setText("SUBTITLES: None  ·  Tap to search online");
        }
        m_subtitleRow->setVisibility(brls::Visibility::VISIBLE);
    }
}

// Shared helper: build a row-style button used inside the audio /
// subtitle pickers. The check-mark prefix mirrors the screenshot's
// "currently selected" affordance, the row height keeps the column
// scannable, and left-alignment with padding lets long track names
// hang off naturally instead of getting clipped at the centre.
static brls::Button* makePickerRow(const std::string& text,
                                   bool isSelected,
                                   std::function<bool(brls::View*)> onClick) {
    auto* btn = new brls::Button();
    btn->setText(isSelected ? ("✓ " + text) : ("   " + text));
    btn->setHeight(40);
    btn->setMarginBottom(6);
    btn->setPaddingLeft(14);
    btn->registerClickAction(onClick);
    btn->addGestureRecognizer(new brls::TapGestureRecognizer(btn));
    return btn;
}

void MediaDetailView::showAudioPicker() {
    if (m_partId <= 0 || m_streams.empty()) return;

    auto* dialog = new brls::Dialog("Select audio track");
    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setPadding(20);
    content->setWidth(460);

    // Audio lists are typically short (a few tracks per release) so we
    // intentionally skip the search box and just give the dialog a tall
    // scroll area for the rare polyglot release.
    auto* scroll = new brls::ScrollingFrame();
    scroll->setHeight(360);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    auto* listBox = new brls::Box();
    listBox->setAxis(brls::Axis::COLUMN);
    scroll->setContentView(listBox);

    bool any = false;
    for (const auto& s : m_streams) {
        if (s.streamType != 2) continue;
        any = true;
        int streamId = s.id;
        std::string display = s.displayTitle.empty() ? s.language : s.displayTitle;
        auto alive = m_alive;
        listBox->addView(makePickerRow(display, s.selected,
            [this, alive, dialog, streamId, display](brls::View*) {
                dialog->dismiss();
                int partId = m_partId;
                asyncRun([this, alive, partId, streamId, display]() {
                    PlexClient::getInstance().setStreamSelection(partId, streamId, -1);
                    brls::sync([this, alive, streamId, display]() {
                        if (!alive->load()) return;
                        for (auto& s : m_streams) {
                            if (s.streamType == 2) s.selected = (s.id == streamId);
                        }
                        updateStreamRowLabels();
                    });
                });
                return true;
            }));
    }
    if (!any) {
        delete scroll;
        delete content;
        delete dialog;
        return;
    }
    content->addView(scroll);
    dialog->addView(content);
    dialog->open();
}

void MediaDetailView::showSubtitlePicker() {
    if (m_partId <= 0) return;

    // brls::Dialog dies when the user presses B / taps outside, but the
    // closures we hand to the IME / brls::sync / asyncRun still hold raw
    // pointers to listBox and the dialog. Subclass with a destructor
    // that flips a shared_ptr<bool>; every closure captures the flag and
    // bails if the dialog is already gone. Same pattern as m_alive but
    // scoped to this dialog rather than the whole detail view.
    struct SubtitleDialog : public brls::Dialog {
        using brls::Dialog::Dialog;
        std::shared_ptr<bool> dlgAlive = std::make_shared<bool>(true);
        ~SubtitleDialog() override { if (dlgAlive) *dlgAlive = false; }
    };

    auto* dialog = new SubtitleDialog("Subtitles");
    auto dlgAlive = dialog->dlgAlive;
    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setPadding(20);
    content->setWidth(460);

    // Track whether ANY subtitle is currently selected so we can render
    // the leading "None" row with the correct check-mark state.
    bool anySelected = false;
    for (const auto& s : m_streams) {
        if ((s.streamType == 3 || s.streamType == 4) && s.selected) {
            anySelected = true; break;
        }
    }

    // ── Search online row ───────────────────────────────────────────
    // Mirrors the in-player flow: tap → IME prompts for a language
    // code (defaults to "en") → PlexClient::searchSubtitles hits the
    // server's subtitle providers (OpenSubtitles etc.) → results
    // replace the list. Picking a result installs it via
    // selectSearchedSubtitle and refreshes the installed list.
    auto* searchBtn = new brls::Button();
    searchBtn->setText("Search online for subtitles…");
    searchBtn->setHeight(40);
    searchBtn->setMarginBottom(12);
    searchBtn->setPaddingLeft(14);
    content->addView(searchBtn);

    // Tracks the language currently driving the results view. Lives on
    // the heap so the IME callback (where it's assigned) and the
    // re-labelling helpers (where it's read) share the same value
    // through their shared_ptr captures.
    auto currentLang = std::make_shared<std::string>(
        Application::getInstance().getSettings().defaultSubtitleLanguage);
    if (currentLang->empty()) *currentLang = "en";

    // Re-label the search button to fit the current view: the
    // installed-list mode just invites a fresh search, while the
    // results mode shows the language in play and reads as "change
    // language" so the user knows tapping it re-prompts.
    auto setSearchBtnFor = [searchBtn, currentLang](bool resultsMode) {
        if (resultsMode) {
            searchBtn->setText("Change language: " + *currentLang);
        } else {
            searchBtn->setText("Search online for subtitles…");
        }
    };

    // ── Scrollable list ─────────────────────────────────────────────
    auto* scroll = new brls::ScrollingFrame();
    scroll->setHeight(360);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    auto* listBox = new brls::Box();
    listBox->setAxis(brls::Axis::COLUMN);
    scroll->setContentView(listBox);
    content->addView(scroll);

    // Captured by closures so both the initial render and any later
    // refresh (after installing a fresh subtitle) hit the same body.
    auto alive    = m_alive;
    auto buildInstalledList = std::make_shared<std::function<void()>>();
    *buildInstalledList = [this, alive, dlgAlive, dialog, listBox, searchBtn, setSearchBtnFor]() {
        if (!alive->load() || !*dlgAlive) return;
        // Reset the top button to "search" wording since we're no
        // longer looking at a language-scoped results list.
        setSearchBtnFor(/*resultsMode=*/false);
        // Borealis keeps a raw pointer to whatever view currently has
        // focus. clearViews() below deletes every row in listBox —
        // including the focused "Back to installed" button that
        // triggered this rebuild. Park focus on searchBtn (which sits
        // outside listBox and survives the rebuild) so the focus
        // pointer can't dangle.
        brls::Application::giveFocus(searchBtn);
        listBox->clearViews();

        // Re-read selection state, since installing a new subtitle
        // updates m_streams (we refetch after every install).
        bool anyOn = false;
        for (const auto& s : m_streams) {
            if ((s.streamType == 3 || s.streamType == 4) && s.selected) {
                anyOn = true; break;
            }
        }

        // "None" first — Plex uses subtitleStreamID=0 to mean off.
        listBox->addView(makePickerRow("None", !anyOn,
            [this, alive, dialog](brls::View*) {
                dialog->dismiss();
                int partId = m_partId;
                asyncRun([this, alive, partId]() {
                    PlexClient::getInstance().setStreamSelection(partId, -1, 0);
                    brls::sync([this, alive]() {
                        if (!alive->load()) return;
                        for (auto& s : m_streams) {
                            if (s.streamType == 3 || s.streamType == 4) s.selected = false;
                        }
                        updateStreamRowLabels();
                    });
                });
                return true;
            }));

        for (const auto& s : m_streams) {
            if (s.streamType != 3 && s.streamType != 4) continue;
            std::string display = s.displayTitle.empty() ? s.language : s.displayTitle;
            int streamId  = s.id;
            bool selected = s.selected;
            listBox->addView(makePickerRow(display, selected,
                [this, alive, dialog, streamId, display](brls::View*) {
                    dialog->dismiss();
                    int partId = m_partId;
                    asyncRun([this, alive, partId, streamId, display]() {
                        PlexClient::getInstance().setStreamSelection(partId, -1, streamId);
                        brls::sync([this, alive, streamId, display]() {
                            if (!alive->load()) return;
                            for (auto& s : m_streams) {
                                if (s.streamType == 3 || s.streamType == 4) {
                                    s.selected = (s.id == streamId);
                                }
                            }
                            updateStreamRowLabels();
                        });
                    });
                    return true;
                }));
        }
    };
    (*buildInstalledList)();

    // Build the search-results view inside the same scroll area.
    // Picking a result calls selectSearchedSubtitle (Plex downloads
    // and attaches it server-side), then we refetch the stream list
    // so the newly-installed subtitle shows up as an "installed" row.
    auto showResults = std::make_shared<std::function<void(const std::vector<PlexClient::SubtitleResult>&)>>();
    *showResults = [this, alive, dlgAlive, dialog, listBox, searchBtn, setSearchBtnFor, buildInstalledList](
            const std::vector<PlexClient::SubtitleResult>& results) {
        if (!alive->load() || !*dlgAlive) return;
        // Results mode: top button advertises a language change.
        setSearchBtnFor(/*resultsMode=*/true);
        // Park focus on searchBtn before tearing down listBox's rows
        // (the "Searching…" label this replaces is focusable on Vita
        // because the IME callback giveFocus'd it).
        brls::Application::giveFocus(searchBtn);
        listBox->clearViews();

        auto* back = new brls::Button();
        back->setText("‹ Back to installed");
        back->setHeight(36);
        back->setMarginBottom(10);
        back->setPaddingLeft(14);
        // buildInstalledList calls listBox->clearViews(), which would
        // delete this back button while we're still inside its click
        // handler — a classic use-after-free that crashed the app. Defer
        // the rebuild to the next main-loop iteration via brls::sync so
        // the click handler returns first and the view tree is safe to
        // tear down. dlgAlive guards against the dialog dying first.
        back->registerClickAction([dlgAlive, buildInstalledList](brls::View*) {
            brls::sync([dlgAlive, buildInstalledList]() {
                if (!*dlgAlive) return;
                (*buildInstalledList)();
            });
            return true;
        });
        back->addGestureRecognizer(new brls::TapGestureRecognizer(back));
        listBox->addView(back);

        if (results.empty()) {
            auto* none = new brls::Label();
            none->setText("No subtitles found");
            none->setFontSize(14);
            none->setMarginTop(8);
            listBox->addView(none);
            return;
        }

        for (const auto& r : results) {
            std::string display = r.displayTitle.empty() ? r.language : r.displayTitle;
            if (!r.provider.empty()) display += "  (" + r.provider + ")";
            std::string key       = r.key;
            std::string ratingKey = m_item.ratingKey;
            listBox->addView(makePickerRow(display, false,
                [this, alive, dialog, key, ratingKey, display](brls::View*) {
                    dialog->dismiss();
                    int partId = m_partId;
                    asyncRun([this, alive, ratingKey, partId, key, display]() {
                        bool ok = PlexClient::getInstance()
                            .selectSearchedSubtitle(ratingKey, partId, key);
                        // Refresh the stream list so the newly-attached
                        // subtitle shows up next time the picker opens.
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
                }));
        }
    };

    // Extracted so the search button, the "change language" path, the
    // auto-trigger on empty installed list, and the post-IME callback
    // can all share the loading-state + asyncRun + result handoff glue.
    auto runSearch = std::make_shared<std::function<void(std::string)>>();
    *runSearch = [this, alive, dlgAlive, listBox, searchBtn, currentLang, showResults](std::string lang) {
        if (!alive->load() || !*dlgAlive) return;
        if (lang.empty()) lang = "en";
        *currentLang = lang;

        // Park focus on searchBtn before tearing down listBox.
        brls::Application::giveFocus(searchBtn);
        listBox->clearViews();
        auto* loading = new brls::Label();
        loading->setText("Searching " + lang + "…");
        loading->setFontSize(14);
        loading->setMarginTop(8);
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

    // "Change language" path — IME-prompts for the language, then
    // hands off to runSearch. Only used from the results-mode button
    // press; the initial / auto search skips this and goes straight
    // to runSearch with the saved default.
    auto promptLanguageThenSearch = std::make_shared<std::function<void()>>();
    *promptLanguageThenSearch = [alive, dlgAlive, currentLang, runSearch]() {
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

    searchBtn->registerClickAction([currentLang, runSearch, promptLanguageThenSearch, searchBtn](brls::View*) {
        // Read the live button label so we don't need a separate flag
        // for "which mode am I in" — setSearchBtnFor() is the only
        // thing that updates the text, and there are exactly two
        // possible captions.
        std::string label = searchBtn->getText();
        if (label.rfind("Change language", 0) == 0) {
            (*promptLanguageThenSearch)();
        } else {
            // First search — use the saved default; the user can
            // still switch via the relabelled button once results
            // show up.
            std::string lang = *currentLang;
            if (lang.empty()) lang = "en";
            (*runSearch)(lang);
        }
        return true;
    });
    searchBtn->addGestureRecognizer(new brls::TapGestureRecognizer(searchBtn));

    dialog->addView(content);
    dialog->open();

    // If nothing is installed, jump straight to the online search using
    // the saved default language. User can still change it from the
    // "Change language" affordance once the results list is showing.
    bool anyInstalled = false;
    for (const auto& s : m_streams) {
        if (s.streamType == 3 || s.streamType == 4) { anyInstalled = true; break; }
    }
    if (!anyInstalled) {
        brls::sync([dlgAlive, currentLang, runSearch]() {
            if (!*dlgAlive) return;
            std::string lang = *currentLang;
            if (lang.empty()) lang = "en";
            (*runSearch)(lang);
        });
    }
}

} // namespace vitaplex
