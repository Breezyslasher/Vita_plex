/**
 * VitaPlex - Semi-Transparent Overlay Dialog
 * Configurable backdrop opacity dialog that renders as a full-screen overlay
 */

#pragma once

#include <borealis.hpp>
#include <functional>
#include <string>

namespace vitaplex {

class OverlayDialog : public brls::Box {
public:
    /**
     * Create a semi-transparent overlay dialog.
     * @param title       Dialog title text
     * @param message     Dialog message body
     * @param backdropAlpha Backdrop opacity (0 = fully transparent, 255 = fully opaque)
     */
    OverlayDialog(const std::string& title, const std::string& message, uint8_t backdropAlpha = 128);
    ~OverlayDialog();

    // Set the content box background opacity (0-255)
    void setContentAlpha(uint8_t alpha);

    // Add a button to the dialog
    void addButton(const std::string& text, std::function<void()> callback);

    // Set custom content instead of message label
    void setCustomContent(brls::Box* content);

    // Show/dismiss the overlay
    void show();
    void dismiss();

private:
    brls::Box* m_container = nullptr;
    brls::Label* m_titleLabel = nullptr;
    brls::Label* m_messageLabel = nullptr;
    brls::Box* m_buttonBox = nullptr;
    brls::Box* m_contentArea = nullptr;
    bool m_dismissed = false;
    uint8_t m_backdropAlpha;
    uint8_t m_contentAlpha = 230;
};

} // namespace vitaplex
