/**
 * VitaPlex - iOS / tvOS "Now Playing" backend
 *
 * The Apple side of include/utils/now_playing.hpp:
 *
 *  - AVAudioSession in the Playback category, made active when something
 *    plays. Without it iOS silences the app when the phone locks, another app
 *    comes to the front or the ring/silent switch is set to silent, which is
 *    what "background music" needs above all. Info.plist already declares the
 *    "audio" background mode.
 *  - MPNowPlayingInfoCenter: the lock screen / Control Centre card (title,
 *    artist, album, cover, position).
 *  - MPRemoteCommandCenter: play / pause / next / previous / seek / repeat /
 *    shuffle from that card, from AirPods and wired headphone buttons, and from
 *    a car's Bluetooth controls.
 *  - Interruptions (a call, Siri, an alarm) pause and, when iOS says so, resume;
 *    headphones being unplugged pause, as on Android.
 *
 * Every command reaches the app through brls::sync, so it runs in the main loop
 * between frames like the Android JNI path, not in the middle of whatever UIKit
 * was doing when it delivered it.
 */

#include "utils/now_playing.hpp"

#include <borealis.hpp>

#include <TargetConditionals.h>
#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#import <MediaPlayer/MediaPlayer.h>
#import <UIKit/UIKit.h>

#include <string>

namespace vitaplex {
namespace nowplaying {
namespace detail {

namespace {

bool g_commandsWired = false;
bool g_sessionActive = false;

// The cover currently on the card, and the URL it came from, so a refresh of
// the position does not download it again.
NSString* g_artUrl = nil;
MPMediaItemArtwork* g_artwork = nil;
// Bumped on every new track, so a slow download for the previous one is dropped.
uint64_t g_artGeneration = 0;

void post(Transport t) {
    brls::sync([t]() { dispatchTransport(t); });
}

void configureSession() {
    static bool configured = false;
    if (configured) return;
    configured = true;

    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSError* err = nil;
    if (![session setCategory:AVAudioSessionCategoryPlayback error:&err]) {
        brls::Logger::warning("Now playing: audio session category: {}",
                              err ? err.localizedDescription.UTF8String : "?");
    }

    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];

    // A call, Siri, an alarm: iOS has already stopped our audio when "began"
    // arrives, so make the app agree; resume only when iOS says it should.
    [nc addObserverForName:AVAudioSessionInterruptionNotification
                    object:session
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification* note) {
                    const NSUInteger type =
                        [note.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
                    if (type == AVAudioSessionInterruptionTypeBegan) {
                        brls::Logger::info("Now playing: interrupted, pausing");
                        post(Transport::Pause);
                    } else if (type == AVAudioSessionInterruptionTypeEnded) {
                        const NSUInteger opts =
                            [note.userInfo[AVAudioSessionInterruptionOptionKey] unsignedIntegerValue];
                        if (opts & AVAudioSessionInterruptionOptionShouldResume) {
                            brls::Logger::info("Now playing: interruption over, resuming");
                            [[AVAudioSession sharedInstance] setActive:YES error:nil];
                            post(Transport::Play);
                        }
                    }
                }];

    // Headphones or a Bluetooth speaker went away: pause rather than carry on
    // out of the phone's speaker.
    [nc addObserverForName:AVAudioSessionRouteChangeNotification
                    object:session
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification* note) {
                    const NSUInteger reason =
                        [note.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue];
                    if (reason == AVAudioSessionRouteChangeReasonOldDeviceUnavailable) {
                        brls::Logger::info("Now playing: audio device removed, pausing");
                        post(Transport::Pause);
                    }
                }];
}

void activateSession() {
    if (g_sessionActive) return;
    NSError* err = nil;
    if ([[AVAudioSession sharedInstance] setActive:YES error:&err]) {
        g_sessionActive = true;
    } else {
        brls::Logger::warning("Now playing: audio session activation: {}",
                              err ? err.localizedDescription.UTF8String : "?");
    }
}

void wireCommands() {
    if (g_commandsWired) return;
    g_commandsWired = true;

    MPRemoteCommandCenter* cc = [MPRemoteCommandCenter sharedCommandCenter];
    [cc.playCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
        post(Transport::Play);
        return MPRemoteCommandHandlerStatusSuccess;
    }];
    [cc.pauseCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
        post(Transport::Pause);
        return MPRemoteCommandHandlerStatusSuccess;
    }];
    [cc.togglePlayPauseCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
        post(Transport::Toggle);
        return MPRemoteCommandHandlerStatusSuccess;
    }];
    [cc.stopCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
        post(Transport::Stop);
        return MPRemoteCommandHandlerStatusSuccess;
    }];
    [cc.nextTrackCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
        post(Transport::Next);
        return MPRemoteCommandHandlerStatusSuccess;
    }];
    [cc.previousTrackCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
        post(Transport::Previous);
        return MPRemoteCommandHandlerStatusSuccess;
    }];
    [cc.changePlaybackPositionCommand
        addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* event) {
            const int64_t ms =
                (int64_t)(((MPChangePlaybackPositionCommandEvent*)event).positionTime * 1000.0);
            brls::sync([ms]() { dispatchSeek(ms); });
            return MPRemoteCommandHandlerStatusSuccess;
        }];
