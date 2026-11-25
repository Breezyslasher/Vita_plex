/**
 * VitaPlex - Picture in Picture on iPhone / iPad
 *
 * iOS shows another app's video in a PiP window only through its own video
 * layers, so this keeps an AVSampleBufferDisplayLayer behind the app's view
 * and, while a video plays, gives it a copy of every frame mpv draws
 * (MpvPlayer's software render path calls pushFrameRGBA). An
 * AVPictureInPictureController (iOS 15's sample-buffer content source) takes
 * that layer into the PiP window: from the player's PiP button, or by itself
 * when the app goes to the background while the video plays.
 *
 * The PiP window's play/pause and skip buttons reach mpv through the playback
 * delegate; a control timebase kept at mpv's position gives it its progress.
 * Frames go on being drawn in the background (mpv's software renderer needs no
 * GPU), and MpvPlayer skips the on-screen upload there.
 *
 * Wired from src/utils/pip.cpp (pip::isAvailable / enter / leave / ...).
 */

#include "player/mpv_player.hpp"

#include <borealis.hpp>

#import <AVFoundation/AVFoundation.h>
#import <AVKit/AVKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <UIKit/UIKit.h>

#include <cstdint>
#include <cstring>

using vitaplex::MpvPlayer;

API_AVAILABLE(ios(15.0))
@interface VPPipController : NSObject <AVPictureInPictureControllerDelegate,
                                       AVPictureInPictureSampleBufferPlaybackDelegate>
@property(nonatomic, strong) UIView* hostView;
@property(nonatomic, strong) AVSampleBufferDisplayLayer* layer;
@property(nonatomic, strong) AVPictureInPictureController* controller;
@property(nonatomic, assign) CMTimebaseRef timebase;
@property(nonatomic, assign) CVPixelBufferPoolRef pool;
@property(nonatomic, assign) int poolW;
@property(nonatomic, assign) int poolH;
@property(nonatomic, assign) CMVideoFormatDescriptionRef format;
@property(nonatomic, assign) CFTimeInterval lastSync;
@end

@implementation VPPipController

- (instancetype)init {
    if ((self = [super init])) {
        _layer = [[AVSampleBufferDisplayLayer alloc] init];
        _layer.videoGravity = AVLayerVideoGravityResizeAspect;

        CMTimebaseRef tb = NULL;
        CMTimebaseCreateWithSourceClock(kCFAllocatorDefault, CMClockGetHostTimeClock(), &tb);
        _timebase = tb;
        if (tb) {
            CMTimebaseSetTime(tb, kCMTimeZero);
            CMTimebaseSetRate(tb, 0.0);
            _layer.controlTimebase = tb;
        }

        AVPictureInPictureControllerContentSource* source =
            [[AVPictureInPictureControllerContentSource alloc]
                initWithSampleBufferDisplayLayer:_layer
                                playbackDelegate:self];
        _controller = [[AVPictureInPictureController alloc] initWithContentSource:source];
        _controller.delegate = self;
        _controller.canStartPictureInPictureAutomaticallyFromInline = YES;
    }
    return self;
}

- (void)dealloc {
    if (_timebase) CFRelease(_timebase);
    if (_pool) CVPixelBufferPoolRelease(_pool);
    if (_format) CFRelease(_format);
}

// The layer has to be in a window to go into PiP: behind the app's own view,
// covered by it, the size of the window.
- (void)attachToWindow {
    UIWindow* window = nil;
    for (UIWindow* w in [UIApplication sharedApplication].windows) {
        if (w.isKeyWindow) { window = w; break; }
    }
    if (!window) window = [UIApplication sharedApplication].windows.firstObject;
    if (!window) return;
    if (!self.hostView) {
        self.hostView = [[UIView alloc] initWithFrame:window.bounds];
        self.hostView.userInteractionEnabled = NO;
        self.hostView.autoresizingMask =
            UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        self.layer.frame = self.hostView.bounds;
        [self.hostView.layer addSublayer:self.layer];
    }
    if (self.hostView.superview != window) [window insertSubview:self.hostView atIndex:0];
}

