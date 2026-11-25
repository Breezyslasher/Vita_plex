#pragma once

/**
 * VitaPlex - a horizontal rail of cards that steps like the Home rows
 *
 * brls::HScrollingFrame's default ("natural") scrolling slides a rail for as
 * long as an arrow is held and moves focus only once the next card is wholly
 * in view, so how far a press goes depends on how long it is held (a remote
 * that sends instant presses only nudged the rail). Its "centered" mode moves
 * focus card by card but keeps the focused card in the middle.
 *
 * This one moves focus card by card, as the Home rows (HorizontalScrollRow)
 * do, and scrolls only as far as needed to show the focused card: not at all
 * while it is already in view, to its edge when it is not. Touch scrolling is
 * HScrollingFrame's own.
 */

#include <borealis.hpp>

#include <algorithm>

namespace vitaplex {

class RailScrollFrame : public brls::HScrollingFrame {
  public:
    RailScrollFrame() {
        // Focus moves card to card (the natural mode's hold-to-slide is off);
        // the scrolling itself is done below instead of centring.
        setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    }

    void onChildFocusGained(brls::View* directChild, brls::View* focusedView) override {
        // Not HScrollingFrame's: it would centre the card.
        brls::Box::onChildFocusGained(directChild, focusedView);
        if (brls::Application::getInputType() != brls::InputType::GAMEPAD) return;
        if (!directChild || !focusedView) return;

        // The card's place in the rail's content.
        float left = 0.0f;
        for (brls::View* v = focusedView; v && v != directChild; v = v->getParent())
            left += v->getLocalX();
        const float right = left + focusedView->getWidth();

        const float visible = getWidth();
        const float maxOffset = std::max(0.0f, directChild->getWidth() - visible);
        float offset = getContentOffsetX();
        if (left < offset)
            offset = left;
        else if (right > offset + visible)
            offset = right - visible;
        offset = std::clamp(offset, 0.0f, maxOffset);
        if (offset != getContentOffsetX()) setContentOffsetX(offset, true);
    }

    brls::View* getNextFocus(brls::FocusDirection direction, brls::View* currentView) override {
        // Not HScrollingFrame's, which hands focus to the rail itself while
        // it can still scroll: past the last card, focus leaves the rail.
        return brls::Box::getNextFocus(direction, currentView);
    }
};

}  // namespace vitaplex
