/**
 * VitaPlex - downloads carried on by iOS. See include/app/downloads_ios.hpp.
 */

#include "app/downloads_ios.hpp"
#include "platform/paths.hpp"

#include <borealis.hpp>
#include <SDL2/SDL_events.h>

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <objc/runtime.h>

#include <unistd.h>  // truncate

#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>

using namespace vitaplex;

namespace {

std::function<void()> g_onBackground, g_onForeground, g_onFinished;
std::mutex g_statusMutex;
void (^g_backgroundEventsDone)(void) = nil;

std::string statusPath() { return platformPath("ios_downloads.txt"); }

// "key\tok\tsize" per line.
void recordResult(const std::string& key, bool ok, int64_t size) {
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        std::ofstream f(statusPath(), std::ios::app);
        f << key << '\t' << (ok ? 1 : 0) << '\t' << size << '\n';
    }
    brls::Logger::info("Downloads: iOS {} {} ({} bytes)", ok ? "finished" : "failed", key, size);
    if (g_onFinished) brls::sync([]() { if (g_onFinished) g_onFinished(); });
}

// key \t path \t offset, kept on the task so it survives the app closing.
std::string describe(const iosdl::Task& t) {
    return t.key + "\t" + t.path + "\t" + std::to_string(t.offset);
}

bool parse(NSString* desc, std::string& key, std::string& path, int64_t& offset) {
    if (!desc) return false;
    std::stringstream ss(desc.UTF8String);
    std::string off;
    if (!std::getline(ss, key, '\t') || !std::getline(ss, path, '\t') || !std::getline(ss, off))
        return false;
    offset = std::atoll(off.c_str());
    return !key.empty() && !path.empty();
}

int64_t fileSize(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return -1;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fclose(f);
    return n;
}

}  // namespace

@interface VPDownloadDelegate : NSObject <NSURLSessionDownloadDelegate>
@end

@implementation VPDownloadDelegate

// The downloaded part is in a temporary file that iOS deletes when this
// returns: put it in the item's file now.
- (void)URLSession:(NSURLSession*)session
                 downloadTask:(NSURLSessionDownloadTask*)task
    didFinishDownloadingToURL:(NSURL*)location {
    std::string key, path;
    int64_t offset = 0;
    if (!parse(task.taskDescription, key, path, offset)) return;
    const NSInteger status = [task.response isKindOfClass:[NSHTTPURLResponse class]]
                                 ? ((NSHTTPURLResponse*)task.response).statusCode
                                 : 0;
    const std::string tmp = location.path.UTF8String;

    bool ok = false;
    if (status == 206 && offset > 0) {
        // The rest of the file: onto the bytes it continues from. Cut to that
        // point first, in case VitaPlex's own download wrote a little past it
        // as it stopped.
        if (truncate(path.c_str(), (off_t)offset) == 0) {
            FILE* in = std::fopen(tmp.c_str(), "rb");
            FILE* out = std::fopen(path.c_str(), "ab");
            if (in && out) {
                char buf[1 << 16];
                size_t n;
                ok = true;
                while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0)
                    if (std::fwrite(buf, 1, n, out) != n) { ok = false; break; }
            }
            if (in) std::fclose(in);
            if (out) std::fclose(out);
        }
    } else if (status == 200) {
        // The whole file (the server ignored the range, or there was none).
        std::remove(path.c_str());
        ok = std::rename(tmp.c_str(), path.c_str()) == 0;
    } else if (status == 416) {
        ok = true;  // asked from its end: it was whole already
    }
    if (!ok)
        brls::Logger::warning("Downloads: iOS download of {} answered {}", key, (long)status);
    recordResult(key, ok, ok ? fileSize(path) : 0);
}

- (void)URLSession:(NSURLSession*)session
                    task:(NSURLSessionTask*)task
    didCompleteWithError:(NSError*)error {
    if (!error) return;  // success was handled above
    if (error.code == NSURLErrorCancelled) return;
    std::string key, path;
    int64_t offset = 0;
    if (!parse(task.taskDescription, key, path, offset)) return;
    brls::Logger::warning("Downloads: iOS download of {} failed: {}", key,
                          error.localizedDescription.UTF8String);
    recordResult(key, false, 0);
}