// Keep the PiP window's clock at mpv's position and rate.
- (void)syncTimebase {
    if (!self.timebase) return;
    MpvPlayer& p = MpvPlayer::getInstance();
    CMTimebaseSetTime(self.timebase, CMTimeMakeWithSeconds(p.getPosition(), 600));
    CMTimebaseSetRate(self.timebase, p.isPlaying() ? 1.0 : 0.0);
    self.lastSync = CACurrentMediaTime();
}

- (void)pushRGBA:(const uint8_t*)src width:(int)w height:(int)h stride:(int)stride {
    // Half size above 1280 wide: a PiP window is small, and the copy is then a
    // quarter of the work.
    const int step = w > 1280 ? 2 : 1;
    const int ow = w / step, oh = h / step;
    if (ow <= 0 || oh <= 0) return;

    if (!self.pool || self.poolW != ow || self.poolH != oh) {
        if (self.pool) CVPixelBufferPoolRelease(self.pool);
        if (self.format) {
            CFRelease(self.format);
            self.format = NULL;
        }
        NSDictionary* attrs = @{
            (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
            (id)kCVPixelBufferWidthKey : @(ow),
            (id)kCVPixelBufferHeightKey : @(oh),
            (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
        };
        CVPixelBufferPoolRef pool = NULL;
        CVPixelBufferPoolCreate(kCFAllocatorDefault, NULL, (__bridge CFDictionaryRef)attrs, &pool);
        self.pool = pool;
        self.poolW = ow;
        self.poolH = oh;
        if (!pool) return;
    }

    CVPixelBufferRef pb = NULL;
    if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, self.pool, &pb) != kCVReturnSuccess || !pb)
        return;
    CVPixelBufferLockBaseAddress(pb, 0);
    uint8_t* dst = (uint8_t*)CVPixelBufferGetBaseAddress(pb);
    const size_t dstStride = CVPixelBufferGetBytesPerRow(pb);
    for (int y = 0; y < oh; y++) {
        const uint8_t* s = src + (size_t)(y * step) * stride;
        uint8_t* d = dst + (size_t)y * dstStride;
        for (int x = 0; x < ow; x++, s += 4 * step, d += 4) {
            d[0] = s[2];  // RGBA -> BGRA
            d[1] = s[1];
            d[2] = s[0];
            d[3] = 255;
        }
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);

    if (!self.format)
        CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, pb, &_format);

    CMSampleTimingInfo timing = {kCMTimeInvalid, kCMTimeInvalid, kCMTimeInvalid};
    if (self.timebase) timing.presentationTimeStamp = CMTimebaseGetTime(self.timebase);
    CMSampleBufferRef sb = NULL;
    CMSampleBufferCreateReadyWithImageBuffer(kCFAllocatorDefault, pb, self.format, &timing, &sb);
    CVPixelBufferRelease(pb);
    if (!sb) return;
    CFArrayRef atts = CMSampleBufferGetSampleAttachmentsArray(sb, YES);
    if (atts && CFArrayGetCount(atts) > 0) {
        CFMutableDictionaryRef a = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(atts, 0);
        CFDictionarySetValue(a, kCMSampleAttachmentKey_DisplayImmediately, kCFBooleanTrue);
    }
    if (self.layer.status == AVQueuedSampleBufferRenderingStatusFailed) [self.layer flush];
    [self.layer enqueueSampleBuffer:sb];
    CFRelease(sb);

    if (CACurrentMediaTime() - self.lastSync > 1.0) [self syncTimebase];
}

// MARK: AVPictureInPictureSampleBufferPlaybackDelegate

- (void)pictureInPictureController:(AVPictureInPictureController*)c setPlaying:(BOOL)playing {
    brls::sync([playing]() {
        MpvPlayer& p = MpvPlayer::getInstance();
        if (playing) p.play();
        else p.pause();
    });
    if (self.timebase) CMTimebaseSetRate(self.timebase, playing ? 1.0 : 0.0);
}

- (CMTimeRange)pictureInPictureControllerTimeRangeForPlayback:(AVPictureInPictureController*)c {
    const double d = MpvPlayer::getInstance().getDuration();
    if (d <= 0) return CMTimeRangeMake(kCMTimeNegativeInfinity, kCMTimePositiveInfinity);
    return CMTimeRangeMake(kCMTimeZero, CMTimeMakeWithSeconds(d, 600));
}

