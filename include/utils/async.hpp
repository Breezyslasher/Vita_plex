/**
 * VitaPlex - Async utilities
 * Simple async task execution with UI thread callbacks
 */

#pragma once

#include <functional>
#include <thread>
#include <borealis.hpp>
#ifdef __vita__
#include <pthread.h>
#endif

namespace vitaplex {

/**
 * Execute a task asynchronously and call a callback on the UI thread when done.
 *
 * @param task The task to run in background (should not touch UI)
 * @param callback Called on UI thread when task completes
 */
template<typename T>
inline void asyncTask(std::function<T()> task, std::function<void(T)> callback) {
    std::thread([task, callback]() {
        T result = task();
        brls::sync([callback, result]() {
            callback(result);
        });
    }).detach();
}

/**
 * Execute a void task asynchronously and call a callback on the UI thread when done.
 *
 * @param task The task to run in background
 * @param callback Called on UI thread when task completes
 */
inline void asyncTask(std::function<void()> task, std::function<void()> callback) {
    std::thread([task, callback]() {
        task();
        brls::sync([callback]() {
            callback();
        });
    }).detach();
}

/**
 * Execute a task asynchronously without a callback
 *
 * @param task The task to run in background
 */
inline void asyncRun(std::function<void()> task) {
    std::thread([task]() {
        task();
    }).detach();
}

/**
 * Execute a task asynchronously with a larger stack size.
 * Use for heavy work like downloads that have deep call stacks.
 *
 * @param task The task to run in background
 * @param stackSize Stack size in bytes (default 512KB)
 */
inline void asyncRunLargeStack(std::function<void()> task, size_t stackSize = 512 * 1024) {
#ifdef __vita__
    // On Vita, std::thread uses a small default stack (256KB) which can overflow
    // during heavy operations like HLS downloads. Use pthread directly with a
    // larger stack to prevent crashes.
    auto* taskCopy = new std::function<void()>(task);
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stackSize);
    pthread_create(&thread, &attr, [](void* arg) -> void* {
        auto* fn = static_cast<std::function<void()>*>(arg);
        (*fn)();
        delete fn;
        return nullptr;
    }, taskCopy);
    pthread_attr_destroy(&attr);
    pthread_detach(thread);
#else
    // On desktop, default stack size is usually large enough
    std::thread([task]() {
        task();
    }).detach();
#endif
}

} // namespace vitaplex
