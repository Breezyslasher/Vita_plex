/**
 * VitaPlex - Live TV Tab implementation
 */

#include "view/livetv_tab.hpp"
#include "app/application.hpp"
#include "utils/async.hpp"
#include "utils/image_loader.hpp"
#include "utils/http_client.hpp"
#include <algorithm>
#include <ctime>

namespace vitaplex {

// Constants for EPG grid layout
static const int CHANNEL_COLUMN_WIDTH = 100;  // Width of channel name column
static const int TIME_SLOT_WIDTH = 120;       // Width per 30-minute slot
static const int ROW_HEIGHT = 60;             // Height of each channel row
static const int TIME_HEADER_HEIGHT = 30;     // Height of time header

// Cache durations
static const int64_t FULL_RELOAD_INTERVAL = 300;   // 5 minutes between full EPG reloads
static const int64_t REFRESH_INTERVAL = 60;         // 1 minute between "now playing" refreshes

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
    m_titleLabel->setMarginBottom(20);
    m_scrollContent->addView(m_titleLabel);

    // Quick Channels section
    m_channelsLabel = new brls::Label();
    m_channelsLabel->setText("Quick Access");
    m_channelsLabel->setFontSize(22);
    m_channelsLabel->setMarginBottom(10);
    m_scrollContent->addView(m_channelsLabel);

    m_channelsRow = new brls::HScrollingFrame();
    m_channelsRow->setHeight(100);
    m_channelsRow->setMarginBottom(20);

    m_channelsContent = new brls::Box();
    m_channelsContent->setAxis(brls::Axis::ROW);
    m_channelsContent->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_channelsContent->setAlignItems(brls::AlignItems::CENTER);

    m_channelsRow->setContentView(m_channelsContent);
    m_scrollContent->addView(m_channelsRow);

    // EPG Guide section
    m_guideLabel = new brls::Label();
    m_guideLabel->setText("Program Guide");
    m_guideLabel->setFontSize(22);
    m_guideLabel->setMarginBottom(10);
    m_guideLabel->setMarginTop(10);
    m_scrollContent->addView(m_guideLabel);

    // Guide container - holds time header and channel grid
    m_guideContainer = new brls::Box();
    m_guideContainer->setAxis(brls::Axis::COLUMN);
    m_guideContainer->setHeight(350);  // Fixed height for guide section
    m_guideContainer->setMarginBottom(20);

    // Time header row (horizontal scroll)
    m_timeHeaderScroll = new brls::HScrollingFrame();
    m_timeHeaderScroll->setHeight(TIME_HEADER_HEIGHT);
    m_timeHeaderScroll->setMarginLeft(CHANNEL_COLUMN_WIDTH);  // Offset for channel column

    m_timeHeaderBox = new brls::Box();
    m_timeHeaderBox->setAxis(brls::Axis::ROW);
    m_timeHeaderBox->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_timeHeaderBox->setBackgroundColor(nvgRGBA(40, 40, 50, 255));
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

    m_scrollContent->addView(m_guideContainer);

    // DVR Recordings section
    m_dvrLabel = new brls::Label();
    m_dvrLabel->setText("DVR Recordings");
    m_dvrLabel->setFontSize(22);
    m_dvrLabel->setMarginBottom(10);
    m_dvrLabel->setMarginTop(10);
    m_scrollContent->addView(m_dvrLabel);

    m_dvrRow = new brls::HScrollingFrame();
    m_dvrRow->setHeight(100);
    m_dvrRow->setMarginBottom(20);

    m_dvrContent = new brls::Box();
    m_dvrContent->setAxis(brls::Axis::ROW);
    m_dvrContent->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_dvrContent->setAlignItems(brls::AlignItems::CENTER);

    m_dvrRow->setContentView(m_dvrContent);
    m_scrollContent->addView(m_dvrRow);

    m_scrollView->setContentView(m_scrollContent);
    this->addView(m_scrollView);

