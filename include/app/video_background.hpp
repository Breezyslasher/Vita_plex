/**
 * VitaPlex - Video in the background
 * A video's sound playing on once its player is left with Back (the
 * "Keep Video Audio Playing" setting), as music plays on.
 *
 * The player (PlayerActivity) goes, mpv plays on with its picture off, and
 * this keeps what the player kept while it was up: mpv's events pumped, where
 * the video is told to the server every few seconds, the place saved as it is
 * stopped, and the video marked watched as it ends. Back from a tab opens the
 * player again where the sound is (reopen): the video is loaded again there,
 * so the picture is a moment in coming.
 *
 * When the video ends here, the next of the video queue opens in the player;
 * the next episode of a series is not gone on to.
 * UI thread only.
 */

#pragma once

#include <borealis.hpp>
#include <string>

namespace vitaplex {

class VideoBackground {
public:
    static VideoBackground& getInstance();

    struct Session {
        std::string ratingKey;
        std::string title;
        bool isLocalFile = false;   // a download, its place kept by DownloadsManager
        int baseOffsetMs = 0;       // where the stream mpv plays begins in the video
        int durationMs = 0;         // the whole video's, 0 if not known
    };

    // The player is going and mpv plays on: taken over from here.
    void begin(const Session& session);
    bool isActive() const { return m_active; }
    // Stopped, its place saved: something else is to play, or the app goes.
    void stop();
    // Back into the player where the sound is. false when nothing plays here.
    bool reopen();

private:
    VideoBackground() = default;
    void tick();
    int positionMs() const;
    void report(const char* state);
    void finish();

    bool m_active = false;
    Session m_session;
    brls::RepeatingTimer m_timer;
    bool m_timerSet = false;
    int m_ticks = 0;
};

}  // namespace vitaplex
