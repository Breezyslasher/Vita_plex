/**
 * VitaPlex - Async utilities
 * Simple async task execution with UI thread callbacks.
 *
 * Every helper here routes through platform::launchThread() rather than
 * std::thread().detach() directly. The Switch's newlib std::thread shim
 * doesn't always register the thread's stack region with the kernel or
 * initialize TLS — a detached thread launched that way crashes with an
 * Atmosphère Instruction Abort (PC at a page boundary, TLS dump zeroed)
 * the first time it indirect-calls through a std::function or vtable.
 * platform::launchThread() routes through pthread_create with explicit
 * attrs on Switch/PSV/PS4 (kernel-managed stack + TLS) and stays on
 * std::thread on Desktop/Android/iOS where bare detach is fine.
 */

#pragma once

#include <functional>
#include <borealis.hpp>
#include "platform/platform.hpp"

namespace vitaplex {

/**
 * Execute a task asynchronously and call a callback on the UI thread when done.
 *
 * @param task The task to run in background (should not touch UI)
 * @param callback Called on UI thread when task completes
 */
template<typename T>
inline void asyncTask(std::function<T()> task, std::function<void(T)> callback) {
    platform::launchThread([task, callback]() {
        T result = task();
        brls::sync([callback, result]() {
            callback(result);
        });
    });
}

/**
 * Execute a void task asynchronously and call a callback on the UI thread when done.
 *
 * @param task The task to run in background
 * @param callback Called on UI thread when task completes
 */
inline void asyncTask(std::function<void()> task, std::function<void()> callback) {
    platform::launchThread([task, callback]() {
        task();
        brls::sync([callback]() {
            callback();
        });
    });
}

/**
 * Execute a task asynchronously without a callback.
 *
 * @param task The task to run in background
 */
inline void asyncRun(std::function<void()> task) {
    platform::launchThread(std::move(task));
}

/**
 * Execute a task asynchronously with a larger stack size.
 * Use for heavy work like downloads that have deep call stacks.
 *
 * @param task The task to run in background
 * @param stackSize Stack size in bytes (default 512KB). Honored on
 *                  Switch/PSV/PS4; ignored on Desktop/Android/iOS where
 *                  std::thread already gets a generous default.
 */
inline void asyncRunLargeStack(std::function<void()> task,
                               std::size_t stackSize = 512 * 1024) {
    platform::launchThread(std::move(task), stackSize);
}

} // namespace vitaplex