    // Load content
    brls::Logger::debug("LiveTVTab: Loading content...");
    loadChannels();
}

LiveTVTab::~LiveTVTab() {
    if (m_alive) { *m_alive = false; }
}

void LiveTVTab::willDisappear(bool resetState) {
    brls::Box::willDisappear(resetState);
    if (m_alive) *m_alive = false;
    ImageLoader::cancelAll();
}

void LiveTVTab::onFocusGained() {
    brls::Box::onFocusGained();
    m_alive = std::make_shared<bool>(true);

    if (!m_loaded) {
        // First load
        loadChannels();
    } else {
        // Already loaded - check if we need a refresh
        int64_t now = time(nullptr);

        if (now - m_lastFullLoadTime > FULL_RELOAD_INTERVAL) {
            // Full reload if EPG data is stale (> 5 min)
            brls::Logger::debug("LiveTVTab: Full EPG reload (stale data)");
            loadChannels();
        } else if (now - m_lastRefreshTime > REFRESH_INTERVAL) {
            // Lightweight refresh: just update "now playing" labels
            brls::Logger::debug("LiveTVTab: Refreshing current programs only");
            refreshCurrentPrograms();
        }
        // Otherwise: data is fresh, do nothing (no rebuild)
    }
}

std::string LiveTVTab::formatTime(int64_t timestamp) {
    if (timestamp == 0) {
        // Use current time if no timestamp
        timestamp = time(nullptr);
    }

    time_t t = (time_t)timestamp;
    struct tm* tm_info = localtime(&t);

    char buffer[16];
    strftime(buffer, sizeof(buffer), "%I:%M %p", tm_info);
    // Remove leading zero from hour
    if (buffer[0] == '0') {
        return std::string(buffer + 1);
    }
    return std::string(buffer);
}