- (BOOL)pictureInPictureControllerIsPlaybackPaused:(AVPictureInPictureController*)c {
    return !MpvPlayer::getInstance().isPlaying();
}

- (void)pictureInPictureController:(AVPictureInPictureController*)c
         didTransitionToRenderSize:(CMVideoDimensions)newRenderSize {
}

- (void)pictureInPictureController:(AVPictureInPictureController*)c
                    skipByInterval:(CMTime)skipInterval
                 completionHandler:(void (^)(void))completionHandler {
    const double secs = CMTimeGetSeconds(skipInterval);
    brls::sync([secs]() { MpvPlayer::getInstance().seekRelative(secs); });
    if (completionHandler) completionHandler();
}

// MARK: AVPictureInPictureControllerDelegate

- (void)pictureInPictureControllerDidStartPictureInPicture:(AVPictureInPictureController*)c {
    brls::Logger::info("PiP: started");
}

- (void)pictureInPictureControllerDidStopPictureInPicture:(AVPictureInPictureController*)c {
    brls::Logger::info("PiP: stopped");
}

- (void)pictureInPictureController:(AVPictureInPictureController*)c
    failedToStartPictureInPictureWithError:(NSError*)error {
    brls::Logger::warning("PiP: could not start: {}",
                          error ? error.localizedDescription.UTF8String : "?");
}

- (void)pictureInPictureController:(AVPictureInPictureController*)c
    restoreUserInterfaceForPictureInPictureStopWithCompletionHandler:(void (^)(BOOL))completionHandler {
    // The player screen is still open behind it; nothing to rebuild.
    if (completionHandler) completionHandler(YES);
}

@end

namespace vitaplex {
namespace pip {
namespace ios {

namespace {
VPPipController* g_pip API_AVAILABLE(ios(15.0)) = nil;
bool g_armed = false;
}  // namespace

bool available() {
    if (@available(iOS 15.0, *)) return [AVPictureInPictureController isPictureInPictureSupported];
    return false;
}

bool isActive() {
    if (@available(iOS 15.0, *)) return g_pip && g_pip.controller.isPictureInPictureActive;
    return false;
}

// A video is playing in the player (w/h > 0), or the player has gone.
void setPlaybackState(bool playing, int w, int h) {
    if (@available(iOS 15.0, *)) {
        if (!available()) return;
        @autoreleasepool {
            if (w > 0 && h > 0) {
                if (!g_pip) g_pip = [[VPPipController alloc] init];
                [g_pip attachToWindow];
                g_armed = true;
                g_pip.controller.canStartPictureInPictureAutomaticallyFromInline = YES;
                [g_pip syncTimebase];
                [g_pip.controller invalidatePlaybackState];
            } else {
                g_armed = false;
                if (g_pip) {
                    if (g_pip.controller.isPictureInPictureActive)
                        [g_pip.controller stopPictureInPicture];
                    g_pip.controller.canStartPictureInPictureAutomaticallyFromInline = NO;
                    [g_pip.layer flushAndRemoveImage];
                }
            }
            (void)playing;
        }
    }
}

bool enter() {
    if (@available(iOS 15.0, *)) {
        if (!g_pip || !g_armed) return false;
        if (!g_pip.controller.isPictureInPicturePossible) {
            brls::Logger::warning("PiP: not possible yet (no frame shown?)");
            return false;
        }
        [g_pip.controller startPictureInPicture];
        return true;
    }
    return false;
}

bool leave() {
    if (@available(iOS 15.0, *)) {
        if (!g_pip || !g_pip.controller.isPictureInPictureActive) return false;
        [g_pip.controller stopPictureInPicture];
        return true;
    }
    return false;
}

bool wantsFrames() {
    return g_armed;
}

void pushFrameRGBA(const void* pixels, int w, int h, int stride) {
    if (@available(iOS 15.0, *)) {
        if (!g_pip || !g_armed || !pixels) return;
        @autoreleasepool {
            [g_pip pushRGBA:(const uint8_t*)pixels width:w height:h stride:stride];
        }
    }
}

}  // namespace ios
}  // namespace pip
}  // namespace vitaplex