- (void)URLSessionDidFinishEventsForBackgroundURLSession:(NSURLSession*)session {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (g_backgroundEventsDone) {
            g_backgroundEventsDone();
            g_backgroundEventsDone = nil;
        }
    });
}

@end

namespace {

NSURLSession* g_session = nil;
VPDownloadDelegate* g_delegate = nil;

NSURLSession* session() {
    if (g_session) return g_session;
    NSString* ident =
        [[[NSBundle mainBundle] bundleIdentifier] stringByAppendingString:@".downloads"];
    NSURLSessionConfiguration* cfg =
        [NSURLSessionConfiguration backgroundSessionConfigurationWithIdentifier:ident];
    cfg.sessionSendsLaunchEvents = YES;
    cfg.discretionary = NO;
    g_delegate = [[VPDownloadDelegate alloc] init];
    g_session = [NSURLSession sessionWithConfiguration:cfg delegate:g_delegate delegateQueue:nil];
    return g_session;
}

// iOS wakes the app for a background session's events through
// -application:handleEventsForBackgroundURLSession:completionHandler:, which
// SDL's app delegate does not have: add it to the delegate's class.
void handleEventsImp(id self, SEL _cmd, UIApplication* app, NSString* identifier,
                     void (^completion)(void)) {
    g_backgroundEventsDone = [completion copy];
    session();  // reattaches the delegate, which then receives the events
}

void addBackgroundEventsHandler() {
    id delegate = [UIApplication sharedApplication].delegate;
    if (!delegate) return;
    SEL sel = @selector(application:handleEventsForBackgroundURLSession:completionHandler:);
    if ([delegate respondsToSelector:sel]) return;
    class_addMethod([delegate class], sel, (IMP)handleEventsImp, "v@:@@@?");
}

}  // namespace

namespace vitaplex {
namespace iosdl {

void init(std::function<void()> onBackground, std::function<void()> onForeground,
          std::function<void()> onFinished) {
    g_onBackground = std::move(onBackground);
    g_onForeground = std::move(onForeground);
    g_onFinished = std::move(onFinished);
    @autoreleasepool {
        addBackgroundEventsHandler();
        session();
    }
    SDL_AddEventWatch(
        [](void*, SDL_Event* e) -> int {
            if (e->type == SDL_APP_DIDENTERBACKGROUND && g_onBackground) g_onBackground();
            else if (e->type == SDL_APP_DIDENTERFOREGROUND && g_onForeground) g_onForeground();
            return 0;
        },
        nullptr);
}

int start(const std::vector<Task>& tasks) {
    int started = 0;
    @autoreleasepool {
        for (const auto& t : tasks) {
            NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:t.url.c_str()]];
            if (!url) continue;
            NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:url];
            if (t.offset > 0)
                [req setValue:[NSString stringWithFormat:@"bytes=%lld-", (long long)t.offset]
                    forHTTPHeaderField:@"Range"];
            NSURLSessionDownloadTask* task = [session() downloadTaskWithRequest:req];
            task.taskDescription = [NSString stringWithUTF8String:describe(t).c_str()];
            [task resume];
            started++;
        }
    }
    return started;
}

std::vector<Result> takeFinished() {
    std::vector<Result> out;
    std::lock_guard<std::mutex> lock(g_statusMutex);
    std::ifstream f(statusPath());
    std::string line;
    while (std::getline(f, line)) {
        std::stringstream ss(line);
        Result r;
        std::string ok, size;
        if (!std::getline(ss, r.key, '\t') || !std::getline(ss, ok, '\t') || !std::getline(ss, size))
            continue;
        r.ok = ok == "1";
        r.size = std::atoll(size.c_str());
        out.push_back(r);
    }
    f.close();
    std::remove(statusPath().c_str());
    return out;
}

void runningKeys(std::function<void(std::vector<std::string>)> cb) {
    [session() getAllTasksWithCompletionHandler:^(NSArray<__kindof NSURLSessionTask*>* tasks) {
        std::vector<std::string> keys;
        for (NSURLSessionTask* t in tasks) {
            if (t.state != NSURLSessionTaskStateRunning && t.state != NSURLSessionTaskStateSuspended)
                continue;
            std::string key, path;
            int64_t offset = 0;
            if (parse(t.taskDescription, key, path, offset)) keys.push_back(key);
        }
        brls::sync([cb, keys]() { cb(keys); });
    }];
}

}  // namespace iosdl
}  // namespace vitaplex
