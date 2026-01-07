/**
 * VitaPlex - Downloads Tab Implementation
 */

#include "view/downloads_tab.hpp"
#include "app/downloads_manager.hpp"
#include "activity/player_activity.hpp"

namespace vitaplex {

DownloadsTab::DownloadsTab() {
    this->setAxis(brls::Axis::COLUMN);
    this->setPadding(20);
    this->setGrow(1.0f);

    // Header
    auto header = new brls::Label();
    header->setText("Downloads");
    header->setFontSize(24);
    header->setMargins(0, 0, 20, 0);
    this->addView(header);

    // Sync button
    auto syncBtn = new brls::Button();
    syncBtn->setText("Sync Progress to Server");
    syncBtn->setMargins(0, 0, 20, 0);
    syncBtn->registerClickAction([](brls::View*) {
        DownloadsManager::getInstance().syncProgressToServer();
        brls::Application::notify("Progress synced to server");
        return true;
    });
    this->addView(syncBtn);

    // List container
    m_listContainer = new brls::Box();
    m_listContainer->setAxis(brls::Axis::COLUMN);
    m_listContainer->setGrow(1.0f);
    this->addView(m_listContainer);

    // Empty label
    m_emptyLabel = new brls::Label();
    m_emptyLabel->setText("No downloads yet.\nUse the download button on media details to save for offline viewing.");
    m_emptyLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    m_emptyLabel->setVerticalAlign(brls::VerticalAlign::CENTER);
    m_emptyLabel->setGrow(1.0f);
    m_emptyLabel->setVisibility(brls::Visibility::GONE);
    m_listContainer->addView(m_emptyLabel);
}

void DownloadsTab::willAppear(bool resetState) {
    brls::Box::willAppear(resetState);
    refresh();
}

void DownloadsTab::refresh() {
    // Clear existing items (except empty label)
    while (m_listContainer->getChildren().size() > 1) {
        m_listContainer->removeView(m_listContainer->getChildren()[0]);
    }

    auto downloads = DownloadsManager::getInstance().getDownloads();

    if (downloads.empty()) {
        m_emptyLabel->setVisibility(brls::Visibility::VISIBLE);
        return;
    }

    m_emptyLabel->setVisibility(brls::Visibility::GONE);

    for (const auto& item : downloads) {
        auto row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setPadding(10);
        row->setMargins(0, 0, 10, 0);
        row->setBackgroundColor(nvgRGBA(40, 40, 40, 200));
        row->setCornerRadius(8);

        // Title and info
        auto infoBox = new brls::Box();
        infoBox->setAxis(brls::Axis::COLUMN);
        infoBox->setGrow(1.0f);

        auto titleLabel = new brls::Label();
        std::string displayTitle = item.title;
        if (!item.parentTitle.empty()) {
            displayTitle = item.parentTitle + " - " + item.title;
        }
        titleLabel->setText(displayTitle);
        titleLabel->setFontSize(18);
        infoBox->addView(titleLabel);

        // Status/progress
        auto statusLabel = new brls::Label();
        statusLabel->setFontSize(14);

        std::string statusText;
        switch (item.state) {
            case DownloadState::QUEUED:
                statusText = "Queued";
                break;
            case DownloadState::DOWNLOADING:
                if (item.totalBytes > 0) {
                    int percent = (int)((item.downloadedBytes * 100) / item.totalBytes);
                    statusText = "Downloading... " + std::to_string(percent) + "%";
                } else {
                    statusText = "Downloading...";
                }
                break;
            case DownloadState::PAUSED:
                statusText = "Paused";
                break;
            case DownloadState::COMPLETED:
                statusText = "Ready to play";
                if (item.viewOffset > 0) {
                    int minutes = (int)(item.viewOffset / 60000);
                    statusText += " (" + std::to_string(minutes) + " min watched)";
                }
                break;
            case DownloadState::FAILED:
                statusText = "Download failed";
                break;
        }
        statusLabel->setText(statusText);
        infoBox->addView(statusLabel);

        row->addView(infoBox);

        // Actions based on state
        if (item.state == DownloadState::COMPLETED) {
            auto playBtn = new brls::Button();
            playBtn->setText("Play");
            playBtn->setMargins(0, 0, 0, 10);

            std::string ratingKey = item.ratingKey;
            std::string localPath = item.localPath;
            playBtn->registerClickAction([ratingKey, localPath](brls::View*) {
                // Play local file
                brls::Application::pushActivity(new PlayerActivity(ratingKey, true));
                return true;
            });
            row->addView(playBtn);

            auto deleteBtn = new brls::Button();
            deleteBtn->setText("Delete");
            std::string key = item.ratingKey;
            deleteBtn->registerClickAction([key](brls::View*) {
                DownloadsManager::getInstance().deleteDownload(key);
                brls::Application::notify("Download deleted");
                return true;
            });
            row->addView(deleteBtn);
        } else if (item.state == DownloadState::DOWNLOADING || item.state == DownloadState::QUEUED) {
            auto cancelBtn = new brls::Button();
            cancelBtn->setText("Cancel");
            std::string key = item.ratingKey;
            cancelBtn->registerClickAction([key](brls::View*) {
                DownloadsManager::getInstance().cancelDownload(key);
                brls::Application::notify("Download cancelled");
                return true;
            });
            row->addView(cancelBtn);
        }

        // Add row at the beginning (before empty label)
        m_listContainer->addView(row, 0);
    }
}

void DownloadsTab::showDownloadOptions(const std::string& ratingKey, const std::string& title) {
    // Not implemented - download options would be shown from media detail view
}

} // namespace vitaplex
