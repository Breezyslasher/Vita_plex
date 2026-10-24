#include "utils/frame_profiler.hpp"

#include <borealis.hpp>

#include <algorithm>
#include <atomic>
#include <map>
#include <string>
#include <typeinfo>
#include <cxxabi.h>
#include <cstdlib>
#include <vector>

#include <yoga/event/event.h>

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#endif

// nanovg's own counts for the frame (patches/nanovg.c); nanovg.h does not
// declare it.
extern "C" void nvgFrameCounts(NVGcontext* ctx, int* drawCalls, int* fillTris, int* strokeTris,
                               int* textTris);

namespace vitaplex {
namespace perf {

namespace {

using Timings = brls::Application::FrameTimings;

const int64_t kWindowUs = 5 * 1000 * 1000;      // one summary per 5 s at most
const int64_t kHeartbeatUs = 60 * 1000 * 1000;  // a quiet window logs once a minute
const int64_t kSlowFrameUs = 50 * 1000;         // a frame this long is logged on its own
const int64_t kSlowGapUs = 2 * 1000 * 1000;     // at most one slow frame line per 2 s
const int kLowFps = 55;                         // a window below this is logged

// The parts of an iteration, as borealis times them.
// "input/..." are the focus changes' parts, within input; "layout" is what
// was left to lay out before drawing (borealis lays out on demand).
const int kParts = 16;
const char* const kPartNames[kParts] = {"poll",    "input",  "anim",       "begin",
                                        "views",   "nvg",    "swap",       "sync",
                                        "runloop", "delete", "outside",    "input/focus-lost",
                                        "input/focus-event", "input/focus-gained",
                                        "input/hints", "layout"};

// How often one frame is drawn with each view's drawing timed by type
// (Application::sampleViewDraws): the timing itself costs a little.
const int64_t kViewSampleEveryUs = 1000 * 1000;

struct Frame {
    int64_t part[kParts] = {};
    int64_t total = 0;  // from one iteration's start to the next's
    int64_t probeUs[(int)Probe::Count] = {};
    int probeN[(int)Probe::Count] = {};
    int drawCalls = 0, tris = 0, textTris = 0;
    size_t tickings = 0;
};

struct Window {
    int64_t startUs = 0;
    int frames = 0;
    int64_t partSum[kParts] = {}, partMax[kParts] = {};
    int64_t totalSum = 0;
    int over33 = 0, over50 = 0;
    Frame worst;
    int64_t probeUs[(int)Probe::Count] = {};
    int probeN[(int)Probe::Count] = {};
    int64_t drawCalls = 0, tris = 0, textTris = 0;
    int64_t tickings = 0;
    int focusChanges = 0;
    // The sampled frames' drawing by view type: own µs and count.
    int sampledFrames = 0;
    std::map<std::string, std::pair<int64_t, int>> viewDraws;
    // Frames in each whole second, for the worst second's rate.
    int64_t secondStartUs = 0;
    int secondFrames = 0;
    int worstSecond = -1;
#ifdef __vita__
    uint64_t runClocksStart = 0;
#endif
};

Frame g_frame;        // the frame being drawn: the probes add to it
Window g_window;
int64_t g_lastIterationStartUs = 0;
int64_t g_lastLogUs = 0;
int64_t g_lastSlowUs = 0;
std::atomic<int> g_httpBegun{0};
std::atomic<int> g_httpInFlight{0};
int64_t g_lastViewSampleUs = 0;
bool g_sampling = false;  // this frame's drawing is being timed by type
int64_t g_layoutStartUs = 0, g_measureStartUs = 0;
int g_layoutDepth = 0;

int64_t nowUs() { return (int64_t)brls::getCPUTimeUsec(); }

double ms(int64_t us) { return (double)us / 1000.0; }

#ifdef __vita__
// The main thread's run time so far, in µs: against wall time, whether it
// was busy or waiting (GPU, vsync, other threads on its core).
uint64_t mainRunClocks() {
    SceKernelThreadInfo info = {};
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0) return 0;
    return info.runClocks;
}
#endif

std::string partsLine(const int64_t* part) {
    std::string out;
    for (int i = 0; i < kParts; i++) {
        if (part[i] < 500 && i != 4 && i != 6) continue;  // views and swap always
        if (!out.empty()) out += ", ";
        out += fmt::format("{} {:.1f}", kPartNames[i], ms(part[i]));
    }
    return out;
}

std::string probesLine(const int64_t* us, const int* n, double per) {
    std::string out;
    static const char* const names[(int)Probe::Count] = {"home draw", "cells", "textures",
                                                         "layout passes", "of them measuring"};
    for (int i = 0; i < (int)Probe::Count; i++) {
        if (!n[i]) continue;
        if (!out.empty()) out += ", ";
        out += fmt::format("{} {:.1f} ms", names[i], ms(us[i]) / per);
        if (i != (int)Probe::HomeDraw) out += fmt::format(" ({:.1f})", n[i] / per);
    }
    return out.empty() ? "none" : out;
}

// A type's name as written, from its mangled name (a lambda's names the
// function it was written in).
std::string demangle(const char* name) {
    int status = 0;
    char* out = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    std::string s = (status == 0 && out) ? out : name;
    std::free(out);
    // A lambda's type ends in its parameters' "{lambda(...)#n}"; the
    // function it sits in is what tells it apart.
    if (s.size() > 160) s = s.substr(0, 160) + "...";
    return s;
}

// What borealis noted this frame: brls::sync tasks that took long, and the
// views it deleted by type (patches/brls_thread.cpp, brls_application.cpp).
void logSlowTasksAndDeletes(int64_t deletionUs) {
    for (const auto& t : brls::Application::slowSyncTasks)
        brls::Logger::info("[perf] slow sync task {:.1f} ms: {}", ms((int64_t)t.second),
                           demangle(t.first.c_str()));
    brls::Application::slowSyncTasks.clear();

    auto& samples = brls::Application::deleteSamples;
    if (deletionUs >= 30000 && !samples.empty()) {
        std::vector<std::pair<const char*, std::pair<brls::Time, int>>> sorted(samples.begin(),
                                                                              samples.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
        std::string out;
        for (size_t i = 0; i < sorted.size() && i < 8; i++)
            out += fmt::format("{} {} x{} {:.1f} ms", i ? "," : "", demangle(sorted[i].first),
                               sorted[i].second.second, ms((int64_t)sorted[i].second.first));
        brls::Logger::info("[perf] deleting views took {:.1f} ms, own time by type:{}",
                           ms(deletionUs), out);
    }
    samples.clear();
}

// The running tickings by type, most first: an animation or task that never
// ends shows here.
std::string tickingsLine() {
    std::map<std::string, int> byType;
    for (brls::Ticking* t : brls::Ticking::runningTickings) {
        if (!t) continue;
        byType[demangle(typeid(*t).name())]++;
    }
    std::vector<std::pair<std::string, int>> sorted(byType.begin(), byType.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string out;
    for (size_t i = 0; i < sorted.size() && i < 6; i++) {
        if (!out.empty()) out += ", ";
        out += fmt::format("{} x{}", sorted[i].first, sorted[i].second);
    }
    return out.empty() ? "none" : out;
}

// The sampled frames' drawing by view type, most own time first, per frame.
std::string viewDrawsLine(const Window& w) {
    if (!w.sampledFrames) return "not sampled";
    std::vector<std::pair<std::string, std::pair<int64_t, int>>> sorted(w.viewDraws.begin(),
                                                                         w.viewDraws.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    const double per = (double)w.sampledFrames;
    int64_t all = 0;
    for (const auto& e : sorted) all += e.second.first;
    std::string out = fmt::format("{:.1f} ms in {} frame(s):", ms(all) / per, w.sampledFrames);
    for (size_t i = 0; i < sorted.size() && i < 10; i++)
        out += fmt::format("{} {} x{:.0f} {:.2f} ms", i ? "," : "", sorted[i].first,
                           sorted[i].second.second / per, ms(sorted[i].second.first) / per);
    return out;
}

// What is on screen: the top activity's content and the focused view.
std::string screenLine() {
    std::string out;
    auto stack = brls::Application::getActivitiesStack();
    if (!stack.empty() && stack.back()) {
        brls::View* content = stack.back()->getContentView();
        out = fmt::format("{} activities, top {}", stack.size(),
                          content ? content->describe() : std::string("(none)"));
    }
    if (brls::View* focus = brls::Application::getCurrentFocus())
        out += ", focus " + focus->describe();
    return out;
}

void report(int64_t now, bool quiet) {
    Window& w = g_window;
    const int64_t span = now - w.startUs;
    if (w.frames == 0 || span <= 0) return;
    const double per = (double)w.frames;
    const double fps = w.frames * 1e6 / (double)span;

    std::string cpu = "n/a";
#ifdef __vita__
    const uint64_t clocks = mainRunClocks();
    if (clocks && w.runClocksStart)
        cpu = fmt::format("{:.0f}%", 100.0 * (double)(clocks - w.runClocksStart) / (double)span);
#endif

    brls::Logger::info(
        "[perf] {:.1f} s{}: {:.0f} fps (worst second {}), frame avg {:.1f} ms, {} over 33 ms, "
        "{} over 50 ms, worst {:.1f} ms; main thread busy {}",
        span / 1e6, quiet ? " (heartbeat)" : "", fps, w.worstSecond < 0 ? w.frames : w.worstSecond,
        ms(w.totalSum) / per, w.over33, w.over50, ms(w.worst.total), cpu);
    if (quiet) return;

    int64_t avg[kParts];
    for (int i = 0; i < kParts; i++) avg[i] = (int64_t)(w.partSum[i] / per);
    // An average below the line's 0.5 ms cut still matters when it is the
    // focus: logged as is.
    brls::Logger::info("[perf]   avg ms: {}", partsLine(avg));
    brls::Logger::info("[perf]   max ms: {}", partsLine(w.partMax));
    brls::Logger::info("[perf]   worst frame {:.1f} ms: {}", ms(w.worst.total),
                       partsLine(w.worst.part));
    brls::Logger::info(
        "[perf]   per frame: {:.0f} draw calls, {:.0f} triangles ({:.0f} text), {:.1f} tickings; "
        "{}; http {} begun, {} in flight",
        w.drawCalls / per, w.tris / per, w.textTris / per, w.tickings / per,
        probesLine(w.probeUs, w.probeN, per), g_httpBegun.exchange(0), g_httpInFlight.load());
    brls::Logger::info("[perf]   focus changes: {}", w.focusChanges);
    brls::Logger::info("[perf]   drawing by view type (own time): {}", viewDrawsLine(w));
    brls::Logger::info("[perf]   tickings: {}", tickingsLine());
    brls::Logger::info("[perf]   screen: {}", screenLine());
}

void startWindow(int64_t now) {
    g_window = Window();
    g_window.startUs = now;
    g_window.secondStartUs = now;
#ifdef __vita__
    g_window.runClocksStart = mainRunClocks();
#endif
}

void onFrame(const Timings& t) {
    const int64_t start = (int64_t)t.start;
    const int64_t now = nowUs();
    if (!g_window.startUs) {
        startWindow(start);
        g_lastLogUs = start;
    }

    Frame& f = g_frame;
    f.part[0] = t.poll;
    f.part[1] = t.input;
    f.part[2] = t.anim;
    f.part[3] = t.begin;
    f.part[4] = t.views;
    f.part[5] = t.nvgEnd;
    f.part[6] = t.end;
    f.part[7] = t.sync;
    f.part[8] = t.runLoop;
    f.part[9] = t.deletion;
    f.part[11] = t.focusLost;
    f.part[12] = t.focusEvent;
    f.part[13] = t.focusGained;
    f.part[14] = t.focusHints;
    f.part[15] = t.layout;
    // The time between the last iteration's end and this one's start:
    // frame-limit sleeps, the platform's own loop.
    f.part[10] = g_lastIterationStartUs ? std::max<int64_t>(0, start - g_lastIterationStartUs) -
                                              std::max<int64_t>(0, g_frame.total)
                                        : 0;
    if (f.part[10] < 0) f.part[10] = 0;
    f.total = (int64_t)t.total + f.part[10];
    f.tickings = t.tickings;
    if (t.drawn) {
        if (NVGcontext* vg = brls::Application::getNVGContext()) {
            int fill = 0, stroke = 0, text = 0;
            nvgFrameCounts(vg, &f.drawCalls, &fill, &stroke, &text);
            f.tris = fill + stroke + text;
            f.textTris = text;
        }
    }

    logSlowTasksAndDeletes(t.deletion);

    Window& w = g_window;
    w.frames++;
    w.totalSum += f.total;
    for (int i = 0; i < kParts; i++) {
        w.partSum[i] += f.part[i];
        w.partMax[i] = std::max(w.partMax[i], f.part[i]);
    }
    for (int i = 0; i < (int)Probe::Count; i++) {
        w.probeUs[i] += f.probeUs[i];
        w.probeN[i] += f.probeN[i];
    }
    w.drawCalls += f.drawCalls;
    w.tris += f.tris;
    w.textTris += f.textTris;
    w.tickings += (int64_t)f.tickings;
    w.focusChanges += t.focusChanges;
    if (g_sampling) {
        g_sampling = false;
        brls::Application::sampleViewDraws = false;
        w.sampledFrames++;
        for (const auto& e : brls::Application::viewDrawSamples) {
            auto& into = w.viewDraws[demangle(e.first)];
            into.first += (int64_t)e.second.first;
            into.second += e.second.second;
        }
        brls::Application::viewDrawSamples.clear();
    }
    if (f.total > 33333) w.over33++;
    if (f.total > kSlowFrameUs) w.over50++;
    if (f.total > w.worst.total) w.worst = f;
    w.secondFrames++;
    if (start - w.secondStartUs >= 1000000) {
        if (w.worstSecond < 0 || w.secondFrames < w.worstSecond) w.worstSecond = w.secondFrames;
        w.secondStartUs = start;
        w.secondFrames = 0;
    }

    if (f.total >= kSlowFrameUs && start - g_lastSlowUs >= kSlowGapUs) {
        g_lastSlowUs = start;
        brls::Logger::info(
            "[perf] slow frame {:.1f} ms: {}; {} draw calls; {}; {} tickings", ms(f.total),
            partsLine(f.part), f.drawCalls, probesLine(f.probeUs, f.probeN, 1.0), f.tickings);
    }

    if (now - w.startUs >= kWindowUs) {
        const double fps = w.frames * 1e6 / (double)(now - w.startUs);
        const bool bad = fps < kLowFps || w.over50 > 0;
        if (bad || now - g_lastLogUs >= kHeartbeatUs) {
            report(now, !bad);
            g_lastLogUs = now;
        }
        startWindow(now);
    }

    // Time the next frame's drawing by view type, once a second.
    if (now - g_lastViewSampleUs >= kViewSampleEveryUs) {
        g_lastViewSampleUs = now;
        g_sampling = true;
        brls::Application::sampleViewDraws = true;
    }

    g_lastIterationStartUs = start;
    const int64_t total = (int64_t)t.total;
    g_frame = Frame();
    g_frame.total = total;  // this iteration's own length, for the next gap
}

// Yoga's events: each layout pass and measure callback, into the frame.
// Layout runs on the UI thread (borealis's invalidate()).
void onYogaEvent(const YGNode&, facebook::yoga::Event::Type type,
                 facebook::yoga::Event::Data) {
    using E = facebook::yoga::Event;
    switch (type) {
        case E::LayoutPassStart:
            if (g_layoutDepth++ == 0) g_layoutStartUs = nowUs();
            break;
        case E::LayoutPassEnd:
            if (g_layoutDepth > 0 && --g_layoutDepth == 0)
                addUi(Probe::Layout, nowUs() - g_layoutStartUs);
            break;
        case E::MeasureCallbackStart:
            g_measureStartUs = nowUs();
            break;
        case E::MeasureCallbackEnd:
            addUi(Probe::Measure, nowUs() - g_measureStartUs);
            break;
        default:
            break;
    }
}

}  // namespace

void install() {
    facebook::yoga::Event::subscribe(onYogaEvent);
    brls::Application::frameTimingsCallback = [](const Timings& t) { onFrame(t); };
    brls::Logger::info("[perf] frame profiler on: a summary every {} s when below {} fps or a "
                       "frame passes {} ms, else one a minute",
                       kWindowUs / 1000000, kLowFps, kSlowFrameUs / 1000);
}

void addUi(Probe probe, int64_t us) {
    const int i = (int)probe;
    g_frame.probeUs[i] += us;
    g_frame.probeN[i]++;
}

void httpBegun() {
    g_httpBegun.fetch_add(1);
    g_httpInFlight.fetch_add(1);
}

void httpEnded() { g_httpInFlight.fetch_sub(1); }

Scope::Scope(Probe probe) : m_probe(probe), m_start(nowUs()) {}

Scope::~Scope() { addUi(m_probe, nowUs() - m_start); }

}  // namespace perf
}  // namespace vitaplex
