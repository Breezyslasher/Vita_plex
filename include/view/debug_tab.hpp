/**
 * VitaPlex - Debug Tab
 * UI test tab with buttons to show various dialog and notification styles
 */

#pragma once

#include <borealis.hpp>

namespace vitaplex {

class DebugTab : public brls::Box {
public:
    DebugTab();

private:
    void createDialogSection();
    void createNotificationSection();
    void createCustomDialogSection();

    brls::ScrollingFrame* m_scrollView = nullptr;
    brls::Box* m_contentBox = nullptr;

    // Dialog demos
    void showBasicAlert();
    void showConfirmDialog();
    void showThreeButtonDialog();
    void showLongTextDialog();
    void showStackedButtonsDialog();
    void showCustomContentDialog();
    void showProgressStyleDialog();
    void showListSelectionDialog();
    void showNestedDialog();
    void showTimedAutoCloseDialog();
    void showFullWidthDialog();
    void showWarningDialog();
    void showErrorDialog();
    void showSuccessDialog();
    void showInputPromptDialog();

    // Notification demos
    void showBasicNotification();
    void showLongNotification();
    void showMultiNotifications();
};

} // namespace vitaplex
