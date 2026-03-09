/**
 * VitaPlex - Debug Tab implementation
 * UI test tab with 15+ different dialog/notification style demos
 */

#include "view/debug_tab.hpp"
#include "view/progress_dialog.hpp"

namespace vitaplex {

DebugTab::DebugTab() {
    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setGrow(1.0f);

    m_scrollView = new brls::ScrollingFrame();
    m_scrollView->setGrow(1.0f);

    m_contentBox = new brls::Box();
    m_contentBox->setAxis(brls::Axis::COLUMN);
    m_contentBox->setPadding(20);
    m_contentBox->setGrow(1.0f);

    createDialogSection();
    createNotificationSection();
    createCustomDialogSection();

    m_scrollView->setContentView(m_contentBox);
    this->addView(m_scrollView);
}

// ─── Helper to create a button cell ───────────────────────────────────────────
static brls::DetailCell* makeButton(const std::string& title, const std::string& subtitle,
                                     std::function<void()> callback) {
    auto* cell = new brls::DetailCell();
    cell->setText(title);
    cell->setDetailText(subtitle);
    cell->registerClickAction([callback](brls::View* view) {
        callback();
        return true;
    });
    return cell;
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 1: Standard Dialog Styles
// ═══════════════════════════════════════════════════════════════════════════════

void DebugTab::createDialogSection() {
    auto* header = new brls::Header();
    header->setTitle("Dialog Styles");
    m_contentBox->addView(header);

    // 1. Basic Alert
    m_contentBox->addView(makeButton("1. Basic Alert", "Simple OK dialog", [this]() {
        showBasicAlert();
    }));

    // 2. Confirm Dialog
    m_contentBox->addView(makeButton("2. Confirm Dialog", "OK / Cancel two-button", [this]() {
        showConfirmDialog();
    }));

    // 3. Three-Button Dialog
    m_contentBox->addView(makeButton("3. Three-Button Dialog", "Three action choices", [this]() {
        showThreeButtonDialog();
    }));

    // 4. Long Text Dialog
    m_contentBox->addView(makeButton("4. Long Text Dialog", "Scrollable text content", [this]() {
        showLongTextDialog();
    }));

    // 5. Stacked Buttons Dialog
    m_contentBox->addView(makeButton("5. Stacked Buttons Dialog", "Many vertical buttons", [this]() {
        showStackedButtonsDialog();
    }));

    // 6. Custom Content Dialog
    m_contentBox->addView(makeButton("6. Custom Content Dialog", "Box with labels and toggles", [this]() {
        showCustomContentDialog();
    }));

    // 7. Progress-Style Dialog
    m_contentBox->addView(makeButton("7. Progress-Style Dialog", "Fake progress bar dialog", [this]() {
        showProgressStyleDialog();
    }));

    // 8. List Selection Dialog
    m_contentBox->addView(makeButton("8. List Selection Dialog", "Pick from a list of items", [this]() {
        showListSelectionDialog();
    }));

    // 9. Nested Dialog
    m_contentBox->addView(makeButton("9. Nested Dialog", "Dialog that opens another dialog", [this]() {
        showNestedDialog();
    }));

    // 10. Timed Auto-Close Dialog
    m_contentBox->addView(makeButton("10. Timed Auto-Close", "Closes after a few seconds", [this]() {
        showTimedAutoCloseDialog();
    }));

    // 11. Full-Width Content Dialog
    m_contentBox->addView(makeButton("11. Full-Width Content", "Wide custom content dialog", [this]() {
        showFullWidthDialog();
    }));

    // 12. Warning Dialog
    m_contentBox->addView(makeButton("12. Warning Dialog", "Yellow/orange warning style", [this]() {
        showWarningDialog();
    }));

    // 13. Error Dialog
    m_contentBox->addView(makeButton("13. Error Dialog", "Red error style", [this]() {
        showErrorDialog();
    }));

    // 14. Success Dialog
    m_contentBox->addView(makeButton("14. Success Dialog", "Green success style", [this]() {
        showSuccessDialog();
    }));

    // 15. Input Prompt Dialog
    m_contentBox->addView(makeButton("15. Input Prompt Dialog", "Simulated text input", [this]() {
        showInputPromptDialog();
    }));
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 2: Notification / Toast Styles
// ═══════════════════════════════════════════════════════════════════════════════

void DebugTab::createNotificationSection() {
    auto* header = new brls::Header();
    header->setTitle("Notification / Toast Styles");
    m_contentBox->addView(header);

    // 16. Basic notification toast
    m_contentBox->addView(makeButton("16. Basic Notification", "Simple toast message", [this]() {
        showBasicNotification();
    }));

    // 17. Long notification
    m_contentBox->addView(makeButton("17. Long Notification", "Multi-line toast", [this]() {
        showLongNotification();
    }));

    // 18. Rapid-fire notifications
    m_contentBox->addView(makeButton("18. Rapid-Fire Notifications", "Queue of 5 toasts", [this]() {
        showMultiNotifications();
    }));
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 3: Custom-Styled Dialogs
// ═══════════════════════════════════════════════════════════════════════════════

void DebugTab::createCustomDialogSection() {
    auto* header = new brls::Header();
    header->setTitle("Custom Styled Dialogs");
    m_contentBox->addView(header);

    auto* infoLabel = new brls::Label();
    infoLabel->setText("These demos test various borealis dialog features");
    infoLabel->setFontSize(14);
    infoLabel->setMarginLeft(16);
    infoLabel->setMarginTop(8);
    infoLabel->setMarginBottom(20);
    m_contentBox->addView(infoLabel);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Dialog Implementations
// ═══════════════════════════════════════════════════════════════════════════════

// 1. Basic Alert — single OK button
void DebugTab::showBasicAlert() {
    auto* dialog = new brls::Dialog("This is a basic alert dialog. Press OK to dismiss.");
    dialog->addButton("OK", [dialog]() {
        dialog->close();
    });
    dialog->open();
}

// 2. Confirm Dialog — OK / Cancel
void DebugTab::showConfirmDialog() {
    auto* dialog = new brls::Dialog("Do you want to perform this action?");

    dialog->addButton("Cancel", [dialog]() {
        dialog->close();
        brls::Application::notify("Action cancelled");
    });

    dialog->addButton("Confirm", [dialog]() {
        dialog->close();
        brls::Application::notify("Action confirmed!");
    });

    dialog->open();
}

// 3. Three-Button Dialog — Delete / Archive / Cancel
void DebugTab::showThreeButtonDialog() {
    auto* dialog = new brls::Dialog("What would you like to do with this item?");

    dialog->addButton("Cancel", [dialog]() {
        dialog->close();
    });

    dialog->addButton("Archive", [dialog]() {
        dialog->close();
        brls::Application::notify("Item archived");
    });

    dialog->addButton("Delete", [dialog]() {
        dialog->close();
        brls::Application::notify("Item deleted");
    });

    dialog->open();
}

// 4. Long Text Dialog — scrollable content
void DebugTab::showLongTextDialog() {
    brls::Box* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidth(500);
    content->setHeight(300);

    auto* scrollFrame = new brls::ScrollingFrame();
    scrollFrame->setGrow(1.0f);

    brls::Box* textBox = new brls::Box();
    textBox->setAxis(brls::Axis::COLUMN);
    textBox->setPadding(20);

    std::string longText =
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit. "
        "Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. "
        "Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris.\n\n"
        "Duis aute irure dolor in reprehenderit in voluptate velit esse cillum "
        "dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat non "
        "proident, sunt in culpa qui officia deserunt mollit anim id est laborum.\n\n"
        "Curabitur pretium tincidunt lacus. Nulla gravida orci a odio. Nullam "
        "varius, turpis et commodo pharetra, est eros bibendum elit.\n\n"
        "Praesent dapibus, neque id cursus faucibus, tortor neque egestas augue, "
        "eu vulputate magna eros eu erat. Aliquam erat volutpat. Nam dui mi, "
        "tincidunt quis, accumsan porttitor, facilisis luctus, metus.";

    auto* label = new brls::Label();
    label->setText(longText);
    label->setFontSize(16);
    textBox->addView(label);

    scrollFrame->setContentView(textBox);
    content->addView(scrollFrame);

    auto* dialog = new brls::Dialog(content);
    dialog->addButton("Close", [dialog]() {
        dialog->close();
    });
    dialog->open();
}

// 5. Stacked Buttons Dialog — many options
void DebugTab::showStackedButtonsDialog() {
    auto* dialog = new brls::Dialog("Select a quality setting:");

    dialog->addButton("240p", [dialog]() {
        dialog->close();
        brls::Application::notify("Selected: 240p");
    });
    dialog->addButton("480p", [dialog]() {
        dialog->close();
        brls::Application::notify("Selected: 480p");
    });
    dialog->addButton("720p", [dialog]() {
        dialog->close();
        brls::Application::notify("Selected: 720p");
    });
    dialog->addButton("1080p", [dialog]() {
        dialog->close();
        brls::Application::notify("Selected: 1080p");
    });
    dialog->addButton("Original", [dialog]() {
        dialog->close();
        brls::Application::notify("Selected: Original");
    });

    dialog->open();
}

// 6. Custom Content Dialog — toggles and labels
void DebugTab::showCustomContentDialog() {
    brls::Box* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidth(450);
    content->setHeight(320);
    content->setPadding(20);

    auto* titleLabel = new brls::Label();
    titleLabel->setText("Filter Options");
    titleLabel->setFontSize(22);
    titleLabel->setMarginBottom(15);
    content->addView(titleLabel);

    auto* toggle1 = new brls::BooleanCell();
    toggle1->init("Show Movies", true, [](bool v) {});
    content->addView(toggle1);

    auto* toggle2 = new brls::BooleanCell();
    toggle2->init("Show TV Shows", true, [](bool v) {});
    content->addView(toggle2);

    auto* toggle3 = new brls::BooleanCell();
    toggle3->init("Show Music", false, [](bool v) {});
    content->addView(toggle3);

    auto* toggle4 = new brls::BooleanCell();
    toggle4->init("Show Photos", false, [](bool v) {});
    content->addView(toggle4);

    auto* infoLabel = new brls::Label();
    infoLabel->setText("Toggle categories to filter content");
    infoLabel->setFontSize(14);
    infoLabel->setMarginTop(10);
    content->addView(infoLabel);

    auto* dialog = new brls::Dialog(content);
    dialog->addButton("Cancel", [dialog]() {
        dialog->close();
    });
    dialog->addButton("Apply", [dialog]() {
        dialog->close();
        brls::Application::notify("Filters applied!");
    });
    dialog->open();
}

// 7. Progress-Style Dialog — fake progress bar
void DebugTab::showProgressStyleDialog() {
    brls::Box* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidth(400);
    content->setHeight(180);
    content->setPadding(25);

    auto* titleLabel = new brls::Label();
    titleLabel->setText("Downloading...");
    titleLabel->setFontSize(20);
    titleLabel->setMarginBottom(15);
    content->addView(titleLabel);

    auto* statusLabel = new brls::Label();
    statusLabel->setText("movie_file.mp4");
    statusLabel->setFontSize(16);
    statusLabel->setMarginBottom(15);
    content->addView(statusLabel);

    // Progress bar background
    auto* progressBg = new brls::Rectangle();
    progressBg->setHeight(12);
    progressBg->setColor(nvgRGBA(80, 80, 80, 255));
    progressBg->setCornerRadius(6);
    content->addView(progressBg);

    // Progress bar fill (overlay style using a second rectangle)
    auto* progressBar = new brls::Rectangle();
    progressBar->setHeight(12);
    progressBar->setWidth(250);
    progressBar->setColor(nvgRGBA(0, 150, 255, 255));
    progressBar->setCornerRadius(6);
    progressBar->setMarginTop(-12);
    content->addView(progressBar);

    auto* percentLabel = new brls::Label();
    percentLabel->setText("63% - 2.4 MB/s");
    percentLabel->setFontSize(14);
    percentLabel->setMarginTop(10);
    content->addView(percentLabel);

    auto* dialog = new brls::Dialog(content);
    dialog->addButton("Cancel", [dialog]() {
        dialog->close();
        brls::Application::notify("Download cancelled");
    });
    dialog->open();
}

// 8. List Selection Dialog — pick from a list
void DebugTab::showListSelectionDialog() {
    brls::Box* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidth(400);
    content->setHeight(350);

    auto* titleLabel = new brls::Label();
    titleLabel->setText("Select a server:");
    titleLabel->setFontSize(20);
    titleLabel->setMarginBottom(10);
    titleLabel->setMarginLeft(20);
    titleLabel->setMarginTop(20);
    content->addView(titleLabel);

    auto* scrollFrame = new brls::ScrollingFrame();
    scrollFrame->setGrow(1.0f);

    brls::Box* listBox = new brls::Box();
    listBox->setAxis(brls::Axis::COLUMN);
    listBox->setPaddingLeft(20);
    listBox->setPaddingRight(20);

    std::vector<std::string> servers = {
        "Living Room Plex", "Bedroom Server", "Office NAS",
        "Cloud Server (Remote)", "Backup Media Server",
        "Friend's Server", "Test Server"
    };

    for (const auto& server : servers) {
        auto* cell = new brls::DetailCell();
        cell->setText(server);
        cell->setDetailText("Available");
        listBox->addView(cell);
    }

    scrollFrame->setContentView(listBox);
    content->addView(scrollFrame);

    auto* dialog = new brls::Dialog(content);
    dialog->addButton("Cancel", [dialog]() {
        dialog->close();
    });
    dialog->open();
}

// 9. Nested Dialog — dialog within dialog
void DebugTab::showNestedDialog() {
    auto* dialog = new brls::Dialog("This dialog will open another dialog. Ready?");

    dialog->addButton("Cancel", [dialog]() {
        dialog->close();
    });

    dialog->addButton("Open Inner", [dialog]() {
        dialog->close();

        auto* inner = new brls::Dialog("This is the inner dialog! You navigated one level deeper.");
        inner->addButton("OK", [inner]() {
            inner->close();
            brls::Application::notify("Returned from nested dialogs");
        });
        inner->open();
    });

    dialog->open();
}

// 10. Timed Auto-Close Dialog (manual dismiss only — Vita has no async timer API)
void DebugTab::showTimedAutoCloseDialog() {
    auto* dialog = new brls::Dialog(
        "On a full platform this dialog would auto-close after a timeout.\n"
        "This demonstrates a dialog intended for transient status messages.");

    dialog->addButton("Dismiss", [dialog]() {
        dialog->close();
        brls::Application::notify("Transient dialog dismissed");
    });

    dialog->open();
}

// 11. Full-Width Content Dialog — wide layout
void DebugTab::showFullWidthDialog() {
    brls::Box* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidth(700);
    content->setHeight(250);
    content->setPadding(25);

    auto* titleLabel = new brls::Label();
    titleLabel->setText("System Information");
    titleLabel->setFontSize(22);
    titleLabel->setMarginBottom(15);
    content->addView(titleLabel);

    // Two-column info layout
    auto* row1 = new brls::Box();
    row1->setAxis(brls::Axis::ROW);
    row1->setMarginBottom(8);
    auto* lbl1a = new brls::Label();
    lbl1a->setText("Platform:");
    lbl1a->setFontSize(16);
    lbl1a->setWidth(200);
    row1->addView(lbl1a);
    auto* lbl1b = new brls::Label();
    lbl1b->setText("PlayStation Vita");
    lbl1b->setFontSize(16);
    row1->addView(lbl1b);
    content->addView(row1);

    auto* row2 = new brls::Box();
    row2->setAxis(brls::Axis::ROW);
    row2->setMarginBottom(8);
    auto* lbl2a = new brls::Label();
    lbl2a->setText("UI Framework:");
    lbl2a->setFontSize(16);
    lbl2a->setWidth(200);
    row2->addView(lbl2a);
    auto* lbl2b = new brls::Label();
    lbl2b->setText("Borealis");
    lbl2b->setFontSize(16);
    row2->addView(lbl2b);
    content->addView(row2);

    auto* row3 = new brls::Box();
    row3->setAxis(brls::Axis::ROW);
    row3->setMarginBottom(8);
    auto* lbl3a = new brls::Label();
    lbl3a->setText("App:");
    lbl3a->setFontSize(16);
    lbl3a->setWidth(200);
    row3->addView(lbl3a);
    auto* lbl3b = new brls::Label();
    lbl3b->setText("VitaPlex Debug Tab");
    lbl3b->setFontSize(16);
    row3->addView(lbl3b);
    content->addView(row3);

    auto* row4 = new brls::Box();
    row4->setAxis(brls::Axis::ROW);
    row4->setMarginBottom(8);
    auto* lbl4a = new brls::Label();
    lbl4a->setText("Renderer:");
    lbl4a->setFontSize(16);
    lbl4a->setWidth(200);
    row4->addView(lbl4a);
    auto* lbl4b = new brls::Label();
    lbl4b->setText("GXM (Native)");
    lbl4b->setFontSize(16);
    row4->addView(lbl4b);
    content->addView(row4);

    auto* dialog = new brls::Dialog(content);
    dialog->addButton("Close", [dialog]() {
        dialog->close();
    });
    dialog->open();
}

// 12. Warning Dialog — styled with colored rectangle header
void DebugTab::showWarningDialog() {
    brls::Box* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidth(450);
    content->setHeight(200);

    // Orange warning banner
    auto* banner = new brls::Rectangle();
    banner->setHeight(50);
    banner->setColor(nvgRGBA(230, 160, 0, 255));
    content->addView(banner);

    auto* warningTitle = new brls::Label();
    warningTitle->setText("WARNING");
    warningTitle->setFontSize(20);
    warningTitle->setMarginTop(-40);
    warningTitle->setMarginLeft(20);
    content->addView(warningTitle);

    auto* msgLabel = new brls::Label();
    msgLabel->setText("This action may cause data loss. Cached metadata and thumbnails will be removed. Continue?");
    msgLabel->setFontSize(16);
    msgLabel->setMarginTop(20);
    msgLabel->setMarginLeft(20);
    msgLabel->setMarginRight(20);
    content->addView(msgLabel);

    auto* dialog = new brls::Dialog(content);
    dialog->addButton("Cancel", [dialog]() {
        dialog->close();
    });
    dialog->addButton("Continue", [dialog]() {
        dialog->close();
        brls::Application::notify("Warning acknowledged");
    });
    dialog->open();
}

// 13. Error Dialog — red themed
void DebugTab::showErrorDialog() {
    brls::Box* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidth(450);
    content->setHeight(220);

    // Red error banner
    auto* banner = new brls::Rectangle();
    banner->setHeight(50);
    banner->setColor(nvgRGBA(200, 40, 40, 255));
    content->addView(banner);

    auto* errorTitle = new brls::Label();
    errorTitle->setText("ERROR");
    errorTitle->setFontSize(20);
    errorTitle->setMarginTop(-40);
    errorTitle->setMarginLeft(20);
    content->addView(errorTitle);

    auto* msgLabel = new brls::Label();
    msgLabel->setText("Connection to server failed.\nError code: TIMEOUT_408\nPlease check your network settings and try again.");
    msgLabel->setFontSize(16);
    msgLabel->setMarginTop(20);
    msgLabel->setMarginLeft(20);
    msgLabel->setMarginRight(20);
    content->addView(msgLabel);

    auto* dialog = new brls::Dialog(content);
    dialog->addButton("Retry", [dialog]() {
        dialog->close();
        brls::Application::notify("Retrying connection...");
    });
    dialog->addButton("Dismiss", [dialog]() {
        dialog->close();
    });
    dialog->open();
}

// 14. Success Dialog — green themed
void DebugTab::showSuccessDialog() {
    brls::Box* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidth(450);
    content->setHeight(200);

    // Green success banner
    auto* banner = new brls::Rectangle();
    banner->setHeight(50);
    banner->setColor(nvgRGBA(40, 180, 60, 255));
    content->addView(banner);

    auto* successTitle = new brls::Label();
    successTitle->setText("SUCCESS");
    successTitle->setFontSize(20);
    successTitle->setMarginTop(-40);
    successTitle->setMarginLeft(20);
    content->addView(successTitle);

    auto* msgLabel = new brls::Label();
    msgLabel->setText("Download completed successfully!\nFile saved to: ux0:data/VitaPlex/downloads/\nSize: 1.2 GB");
    msgLabel->setFontSize(16);
    msgLabel->setMarginTop(20);
    msgLabel->setMarginLeft(20);
    msgLabel->setMarginRight(20);
    content->addView(msgLabel);

    auto* dialog = new brls::Dialog(content);
    dialog->addButton("OK", [dialog]() {
        dialog->close();
    });
    dialog->open();
}

// 15. Input Prompt Dialog — simulated text input field
void DebugTab::showInputPromptDialog() {
    brls::Box* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidth(450);
    content->setHeight(200);
    content->setPadding(20);

    auto* titleLabel = new brls::Label();
    titleLabel->setText("Enter Server URL");
    titleLabel->setFontSize(20);
    titleLabel->setMarginBottom(15);
    content->addView(titleLabel);

    auto* descLabel = new brls::Label();
    descLabel->setText("Provide the address of your Plex Media Server:");
    descLabel->setFontSize(14);
    descLabel->setMarginBottom(15);
    content->addView(descLabel);

    // Simulated input field (rectangle with text)
    auto* inputBg = new brls::Rectangle();
    inputBg->setHeight(40);
    inputBg->setColor(nvgRGBA(50, 50, 50, 255));
    inputBg->setCornerRadius(8);
    content->addView(inputBg);

    auto* inputText = new brls::Label();
    inputText->setText("http://192.168.1.100:32400");
    inputText->setFontSize(16);
    inputText->setMarginTop(-32);
    inputText->setMarginLeft(12);
    content->addView(inputText);

    auto* hintLabel = new brls::Label();
    hintLabel->setText("(On real Vita this would open the OSK keyboard)");
    hintLabel->setFontSize(12);
    hintLabel->setMarginTop(15);
    content->addView(hintLabel);

    auto* dialog = new brls::Dialog(content);
    dialog->addButton("Cancel", [dialog]() {
        dialog->close();
    });
    dialog->addButton("Connect", [dialog]() {
        dialog->close();
        brls::Application::notify("Connecting to server...");
    });
    dialog->open();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Notification Implementations
// ═══════════════════════════════════════════════════════════════════════════════

// 16. Basic notification toast
void DebugTab::showBasicNotification() {
    brls::Application::notify("This is a basic notification toast!");
}

// 17. Long notification
void DebugTab::showLongNotification() {
    brls::Application::notify("This is a longer notification message that tests how the toast handles multi-line or wrapping text on the PS Vita screen");
}

// 18. Rapid-fire notifications
void DebugTab::showMultiNotifications() {
    brls::Application::notify("Notification 1 of 5: Starting...");
    brls::Application::notify("Notification 2 of 5: Loading data...");
    brls::Application::notify("Notification 3 of 5: Processing...");
    brls::Application::notify("Notification 4 of 5: Almost done...");
    brls::Application::notify("Notification 5 of 5: Complete!");
}

} // namespace vitaplex
