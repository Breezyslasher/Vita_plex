#pragma once

/**
 * VitaPlex - what to say when the app hangs
 *
 * Read without locks by the Vita's watcher thread (background_audio_psv.cpp)
 * when the UI stops: when the UI thread last ran a frame, what it is busy in,
 * and which background loads are running and for how long. Times are
 * brls::getCPUTimeUsec().
 */

#include <borealis.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace vitaplex {
namespace hang {

// Each pass of the main loop.
inline std::atomic<int64_t> g_uiBeatUs{0};
inline void uiBeat() { g_uiBeatUs.store((int64_t)brls::getCPUTimeUsec()); }

// What the UI thread is in (outermost note only).
inline std::atomic<const char*> g_uiWhat{nullptr};
inline std::atomic<int64_t> g_uiSinceUs{0};

struct UiNote {
    bool set = false;
    int64_t start = 0;
    const char* what;
    explicit UiNote(const char* w) : what(w) {
        start = (int64_t)brls::getCPUTimeUsec();
        const char* none = nullptr;
        if (g_uiWhat.compare_exchange_strong(none, w)) {
            set = true;
            g_uiSinceUs.store(start);
        }
    }
    ~UiNote() {
        const int64_t ms = ((int64_t)brls::getCPUTimeUsec() - start) / 1000;
        if (ms >= 200) brls::Logger::warning("[hang] UI thread spent {} ms in {}", ms, what);
        if (set) g_uiWhat.store(nullptr);
    }
};

// Running background loads, in fixed slots so the watcher can read them.
constexpr int kTaskSlots = 16;
struct TaskSlot {
    std::atomic<const char*> what{nullptr};
    std::atomic<int64_t> sinceUs{0};
    std::atomic<int> seq{0};
};
inline TaskSlot g_tasks[kTaskSlots];
inline std::atomic<int> g_taskSeq{0};
inline std::atomic<int> g_tasksRunning{0};

// Logged as it starts and ends; with `alive` given, the end says whether the
// page that asked for it is still open (its result is dropped when not).
struct TaskNote {
    const char* what;
    int seq;
    int slot = -1;
    int64_t start;
    std::weak_ptr<bool> alive;
    bool hasAlive;

    explicit TaskNote(const char* w) : TaskNote(w, std::weak_ptr<bool>(), false) {}
    TaskNote(const char* w, std::weak_ptr<bool> a) : TaskNote(w, std::move(a), true) {}
    TaskNote(const char* w, std::weak_ptr<bool> a, bool has)
        : what(w), seq(++g_taskSeq), start((int64_t)brls::getCPUTimeUsec()),
          alive(std::move(a)), hasAlive(has) {
        const int running = ++g_tasksRunning;
        for (int i = 0; i < kTaskSlots; ++i) {
            const char* none = nullptr;
            if (g_tasks[i].what.compare_exchange_strong(none, w)) {
                g_tasks[i].sinceUs.store(start);
                g_tasks[i].seq.store(seq);
                slot = i;
                break;
            }
        }
        brls::Logger::info("[load #{}] {} started ({} running)", seq, what, running);
    }
    ~TaskNote() {
        const int running = --g_tasksRunning;
        if (slot >= 0) g_tasks[slot].what.store(nullptr);
        const int64_t ms = ((int64_t)brls::getCPUTimeUsec() - start) / 1000;
        const char* page = "";
        if (hasAlive) {
            auto a = alive.lock();
            page = (a && *a) ? ", page open" : ", page gone: result dropped";
        }
        brls::Logger::info("[load #{}] {} done in {} ms{} ({} still running)", seq, what, ms,
                           page, running);
    }
};

// "UI thread last ran 12.3s ago, in X for 12.0s; 3 loads running: #4 a (10.2s), ..."
inline std::string report(int64_t nowUs) {
    char buf[256];
    std::string out;
    const int64_t beat = g_uiBeatUs.load();
    std::snprintf(buf, sizeof(buf), "UI thread last ran a frame %.1fs ago",
                  beat ? (nowUs - beat) / 1e6 : -1.0);
    out = buf;
    if (const char* w = g_uiWhat.load()) {
        std::snprintf(buf, sizeof(buf), ", in %s for %.1fs", w, (nowUs - g_uiSinceUs.load()) / 1e6);
        out += buf;
    }
    std::snprintf(buf, sizeof(buf), "; %d loads running", g_tasksRunning.load());
    out += buf;
    bool first = true;
    for (int i = 0; i < kTaskSlots; ++i) {
        const char* w = g_tasks[i].what.load();
        if (!w) continue;
        std::snprintf(buf, sizeof(buf), "%s#%d %s (%.1fs)", first ? ": " : ", ",
                      g_tasks[i].seq.load(), w, (nowUs - g_tasks[i].sinceUs.load()) / 1e6);
        out += buf;
        first = false;
    }
    return out;
}

}  // namespace hang
}  // namespace vitaplex
