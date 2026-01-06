/**
 * VitaPlex - Async utilities
 * Simple async task execution with UI thread callbacks
 */

#pragma once

#include <functional>
#include <thread>
#include <borealis.hpp>

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

} // namespace vitaplex