void LiveTVTab::refreshCurrentPrograms() {
    // Lightweight refresh: fetch EPG data and only update "now playing" text
    // without rebuilding the entire UI
    asyncRun([this, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<LiveTVChannel> freshChannels;
        bool success = client.fetchEPGGrid(freshChannels, m_hoursToShow);

        if (success) {
            brls::sync([this, freshChannels, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_lastRefreshTime = time(nullptr);

                // Update current program info in cached channel data
                time_t now = time(nullptr);
                for (size_t i = 0; i < m_channels.size() && i < freshChannels.size(); i++) {
                    // Find matching channel in fresh data
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

                // Update quick access "now playing" labels
                updateQuickAccessPrograms();

                // Rebuild the EPG grid with fresh program data
                buildEPGGrid();
            });
        }
    });
}

void LiveTVTab::updateQuickAccessPrograms() {
    // Update the current program text on quick access cards
    for (size_t i = 0; i < m_quickAccessProgLabels.size() && i < m_channels.size(); i++) {
        if (m_quickAccessProgLabels[i]) {
            std::string prog = m_channels[i].currentProgram;
            if (prog.empty()) {
                // Find current program from programs list
                time_t now = time(nullptr);
                for (const auto& p : m_channels[i].programs) {
                    if (p.startTime <= (int64_t)now && p.endTime > (int64_t)now) {
                        prog = p.title;
                        break;
                    }
                }
            }
            if (prog.length() > 14) prog = prog.substr(0, 13) + "..";
            m_quickAccessProgLabels[i]->setText(prog.empty() ? "" : prog);
        }
    }
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
                m_channelsContent->clearViews();
                m_quickAccessProgLabels.clear();

                // Build quick access channel buttons
                for (const auto& channel : m_channels) {
                    auto* card = new brls::Box();
                    card->setAxis(brls::Axis::COLUMN);
                    card->setWidth(120);
                    card->setHeight(80);
                    card->setMarginRight(10);
                    card->setPadding(8);
                    card->setFocusable(true);
                    card->setBackgroundColor(nvgRGBA(50, 50, 60, 255));
                    card->setCornerRadius(8);

                    // Channel number
                    auto* numLabel = new brls::Label();
                    numLabel->setText(!channel.channelIdentifier.empty() ? channel.channelIdentifier : std::to_string(channel.channelNumber));
                    numLabel->setFontSize(20);
                    card->addView(numLabel);

                    // Channel name
                    auto* nameLabel = new brls::Label();
                    std::string name = channel.callSign.empty() ? channel.title : channel.callSign;
                    if (name.length() > 12) name = name.substr(0, 11) + "..";
                    nameLabel->setText(name);
                    nameLabel->setFontSize(12);
                    card->addView(nameLabel);

                    // Current program (stored for lightweight updates)
                    auto* progLabel = new brls::Label();
                    std::string prog = channel.currentProgram;
                    if (prog.empty()) {
                        // Check programs list for current
                        time_t now = time(nullptr);
                        for (const auto& p : channel.programs) {
                            if (p.startTime <= (int64_t)now && p.endTime > (int64_t)now) {
                                prog = p.title;
                                break;
                            }
                        }
                    }
                    if (prog.length() > 14) prog = prog.substr(0, 13) + "..";
                    progLabel->setText(prog);
                    progLabel->setFontSize(10);
                    progLabel->setMarginTop(4);
                    card->addView(progLabel);
                    m_quickAccessProgLabels.push_back(progLabel);

                    LiveTVChannel capturedChannel = channel;
                    card->registerClickAction([this, capturedChannel](brls::View* view) {
                        onChannelSelected(capturedChannel);
                        return true;
                    });

                    m_channelsContent->addView(card);
                }

                if (m_channels.empty()) {
                    auto* placeholder = new brls::Label();
                    placeholder->setText("No channels found. Set up Live TV in Plex settings.");
                    placeholder->setFontSize(16);
                    m_channelsContent->addView(placeholder);
                }

                // Build the EPG grid
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
                m_channelsContent->clearViews();
                auto* errorLabel = new brls::Label();
                errorLabel->setText("Failed to load Live TV");
                errorLabel->setFontSize(16);
                m_channelsContent->addView(errorLabel);
                m_loaded = true;
            });
        }
    });

    // Also load DVR recordings
    loadRecordings();
}

