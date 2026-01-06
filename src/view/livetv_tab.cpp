/**
 * VitaPlex - Live TV Tab implementation
 */

#include "view/livetv_tab.hpp"
#include "app/application.hpp"
#include "utils/async.hpp"
#include "utils/image_loader.hpp"

namespace vitaplex {

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

    // Channels section
    auto* channelsLabel = new brls::Label();
    channelsLabel->setText("Channels");
    channelsLabel->setFontSize(22);
    channelsLabel->setMarginBottom(10);
    m_scrollContent->addView(channelsLabel);

    m_channelsRow = new brls::HScrollingFrame();
    m_channelsRow->setHeight(120);
    m_channelsRow->setMarginBottom(20);

    m_channelsContent = new brls::Box();
    m_channelsContent->setAxis(brls::Axis::ROW);
    m_channelsContent->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_channelsContent->setAlignItems(brls::AlignItems::CENTER);

    m_channelsRow->setContentView(m_channelsContent);
    m_scrollContent->addView(m_channelsRow);

    // Guide section header
    auto* guideLabel = new brls::Label();
    guideLabel->setText("Program Guide");
    guideLabel->setFontSize(22);
    guideLabel->setMarginBottom(10);
    guideLabel->setMarginTop(10);
    m_scrollContent->addView(guideLabel);

    m_guideBox = new brls::Box();
    m_guideBox->setAxis(brls::Axis::COLUMN);
    m_guideBox->setMarginBottom(20);
    m_scrollContent->addView(m_guideBox);

    // DVR Recordings section
    m_dvrLabel = new brls::Label();
    m_dvrLabel->setText("Scheduled Recordings");
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

void LiveTVTab::onFocusGained() {
    brls::Box::onFocusGained();

    if (!m_loaded) {
        loadChannels();
    }
}

void LiveTVTab::loadChannels() {
    asyncRun([this]() {
        brls::Logger::debug("LiveTVTab: Fetching channels (async)...");
        PlexClient& client = PlexClient::getInstance();

        std::vector<LiveTVChannel> channels;
        if (client.fetchLiveTVChannels(channels)) {
            brls::Logger::info("LiveTVTab: Got {} channels", channels.size());

            brls::sync([this, channels]() {
                m_channels = channels;
                m_channelsContent->clearViews();

                for (const auto& channel : m_channels) {
                    // Create channel card
                    auto* card = new brls::Box();
                    card->setAxis(brls::Axis::COLUMN);
                    card->setWidth(150);
                    card->setHeight(100);
                    card->setMarginRight(10);
                    card->setPadding(10);
                    card->setFocusable(true);
                    card->setBackgroundColor(nvgRGBA(50, 50, 60, 255));
                    card->setCornerRadius(8);

                    // Channel number and name
                    auto* numLabel = new brls::Label();
                    numLabel->setText(std::to_string(channel.channelNumber));
                    numLabel->setFontSize(24);
                    card->addView(numLabel);

                    auto* nameLabel = new brls::Label();
                    nameLabel->setText(channel.callSign.empty() ? channel.title : channel.callSign);
                    nameLabel->setFontSize(14);
                    card->addView(nameLabel);

                    // Current program
                    if (!channel.currentProgram.empty()) {
                        auto* progLabel = new brls::Label();
                        progLabel->setText(channel.currentProgram);
                        progLabel->setFontSize(12);
                        progLabel->setMarginTop(5);
                        card->addView(progLabel);
                    }

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

                m_loaded = true;
            });
        } else {
            brls::Logger::error("LiveTVTab: Failed to fetch channels");
            brls::sync([this]() {
                m_channelsContent->clearViews();
                auto* errorLabel = new brls::Label();
                errorLabel->setText("Failed to load Live TV channels");
                errorLabel->setFontSize(16);
                m_channelsContent->addView(errorLabel);
                m_loaded = true;
            });
        }
    });

    // Also load DVR recordings
    loadRecordings();
}

void LiveTVTab::loadGuide() {
    // TODO: Implement program guide loading
    // Uses /livetv/dvrs and grid endpoints
}

void LiveTVTab::loadRecordings() {
    asyncRun([this]() {
        brls::Logger::debug("LiveTVTab: Fetching DVR recordings...");
        PlexClient& client = PlexClient::getInstance();

        // TODO: Implement fetchDVRRecordings in PlexClient
        // For now, just clear the DVR section

        brls::sync([this]() {
            m_dvrContent->clearViews();

            auto* placeholder = new brls::Label();
            placeholder->setText("No scheduled recordings");
            placeholder->setFontSize(14);
            m_dvrContent->addView(placeholder);
        });
    });
}

void LiveTVTab::onChannelSelected(const LiveTVChannel& channel) {
    brls::Logger::info("LiveTVTab: Selected channel: {}", channel.title);

    // Start playing the channel
    Application::getInstance().pushPlayerActivity(channel.ratingKey);
}

void LiveTVTab::onProgramSelected(const GuideProgram& program, const LiveTVChannel& channel) {
    // Show program details with option to record
    brls::Dialog* dialog = new brls::Dialog(program.title + "\n\n" + program.summary);

    dialog->addButton("Watch Now", [this, channel, dialog]() {
        dialog->close();
        Application::getInstance().pushPlayerActivity(channel.ratingKey);
    });

    dialog->addButton("Record", [this, program, channel, dialog]() {
        dialog->close();
        scheduleRecording(program, channel);
    });

    dialog->addButton("Cancel", [dialog]() {
        dialog->close();
    });

    dialog->open();
}

void LiveTVTab::scheduleRecording(const GuideProgram& program, const LiveTVChannel& channel) {
    // TODO: Implement recording scheduling via Plex API
    // POST to /media/subscriptions with program details

    brls::Dialog* dialog = new brls::Dialog("Recording scheduled for: " + program.title);
    dialog->addButton("OK", [dialog]() { dialog->close(); });
    dialog->open();
}

} // namespace vitaplex
