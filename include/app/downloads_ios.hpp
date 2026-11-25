#pragma once

/**
 * VitaPlex - downloads carried on by iOS (iPhone / iPad)
 *
 * iOS freezes an app soon after it leaves the screen, and VitaPlex's own
 * downloads (curl, on a thread) stop with it. Only the system's background
 * URLSession goes on. As the app goes to the background, DownloadsManager
 * hands each unfinished download it can name a file URL for to this module
 * (start), which asks iOS for the rest of the file from the bytes already on
 * disk (HTTP Range). iOS downloads it, also with the app closed, and this
 * module puts the result into the item's file: the part appended on a 206,
 * the whole file on a 200. What came of each is written to a status file,
 * read back with takeFinished(), and announced through the result callback
 * when the app is running.
 *
 * Built for iPhone / iPad only (VITAPLEX_IOS_DOWNLOADS); see downloads_ios.mm.
 */

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vitaplex {
namespace iosdl {

struct Task {
    std::string key;      // the item's ratingKey
    std::string url;      // the file, token included
    std::string path;     // the item's local file
    int64_t offset = 0;   // bytes already in it: the request asks for the rest
};

struct Result {
    std::string key;
    bool ok = false;
    int64_t size = 0;     // the file's size once whole
};

// Hooks: onBackground runs as the app goes to the background (on the main
// thread, with a few seconds to finish), onForeground as it comes back,
// onFinished whenever iOS reports a download done or failed while the app
// runs (on the main loop). Sets up the background session, which also picks
// up downloads iOS carried on while the app was closed.
void init(std::function<void()> onBackground, std::function<void()> onForeground,
          std::function<void()> onFinished);

// Hand these over. Returns how many iOS took.
int start(const std::vector<Task>& tasks);

// What iOS finished since the last call (from the status file), cleared.
std::vector<Result> takeFinished();

// The keys iOS is still downloading. Asynchronous; cb runs on the main loop.
void runningKeys(std::function<void(std::vector<std::string>)> cb);

}  // namespace iosdl
}  // namespace vitaplex
