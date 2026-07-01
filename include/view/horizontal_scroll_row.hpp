/**
 * VitaPlex - Horizontal Scroll Row
 * A horizontal scrollable row for touch and d-pad navigation.
 * Unlike HScrollingFrame, this properly delegates focus navigation
 * to parent views (e.g. sidebar) when at row boundaries.
 */

#pragma once

#include <borealis.hpp>

namespace vitaplex {

class HorizontalScrollRow : public brls::Box {
public:
    HorizontalScrollRow();

    void onLayout() override;
    // Culls children scrolled out of the row before drawing. clipsToBounds
    // only hides them visually — every off-screen card still ran its full
    // draw() (cover pattern, badge paths, per-frame text measurement) every
    // frame, which is what made card-heavy tabs slow on Vita.
    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;
    brls::View* getNextFocus(brls::FocusDirection direction, brls::View* currentView) override;

    void setScrollOffset(float offset);
    float getScrollOffset() const { return m_scrollOffset; }

    void scrollToView(brls::View* view);

private:
    void onPan(brls::PanGestureStatus status, brls::Sound* sound);
    void updateScroll();

    float m_scrollOffset = 0.0f;
    float m_contentWidth = 0.0f;
    float m_visibleWidth = 0.0f;
    float m_panStartOffset = 0.0f;
};

} // namespace vitaplex
