/**
 * VitaSuwayomi - Long Press Gesture Recognizer implementation
 */

#include "view/long_press_gesture.hpp"

namespace vitasuwayomi {

LongPressGestureRecognizer::LongPressGestureRecognizer(brls::View* view, Callback callback, int holdDurationMs)
    : m_callback(callback)
    , m_view(view)
    , m_holdDurationMs(holdDurationMs)
{
    this->state = brls::GestureState::FAILED;
}

brls::GestureState LongPressGestureRecognizer::recognitionLoop(
    brls::TouchState touch, brls::MouseState mouse, brls::View* view, brls::Sound* soundToPlay)
{
    brls::TouchPhase phase = touch.phase;
    brls::Point position = touch.position;

    // Also handle mouse
    if (phase == brls::TouchPhase::NONE) {
        position = mouse.position;
        phase = mouse.leftButton;
    }

    if (!enabled || phase == brls::TouchPhase::NONE) {
        m_tracking = false;
        this->state = brls::GestureState::FAILED;
        return this->state;
    }

    // If already interrupted, stay that way
    if (this->state == brls::GestureState::INTERRUPTED) {
        m_tracking = false;
        return this->state;
    }

    switch (phase) {
        case brls::TouchPhase::START: {
            // Start tracking
            m_tracking = true;
            m_triggered = false;
            m_startPosition = position;
            m_startTime = std::chrono::steady_clock::now();
            this->state = brls::GestureState::UNSURE;
            break;
        }

        case brls::TouchPhase::STAY: {
            if (!m_tracking || m_triggered) {
                break;
            }

            // Check if touch is still within view bounds
            if (position.x < view->getX() || position.x > view->getX() + view->getWidth() ||
                position.y < view->getY() || position.y > view->getY() + view->getHeight()) {
                // Touch moved outside view
                m_tracking = false;
                this->state = brls::GestureState::FAILED;
                break;
            }

            // Check if touch moved too much
            float dx = position.x - m_startPosition.x;
            float dy = position.y - m_startPosition.y;
            float distance = std::sqrt(dx * dx + dy * dy);

            if (distance > MAX_MOVEMENT) {
                // User is moving/swiping, not holding
                m_tracking = false;
                this->state = brls::GestureState::FAILED;
                break;
            }

            // Check if held long enough
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_startTime).count();

            if (elapsed >= m_holdDurationMs) {
                // Long press detected!
                m_triggered = true;
                this->state = brls::GestureState::START;

                LongPressGestureStatus status{
                    .state = brls::GestureState::START,
                    .position = position
                };

                if (m_callback) {
                    m_callback(status);
                }

                // Play a sound to indicate long press triggered
                *soundToPlay = brls::SOUND_CLICK;
            }
            break;
        }

        case brls::TouchPhase::END: {
            if (m_triggered) {
                // Long press was already handled
                this->state = brls::GestureState::END;

                LongPressGestureStatus status{
                    .state = brls::GestureState::END,
                    .position = position
                };

                if (m_callback) {
                    m_callback(status);
                }
            } else {
                // User released before long press threshold
                this->state = brls::GestureState::FAILED;
            }
            m_tracking = false;
            break;
        }

        case brls::TouchPhase::NONE:
            m_tracking = false;
            this->state = brls::GestureState::FAILED;
            break;
    }

    return this->state;
}

} // namespace vitasuwayomi