void LiveTVTab::buildEPGGrid() {
    // Clear existing grid
    m_timeHeaderBox->clearViews();
    m_guideBox->clearViews();

    if (m_channels.empty()) {
        auto* noDataLabel = new brls::Label();
        noDataLabel->setText("No program guide data available");
        noDataLabel->setFontSize(14);
        noDataLabel->setMarginLeft(10);
        m_guideBox->addView(noDataLabel);
        return;
    }

    // Set guide start time to current time rounded down to 30 minutes
    time_t now = time(nullptr);
    m_guideStartTime = now - (now % 1800);  // Round to 30 min

    int totalSlots = m_hoursToShow * 2;  // 2 slots per hour (30 min each)

    // Build time header
    for (int i = 0; i < totalSlots; i++) {
        int64_t slotTime = m_guideStartTime + (i * 1800);

        auto* timeSlot = new brls::Box();
        timeSlot->setWidth(TIME_SLOT_WIDTH);
        timeSlot->setHeight(TIME_HEADER_HEIGHT);
        timeSlot->setJustifyContent(brls::JustifyContent::CENTER);
        timeSlot->setAlignItems(brls::AlignItems::CENTER);
        timeSlot->setBorderColor(nvgRGBA(60, 60, 70, 255));
        timeSlot->setBorderThickness(1);

        auto* timeLabel = new brls::Label();
        timeLabel->setText(formatTime(slotTime));
        timeLabel->setFontSize(12);
        timeSlot->addView(timeLabel);

        m_timeHeaderBox->addView(timeSlot);
    }

    // Build channel rows
    for (const auto& channel : m_channels) {
        // Create row container
        auto* rowBox = new brls::Box();
        rowBox->setAxis(brls::Axis::ROW);
        rowBox->setHeight(ROW_HEIGHT);
        rowBox->setJustifyContent(brls::JustifyContent::FLEX_START);
        rowBox->setAlignItems(brls::AlignItems::CENTER);

        // Channel info column (fixed width)
        auto* channelCol = new brls::Box();
        channelCol->setAxis(brls::Axis::COLUMN);
        channelCol->setWidth(CHANNEL_COLUMN_WIDTH);
        channelCol->setHeight(ROW_HEIGHT);
        channelCol->setPadding(4);
        channelCol->setBackgroundColor(nvgRGBA(35, 35, 45, 255));
        channelCol->setJustifyContent(brls::JustifyContent::CENTER);

        auto* chNumLabel = new brls::Label();
        chNumLabel->setText(!channel.channelIdentifier.empty() ? channel.channelIdentifier : std::to_string(channel.channelNumber));
        chNumLabel->setFontSize(14);
        channelCol->addView(chNumLabel);

        auto* chNameLabel = new brls::Label();
        std::string chName = channel.callSign.empty() ? channel.title : channel.callSign;
        if (chName.length() > 10) chName = chName.substr(0, 9) + "..";
        chNameLabel->setText(chName);
        chNameLabel->setFontSize(10);
        channelCol->addView(chNameLabel);

        // Make channel column clickable to tune
        LiveTVChannel capturedChannel = channel;
        channelCol->setFocusable(true);
        channelCol->registerClickAction([this, capturedChannel](brls::View* view) {
            onChannelSelected(capturedChannel);
            return true;
        });

        rowBox->addView(channelCol);

        // Program cells container (scrollable horizontally)
        auto* programsBox = new brls::Box();
        programsBox->setAxis(brls::Axis::ROW);
        programsBox->setJustifyContent(brls::JustifyContent::FLEX_START);
        programsBox->setAlignItems(brls::AlignItems::STRETCH);
        programsBox->setGrow(1.0f);

        // If we have program data, create program blocks for all programs
        if (!channel.programs.empty()) {
            int64_t guideEndTime = m_guideStartTime + (m_hoursToShow * 3600);
            int64_t lastEndTime = m_guideStartTime;  // Track position for gaps

            for (size_t pi = 0; pi < channel.programs.size(); pi++) {
                const auto& prog = channel.programs[pi];

                // Skip programs entirely outside visible range
                if (prog.endTime <= m_guideStartTime || prog.startTime >= guideEndTime) continue;

                // Clamp to visible range
                int64_t visStart = std::max(prog.startTime, m_guideStartTime);
                int64_t visEnd = std::min(prog.endTime > 0 ? prog.endTime : visStart + 1800, guideEndTime);

                // Add gap spacer if there's a gap before this program
                if (visStart > lastEndTime) {
                    int gapPixels = (int)((visStart - lastEndTime) * TIME_SLOT_WIDTH / 1800);
                    if (gapPixels > 0) {
                        auto* spacer = new brls::Box();
                        spacer->setWidth(gapPixels);
                        spacer->setHeight(ROW_HEIGHT - 4);
                        programsBox->addView(spacer);
                    }
                }

                // Calculate width based on duration
                int64_t durationSec = visEnd - visStart;
                int cellWidth = (int)(durationSec * TIME_SLOT_WIDTH / 1800);
                if (cellWidth < 40) cellWidth = 40;  // Minimum width

                // Determine color: currently airing = brighter
                time_t now = time(nullptr);
                bool isCurrently = (prog.startTime <= (int64_t)now && prog.endTime > (int64_t)now);
                NVGcolor bgColor = isCurrently
                    ? nvgRGBA(60, 80, 100, 255)   // Current: brighter blue
                    : nvgRGBA(50, 60, 80, 255);   // Upcoming: darker

                auto* progCell = new brls::Box();
                progCell->setAxis(brls::Axis::COLUMN);
                progCell->setWidth(cellWidth);
                progCell->setHeight(ROW_HEIGHT - 4);
                progCell->setPadding(4);
                progCell->setMargins(2, 2, 2, 2);
                progCell->setBackgroundColor(bgColor);
                progCell->setCornerRadius(4);
                progCell->setFocusable(true);

                auto* progTitle = new brls::Label();
                std::string title = prog.title;
                int maxChars = cellWidth / 8;
                if (maxChars < 4) maxChars = 4;
                if ((int)title.length() > maxChars) title = title.substr(0, maxChars - 2) + "..";
                progTitle->setText(title);
                progTitle->setFontSize(11);
                progCell->addView(progTitle);

                // Show time range
                auto* timeRange = new brls::Label();
                timeRange->setText(formatTime(prog.startTime) + "-" + formatTime(prog.endTime));
                timeRange->setFontSize(9);
                timeRange->setMarginTop(2);
                progCell->addView(timeRange);

                GuideProgram gp;
                gp.title = prog.title;
                gp.startTime = prog.startTime;
                gp.endTime = prog.endTime;

                progCell->registerClickAction([this, gp, capturedChannel](brls::View* view) {
                    onProgramSelected(gp, capturedChannel);
                    return true;
                });

                programsBox->addView(progCell);
                lastEndTime = visEnd;
            }
        } else if (!channel.currentProgram.empty() && channel.programStart > 0) {
            // Fallback: use legacy current/next program fields
            int64_t progStart = std::max(channel.programStart, m_guideStartTime);
            int64_t guideEndTime = m_guideStartTime + (m_hoursToShow * 3600);
            int64_t progEnd = std::min(channel.programEnd > 0 ? channel.programEnd : progStart + 1800, guideEndTime);

            int startOffset = (int)((progStart - m_guideStartTime) * TIME_SLOT_WIDTH / 1800);
            int cellWidth = (int)((progEnd - progStart) * TIME_SLOT_WIDTH / 1800);
            if (cellWidth < 40) cellWidth = 40;

            if (startOffset > 0) {
                auto* spacer = new brls::Box();
                spacer->setWidth(startOffset);
                spacer->setHeight(ROW_HEIGHT - 4);
                programsBox->addView(spacer);
            }

            auto* progCell = new brls::Box();
            progCell->setAxis(brls::Axis::COLUMN);
            progCell->setWidth(cellWidth);
            progCell->setHeight(ROW_HEIGHT - 4);
            progCell->setPadding(4);
            progCell->setMargins(2, 2, 2, 2);
            progCell->setBackgroundColor(nvgRGBA(60, 80, 100, 255));
            progCell->setCornerRadius(4);
            progCell->setFocusable(true);

            auto* progTitle = new brls::Label();
            std::string title = channel.currentProgram;
            int maxChars = cellWidth / 8;
            if ((int)title.length() > maxChars) title = title.substr(0, maxChars - 2) + "..";
            progTitle->setText(title);
            progTitle->setFontSize(11);
            progCell->addView(progTitle);

            auto* timeRange = new brls::Label();
            timeRange->setText(formatTime(channel.programStart) + "-" + formatTime(channel.programEnd));
            timeRange->setFontSize(9);
            timeRange->setMarginTop(2);
            progCell->addView(timeRange);

            programsBox->addView(progCell);
        } else {
            // No program data - show "No guide data" across the grid
            auto* emptyCell = new brls::Box();
            emptyCell->setWidth(TIME_SLOT_WIDTH * 2);  // Span 2 slots
            emptyCell->setHeight(ROW_HEIGHT - 4);
            emptyCell->setMargins(2, 2, 2, 2);
            emptyCell->setBackgroundColor(nvgRGBA(40, 40, 50, 255));
            emptyCell->setCornerRadius(4);
            emptyCell->setFocusable(true);
            emptyCell->setPadding(4);

            auto* noInfo = new brls::Label();
            noInfo->setText("No guide data");
            noInfo->setFontSize(10);
            emptyCell->addView(noInfo);

            emptyCell->registerClickAction([this, capturedChannel](brls::View* view) {
                onChannelSelected(capturedChannel);
                return true;
            });

            programsBox->addView(emptyCell);
        }

        rowBox->addView(programsBox);
        m_guideBox->addView(rowBox);
    }
}