#if !TARGET_OS_TV
    [cc.changeRepeatModeCommand
        addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* event) {
            RepeatMode mode = RepeatMode::Off;
            switch (((MPChangeRepeatModeCommandEvent*)event).repeatType) {
                case MPRepeatTypeOne: mode = RepeatMode::One; break;
                case MPRepeatTypeAll: mode = RepeatMode::All; break;
                default:              mode = RepeatMode::Off; break;
            }
            brls::sync([mode]() { dispatchSetRepeat(mode); });
            return MPRemoteCommandHandlerStatusSuccess;
        }];
    [cc.changeShuffleModeCommand
        addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* event) {
            const bool on = ((MPChangeShuffleModeCommandEvent*)event).shuffleType != MPShuffleTypeOff;
            brls::sync([on]() { dispatchSetShuffle(on); });
            return MPRemoteCommandHandlerStatusSuccess;
        }];
#endif
    // Skip-by-interval buttons would replace next/previous on the card.
    cc.skipForwardCommand.enabled  = NO;
    cc.skipBackwardCommand.enabled = NO;
}

NSString* ns(const std::string& s) {
    return [NSString stringWithUTF8String:s.c_str()] ?: @"";
}

// Fetch the cover off the main thread, then put it on the card if the track is
// still the one it was fetched for.
void loadArtwork(const std::string& artUrl) {
    NSString* url = ns(artUrl);
    if (url.length == 0) {
        g_artUrl = nil;
        g_artwork = nil;
        return;
    }
    if (g_artUrl && [g_artUrl isEqualToString:url]) return;
    g_artUrl = url;
    g_artwork = nil;
    const uint64_t generation = ++g_artGeneration;

    NSURL* nsurl = ([url hasPrefix:@"http://"] || [url hasPrefix:@"https://"])
                       ? [NSURL URLWithString:url]
                       : [NSURL fileURLWithPath:url];
    if (!nsurl) return;

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSData* data = [NSData dataWithContentsOfURL:nsurl];
        UIImage* image = data ? [UIImage imageWithData:data] : nil;
        if (!image) return;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (generation != g_artGeneration) return;
            g_artwork = [[MPMediaItemArtwork alloc]
                initWithBoundsSize:image.size
                    requestHandler:^UIImage*(CGSize) { return image; }];
            MPNowPlayingInfoCenter* center = [MPNowPlayingInfoCenter defaultCenter];
            NSMutableDictionary* info = [center.nowPlayingInfo mutableCopy];
            if (!info) return;
            info[MPMediaItemPropertyArtwork] = g_artwork;
            center.nowPlayingInfo = info;
        });
    });
}

}  // namespace

void iosUpdate(const Info& info) {
    @autoreleasepool {
        configureSession();
        wireCommands();
        if (info.playing) activateSession();

        MPRemoteCommandCenter* cc = [MPRemoteCommandCenter sharedCommandCenter];
        cc.nextTrackCommand.enabled     = info.hasNext ? YES : NO;
        cc.previousTrackCommand.enabled = info.hasPrev ? YES : NO;
#if !TARGET_OS_TV
        cc.changeRepeatModeCommand.enabled  = info.showRepeat ? YES : NO;
        cc.changeShuffleModeCommand.enabled = info.showShuffle ? YES : NO;
        if (info.showRepeat) {
            cc.changeRepeatModeCommand.currentRepeatType =
                info.repeat == RepeatMode::One   ? MPRepeatTypeOne
                : info.repeat == RepeatMode::All ? MPRepeatTypeAll
                                                 : MPRepeatTypeOff;
        }
        if (info.showShuffle) {
            cc.changeShuffleModeCommand.currentShuffleType =
                info.shuffle ? MPShuffleTypeItems : MPShuffleTypeOff;
        }
#endif

        loadArtwork(info.artUrl);

        NSMutableDictionary* np = [NSMutableDictionary dictionary];
        np[MPMediaItemPropertyTitle] = ns(info.title);
        if (!info.artist.empty()) np[MPMediaItemPropertyArtist] = ns(info.artist);
        if (!info.album.empty()) np[MPMediaItemPropertyAlbumTitle] = ns(info.album);
        if (info.durationMs > 0)
            np[MPMediaItemPropertyPlaybackDuration] = @(info.durationMs / 1000.0);
        np[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(info.positionMs / 1000.0);
        np[MPNowPlayingInfoPropertyPlaybackRate] = @(info.playing ? 1.0 : 0.0);
        np[MPNowPlayingInfoPropertyDefaultPlaybackRate] = @1.0;
        if (g_artwork) np[MPMediaItemPropertyArtwork] = g_artwork;
        [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo = np;
    }
}

void iosClear() {
    @autoreleasepool {
        [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo = nil;
        g_artUrl = nil;
        g_artwork = nil;
        ++g_artGeneration;
        if (g_sessionActive) {
            // Let other apps' audio resume where we interrupted it.
            [[AVAudioSession sharedInstance]
                  setActive:NO
                withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                      error:nil];
            g_sessionActive = false;
        }
    }
}

}  // namespace detail
}  // namespace nowplaying
}  // namespace vitaplex
