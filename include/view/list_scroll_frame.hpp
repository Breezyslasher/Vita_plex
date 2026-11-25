#pragma once

/**
 * VitaPlex - a vertical list that steps row by row
 *
 * brls::ScrollingFrame's default ("natural") scrolling slides the list for as
 * long as an arrow is held and moves focus only once the next row is wholly
 * in view, so a press onto a row below the edge scrolled several rows' worth
 * (Settings, Manage Hidden Libraries). This one moves focus row by row and
 * scrolls only as far as needed to show the focused row, as the rails do
 * (see rail_scroll_frame.hpp). Touch scrolling is ScrollingFrame's own.
 */

#include <borealis.hpp>

#include <algorithm>

namespace vitaplex {

class ListScrollFrame : public brls::ScrollingFrame {
  public:
    ListScrollFrame() {
        // Focus moves row to row (natural mode's hold-to-slide is off); the
        // scrolling is done below instead of centring.
        setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    }

    void onChildFocusGained(brls::View* directChild, brls::View* focusedView) override {
        // Not ScrollingFrame's: it would centre the row.
        brls::Box::onChildFocusGained(directChild, focusedView);
        if (brls::Application::getInputType() != brls::InputType::GAMEPAD) return;
        if (!directChild || !focusedView) return;

        float top = 0.0f;
        for (brls::View* v = focusedView; v && v != directChild; v = v->getParent())
            top += v->getLocalY();
        const float bottom = top + focusedView->getHeight();

        const float visible = getScrollingAreaHeight();
        const float maxOffset = std::max(0.0f, getContentHeight() - visible);
        float offset = getContentOffsetY();
        if (top < offset)
            offset = top;
        else if (bottom > offset + visible)
            offset = bottom - visible;
        offset = std::clamp(offset, 0.0f, maxOffset);
        if (offset != getContentOffsetY()) setContentOffsetY(offset, true);
    }

    brls::View* getNextFocus(brls::FocusDirection direction, brls::View* currentView) override {
        // Not ScrollingFrame's, which hands focus to the list itself while it
        // can still scroll: past the last row, focus leaves the list.
        return brls::Box::getNextFocus(direction, currentView);
    }
};

}  // namespace vitaplex