void LiveTVTab::loadGuide() {
    // Already handled in loadChannels with fetchEPGGrid
}

void LiveTVTab::loadRecordings() {
    asyncRun([this, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        brls::Logger::debug("LiveTVTab: Fetching DVR recordings...");

        PlexClient& client = PlexClient::getInstance();
        HttpClient httpClient;

        // Fetch scheduled recordings via /media/subscriptions
        std::string subsUrl = client.buildApiUrlPublic("/media/subscriptions");
        HttpRequest req;
        req.url = subsUrl;
        req.method = "GET";
        req.headers["Accept"] = "application/json";
        req.timeout = 15;

        HttpResponse resp = httpClient.request(req);

        std::vector<DVRRecording> recordings;

        if (resp.statusCode == 200 && !resp.body.empty()) {
            brls::Logger::debug("LiveTVTab: DVR subscriptions response ({} bytes)", resp.body.length());

            // Parse subscriptions - look for MediaSubscription objects
            size_t pos = 0;
            while (pos < resp.body.length()) {
                // Find next object with a "title" field
                size_t titlePos = resp.body.find("\"title\"", pos);
                if (titlePos == std::string::npos) break;

                // Find the enclosing object
                size_t objStart = resp.body.rfind('{', titlePos);
                if (objStart == std::string::npos) { pos = titlePos + 7; continue; }

                // Extract object
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
                std::string ratingKey = client.extractJsonValuePublic(obj, "ratingKey");
                std::string type = client.extractJsonValuePublic(obj, "type");

                if (!title.empty() && !ratingKey.empty()) {
                    DVRRecording rec;
                    rec.title = title;
                    rec.ratingKey = ratingKey;
                    rec.mediaSubscriptionId = ratingKey;
                    rec.summary = client.extractJsonValuePublic(obj, "summary");

                    std::string beginsAt = client.extractJsonValuePublic(obj, "beginsAt");
                    if (!beginsAt.empty()) {
                        rec.scheduledTime = atoll(beginsAt.c_str());
                    }

                    // Determine status
                    std::string status = client.extractJsonValuePublic(obj, "status");
                    if (status == "2" || status == "recording") {
                        rec.status = "recording";
                    } else if (status == "1" || status == "scheduled") {
                        rec.status = "scheduled";
                    } else {
                        rec.status = "scheduled";
                    }

                    recordings.push_back(rec);
                }

                pos = objEnd;
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
                placeholder->setFontSize(14);
                m_dvrContent->addView(placeholder);
                return;
            }

            for (const auto& rec : m_recordings) {
                auto* card = new brls::Box();
                card->setAxis(brls::Axis::COLUMN);
                card->setWidth(180);
                card->setHeight(80);
                card->setMarginRight(10);
                card->setPadding(8);
                card->setFocusable(true);
                card->setCornerRadius(8);

                // Color based on status
                if (rec.status == "recording") {
                    card->setBackgroundColor(nvgRGBA(120, 40, 40, 255));  // Red for recording
                } else {
                    card->setBackgroundColor(nvgRGBA(50, 60, 80, 255));   // Blue for scheduled
                }

                // Title
                auto* titleLabel = new brls::Label();
                std::string title = rec.title;
                if (title.length() > 20) title = title.substr(0, 19) + "..";
                titleLabel->setText(title);
                titleLabel->setFontSize(13);
                card->addView(titleLabel);

                // Status
                auto* statusLabel = new brls::Label();
                std::string statusText = rec.status;
                if (rec.scheduledTime > 0) {
                    statusText += " - " + formatTime(rec.scheduledTime);
                }
                statusLabel->setText(statusText);
                statusLabel->setFontSize(10);
                statusLabel->setMarginTop(4);
                card->addView(statusLabel);

                // Click to show options (cancel, etc.)
                DVRRecording capturedRec = rec;
                card->registerClickAction([this, capturedRec](brls::View* view) {
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

    // Use the channelIdentifier for tuning (e.g., "2.1")
    std::string channelKey = channel.channelIdentifier;
    if (channelKey.empty()) {
        channelKey = std::to_string(channel.channelNumber);
    }
    if (channelKey == "0" && !channel.ratingKey.empty()) {
        channelKey = channel.ratingKey;  // Last resort fallback
    }

    asyncRun([this, channel, channelKey, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        PlexClient& client = PlexClient::getInstance();
        std::string streamUrl;

        if (client.tuneLiveTVChannel(channelKey, streamUrl)) {
            brls::Logger::info("LiveTVTab: Got stream URL for channel {}", channel.title);
            brls::sync([streamUrl, channel]() {
                std::string title = channel.title;
                if (!channel.currentProgram.empty()) {
                    title += " - " + channel.currentProgram;
                }
                Application::getInstance().pushLiveTVPlayerActivity(streamUrl, title);
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
    // Build dialog message
    std::string message = program.title;
    if (program.startTime > 0 && program.endTime > 0) {
        message += "\n\n" + formatTime(program.startTime) + " - " + formatTime(program.endTime);
    }
    if (!program.summary.empty()) {
        message += "\n\n" + program.summary;
    }

    brls::Dialog* dialog = new brls::Dialog(message);

    dialog->addButton("Watch Now", [this, channel]() {
        onChannelSelected(channel);
    });

    dialog->addButton("Record", [this, program, channel]() {
        scheduleRecording(program, channel);
    });

    dialog->addButton("Cancel", []() {});

    dialog->open();
}

void LiveTVTab::scheduleRecording(const GuideProgram& program, const LiveTVChannel& channel) {
    asyncRun([this, program, channel, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        PlexClient& client = PlexClient::getInstance();
        HttpClient httpClient;

        // POST to /media/subscriptions to schedule a recording
        // The API expects parameters: prefs[minVideoQuality], prefs[replaceLowerQuality],
        // type (show/movie), targetLibrarySectionID, targetSectionID
        std::string subUrl = client.buildApiUrlPublic("/media/subscriptions");

        // Build parameters
        std::string params = "type=1";  // 1 = recording
        if (program.startTime > 0) {
            params += "&beginsAt=" + std::to_string(program.startTime);
        }
        if (program.endTime > 0) {
            params += "&endsAt=" + std::to_string(program.endTime);
        }
        if (!program.ratingKey.empty()) {
            params += "&key=" + HttpClient::urlEncode(program.ratingKey);
        }
        if (!channel.channelIdentifier.empty()) {
            params += "&channelIdentifier=" + HttpClient::urlEncode(channel.channelIdentifier);
        }

        HttpRequest req;
        req.url = subUrl;
        req.method = "POST";
        req.body = params;
        req.headers["Accept"] = "application/json";
        req.headers["Content-Type"] = "application/x-www-form-urlencoded";
        req.timeout = 15;

        HttpResponse resp = httpClient.request(req);

        bool success = (resp.statusCode == 200 || resp.statusCode == 201);
        std::string title = program.title;

        brls::sync([this, success, title, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !*alive) return;

            if (success) {
                brls::Dialog* dialog = new brls::Dialog("Recording scheduled: " + title);
                dialog->addButton("OK", []() {});
                dialog->open();
                // Refresh recordings list
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
        std::string title = recording.title;

        brls::sync([this, success, title, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !*alive) return;

            if (success) {
                brls::Application::notify("Recording cancelled: " + title);
                loadRecordings();
            } else {
                brls::Application::notify("Failed to cancel recording");
            }
        });
    });
}

} // namespace vitaplex
