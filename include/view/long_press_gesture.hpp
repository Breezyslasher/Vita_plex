/**
 * VitaSuwayomi - Long Press Gesture Recognizer
 * Detects when user holds touch on a view for a specified duration
 */

#pragma once

#include <borealis.hpp>
#include <chrono>
#include <functional>

namespace vitasuwayomi {

struct LongPressGestureStatus {
    brls::GestureState state;
    brls::Point position;
};

// Long press gesture recognizer
// Fires when user holds touch for a specified duration without moving
class LongPressGestureRecognizer : public brls::GestureRecognizer {
public:
    using Callback = std::function<void(LongPressGestureStatus)>;

    // Constructor with callback and optional hold duration
    // Default hold duration is 400ms
    LongPressGestureRecognizer(brls::View* view, Callback callback, int holdDurationMs = 400);

    brls::GestureState recognitionLoop(brls::TouchState touch, brls::MouseState mouse,
                                        brls::View* view, brls::Sound* soundToPlay) override;

    // Get whether the long press was triggered (to prevent tap from firing)
    bool wasTriggered() const { return m_triggered; }

    // Reset the triggered state
    void resetTriggered() { m_triggered = false; }

private:
    Callback m_callback;
    brls::View* m_view;
    int m_holdDurationMs;

    brls::Point m_startPosition;
    std::chrono::steady_clock::time_point m_startTime;
    bool m_tracking = false;
    bool m_triggered = false;

    static constexpr float MAX_MOVEMENT = 25.0f;  // Max pixels allowed to move while holding (Vita touchscreen has jitter)
};

} // namespace vitasuwayomi
