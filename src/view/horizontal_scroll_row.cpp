/**
 * VitaPlex - Horizontal Scroll Row implementation
 */

#include "view/horizontal_scroll_row.hpp"

namespace vitaplex {

HorizontalScrollRow::HorizontalScrollRow() {
    this->setAxis(brls::Axis::ROW);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::CENTER);
    this->setClipsToBounds(true);

    // Add pan gesture for touch scrolling
    this->addGestureRecognizer(new brls::PanGestureRecognizer(
        [this](brls::PanGestureStatus status, brls::Sound* soundToPlay) {
            onPan(status, soundToPlay);
        }, brls::PanAxis::HORIZONTAL));
}

void HorizontalScrollRow::onLayout() {
    brls::Box::onLayout();

    // Calculate content width from children
    m_contentWidth = 0.0f;
    for (auto* child : this->getChildren()) {
        m_contentWidth += child->getWidth() + child->getMarginLeft() + child->getMarginRight();
    }

    m_visibleWidth = this->getWidth();

    // Ensure scroll offset is valid
    float maxOffset = std::max(0.0f, m_contentWidth - m_visibleWidth);
    m_scrollOffset = std::max(0.0f, std::min(m_scrollOffset, maxOffset));

    updateScroll();
}

static bool rowIsDescendantOf(brls::View* view, brls::View* ancestor) {
    for (brls::View* v = view; v; v = v->getParent())
        if (v == ancestor) return true;
    return false;
}

void HorizontalScrollRow::draw(NVGcontext* vg, float x, float y, float width, float height,
                               brls::Style style, brls::FrameContext* ctx) {
    // Toggle INVISIBLE on cards scrolled out of the row so View::frame()
    // early-outs their whole subtree (INVISIBLE<->VISIBLE never touches
    // layout — only GONE does). clipsToBounds alone only hides them
    // visually: every off-screen card still ran its full draw() (cover
    // pattern, badge paths, per-frame text measurement) every frame.
    // The margin keeps the immediate off-screen neighbours drawable so
    // directional focus can still reach them (borealis navigation skips
    // non-VISIBLE views); once focused they scroll in and the next pass
    // reveals the following ones.
    const float margin  = 300.0f;
    const float rowLeft = this->getX();
    const float rowRight = rowLeft + this->getWidth();
    brls::View* focus = brls::Application::getCurrentFocus();

    for (brls::View* child : this->getChildren()) {
        const brls::Visibility v = child->getVisibility();
        if (v == brls::Visibility::GONE) continue;  // hidden by owner, keep as-is

        const float cLeft  = child->getX();
        const float cRight = cLeft + child->getWidth();
        bool visible = (cRight >= rowLeft - margin) && (cLeft <= rowRight + margin);
        if (!visible && focus && rowIsDescendantOf(focus, child))
            visible = true;

        const brls::Visibility want = visible ? brls::Visibility::VISIBLE
                                              : brls::Visibility::INVISIBLE;
        if (v != want) child->setVisibility(want);
    }

    brls::Box::draw(vg, x, y, width, height, style, ctx);
}

void HorizontalScrollRow::onPan(brls::PanGestureStatus status, brls::Sound* sound) {
    if (status.state == brls::GestureState::START) {
        m_panStartOffset = m_scrollOffset;
    }
    else if (status.state == brls::GestureState::STAY || status.state == brls::GestureState::END) {
        float deltaX = status.position.x - status.startPosition.x;
        float newOffset = m_panStartOffset - deltaX;

        float maxOffset = std::max(0.0f, m_contentWidth - m_visibleWidth);
        m_scrollOffset = std::max(0.0f, std::min(newOffset, maxOffset));

        updateScroll();
    }
}

void HorizontalScrollRow::updateScroll() {
    for (auto* child : this->getChildren()) {
        child->setTranslationX(-m_scrollOffset);
    }
}

void HorizontalScrollRow::setScrollOffset(float offset) {
    float maxOffset = std::max(0.0f, m_contentWidth - m_visibleWidth);
    m_scrollOffset = std::max(0.0f, std::min(offset, maxOffset));
    updateScroll();
}

void HorizontalScrollRow::scrollToView(brls::View* targetView) {
    if (!targetView) return;

    float viewLeft = 0.0f;
    bool found = false;

    for (auto* child : this->getChildren()) {
        if (child == targetView) {
            found = true;
            break;
        }
        viewLeft += child->getWidth() + child->getMarginLeft() + child->getMarginRight();
    }

    if (!found) return;

    float viewRight = viewLeft + targetView->getWidth();

    float visibleLeft = m_scrollOffset;
    float visibleRight = m_scrollOffset + m_visibleWidth;

    if (viewLeft < visibleLeft) {
        setScrollOffset(viewLeft);
    }
    else if (viewRight > visibleRight) {
        setScrollOffset(viewRight - m_visibleWidth);
    }
}

brls::View* HorizontalScrollRow::getNextFocus(brls::FocusDirection direction, brls::View* currentView) {
    // Get the default next focus from borealis
    brls::View* nextFocus = brls::Box::getNextFocus(direction, currentView);

    // If navigating left/right within this row, scroll to keep focused view visible
    if (nextFocus && (direction == brls::FocusDirection::LEFT || direction == brls::FocusDirection::RIGHT)) {
        brls::sync([this, nextFocus]() {
            scrollToView(nextFocus);
        });
    }

    return nextFocus;
}

} // namespace vitaplex
