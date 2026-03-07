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
    // For LEFT/RIGHT, use index-based navigation instead of spatial lookup.
    // Spatial lookup fails because children are translated off-screen via
    // setTranslationX() for scrolling, which confuses position-based focus finding.
    if (direction == brls::FocusDirection::LEFT || direction == brls::FocusDirection::RIGHT) {
        auto& children = this->getChildren();
        if (children.empty()) return nullptr;

        // Find the index of the currently focused child
        int currentIndex = -1;
        for (size_t i = 0; i < children.size(); i++) {
            if (children[i] == currentView) {
                currentIndex = (int)i;
                break;
            }
        }

        if (currentIndex < 0) {
            // currentView not a direct child - try the default
            return brls::Box::getNextFocus(direction, currentView);
        }

        int nextIndex = currentIndex + (direction == brls::FocusDirection::RIGHT ? 1 : -1);

        if (nextIndex < 0 || nextIndex >= (int)children.size()) {
            // At boundary - return nullptr so focus bubbles up to parent
            // (e.g. sidebar for LEFT at first item)
            return nullptr;
        }

        brls::View* nextFocus = children[nextIndex];
        brls::sync([this, nextFocus]() {
            scrollToView(nextFocus);
        });
        return nextFocus;
    }

    // For UP/DOWN, let borealis find the target (respects custom navigation routes).
    // Then scroll to make the target visible, since it may be off-screen due to
    // setTranslationX() scrolling.
    brls::View* nextFocus = brls::Box::getNextFocus(direction, currentView);
    if (nextFocus) {
        // If the target is a child of THIS row (focus coming into this row from
        // above/below), scroll to make it visible. Reset to first item since
        // custom routes point to children[0] which may be scrolled off.
        auto& children = this->getChildren();
        for (auto* child : children) {
            if (child == nextFocus) {
                brls::sync([this, nextFocus]() {
                    scrollToView(nextFocus);
                });
                break;
            }
        }
    }
    return nextFocus;
}

} // namespace vitaplex
