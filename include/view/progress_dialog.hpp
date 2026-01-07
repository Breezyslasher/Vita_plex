/**
 * VitaPlex - Progress Dialog
 * Shows progress for operations like connecting, downloading
 */

#pragma once

#include <borealis.hpp>
#include <functional>
#include <string>
#include <chrono>

namespace vitaplex {

class ProgressDialog : public brls::Box {
public:
    ProgressDialog(const std::string& title);
    ~ProgressDialog();

    // Update the status text
    void setStatus(const std::string& status);

    // Update progress (0.0 - 1.0)
    void setProgress(float progress);

    // Set attempt info (e.g., "Attempt 2/5")
    void setAttempt(int current, int total);

    // Set network speed display
    void setSpeed(int64_t bytesPerSecond);

    // Update download progress with speed calculation
    void updateDownloadProgress(int64_t downloaded, int64_t total);

    // Show/hide the dialog
    void show();
    void dismiss();

    // Set cancel callback
    void setCancelCallback(std::function<void()> callback);

    // Static helper to show a connection progress dialog
    static ProgressDialog* showConnecting(const std::string& serverName);

    // Static helper to show a download progress dialog
    static ProgressDialog* showDownloading(const std::string& title);

private:
    brls::Label* m_titleLabel = nullptr;
    brls::Label* m_statusLabel = nullptr;
    brls::Label* m_attemptLabel = nullptr;
    brls::Label* m_progressLabel = nullptr;
    brls::Label* m_speedLabel = nullptr;
    brls::Rectangle* m_progressBg = nullptr;
    brls::Rectangle* m_progressBar = nullptr;
    brls::Button* m_cancelButton = nullptr;
    std::function<void()> m_cancelCallback;
    bool m_dismissed = false;

    // For speed calculation
    int64_t m_lastDownloaded = 0;
    std::chrono::steady_clock::time_point m_lastSpeedUpdate;
};

} // namespace vitaplex
