#include "utils/log_file_writer.hpp"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace vitaplex {
namespace logfile {

namespace {

FILE* g_file = nullptr;
// g_fileMutex is held around writing to the file; the queue is taken under it
// too, so lines reach the file in the order they were logged.
std::mutex g_fileMutex;
std::mutex g_queueMutex;
std::condition_variable g_wake;
std::vector<std::string> g_queue;
std::thread g_writer;
bool g_stopping = false;

// Under g_fileMutex: the queued lines, and `extra` after them, written and
// flushed.
void drainLocked(const std::string* extra) {
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        lines.swap(g_queue);
    }
    if (!g_file) return;
    for (const std::string& line : lines) std::fputs(line.c_str(), g_file);
    if (extra) std::fputs(extra->c_str(), g_file);
    if (!lines.empty() || extra) std::fflush(g_file);
}

void writerLoop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(g_queueMutex);
            g_wake.wait(lock, [] { return g_stopping || !g_queue.empty(); });
            if (g_stopping && g_queue.empty()) return;
        }
        std::lock_guard<std::mutex> lock(g_fileMutex);
        drainLocked(nullptr);
    }
}

}  // namespace

void start(FILE* file) {
    if (!file || g_writer.joinable()) return;
    g_file = file;
    std::setvbuf(file, nullptr, _IOFBF, 16 * 1024);
    g_stopping = false;
    g_writer = std::thread(writerLoop);
}

void write(std::string line, bool now) {
    if (now || !g_writer.joinable()) {
        std::lock_guard<std::mutex> lock(g_fileMutex);
        drainLocked(&line);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_queue.push_back(std::move(line));
    }
    g_wake.notify_one();
}

void stop() {
    if (g_writer.joinable()) {
        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            g_stopping = true;
        }
        g_wake.notify_one();
        g_writer.join();
    }
    std::lock_guard<std::mutex> lock(g_fileMutex);
    drainLocked(nullptr);
    g_file = nullptr;
}

}  // namespace logfile
}  // namespace vitaplex
