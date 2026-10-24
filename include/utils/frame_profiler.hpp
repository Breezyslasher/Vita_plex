#pragma once

// Where the frames go. borealis (patches/brls_application.cpp) times each
// part of every main-loop iteration; this adds what the app knows (its own
// draw probes, textures decoded on the UI thread, HTTP requests) and logs a
// summary every few seconds when the frame rate drops, plus a breakdown of
// any slow frame. A quiet window logs nothing but a heartbeat each minute.

#include <cstdint>

namespace vitaplex {
namespace perf {

// Timed on the UI thread, per frame.
enum class Probe {
    HomeDraw,       // HomeTab::draw, its rails and cards
    CellDraw,       // MediaItemCell::draw (counted too)
    TextureDecode,  // an image decoded and uploaded on the UI thread
    Count
};

// Sets borealis's frame-timings callback. Call once, before the main loop.
void install();

// UI thread only: `us` spent in `probe` this frame, one more of it.
void addUi(Probe probe, int64_t us);

// Any thread: an HTTP request begun, and one ended.
void httpBegun();
void httpEnded();

// UI thread: times its scope into a probe.
class Scope {
public:
    explicit Scope(Probe probe);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Probe m_probe;
    int64_t m_start;
};

}  // namespace perf
}  // namespace vitaplex
