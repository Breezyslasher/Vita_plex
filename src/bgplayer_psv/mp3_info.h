/**
 * What a player reading an MP3 has to go on: an ID3v2 tag in front, the first
 * frame's bitrate, a Xing/Info or VBRI frame giving the frame count, and the
 * frames themselves. A variable-bitrate MP3 with no such header leaves a
 * player to work out its length from the first frame's bitrate, and the shell
 * gave the server's MP3 of a 2:51 song as 19:17 long (log 1a1cef7f). This says
 * which of those the file has, without the file leaving the console.
 */

#pragma once

typedef struct Mp3Info {
    long fileSize;
    long id3Size;       // the ID3v2 tag at the start, 0 if none
    long firstFrame;    // offset of the first frame
    int version;        // 10 for MPEG-1, 20 for MPEG-2, 25 for MPEG-2.5
    int sampleRate;     // Hz
    int firstKbps;      // the first frame's bitrate
    char header[5];     // "Xing", "Info" or "VBRI" in the first frame, else ""
    long headerFrames;  // the frame count that header gives, -1 if none
    long frames;        // audio frames; a header's own frame is not one
    long audioBytes;    // the bytes those frames take
    int minKbps, maxKbps;
    double seconds;     // the length the frames add up to
    long trailing;      // bytes after the last frame (an ID3v1 tag is 128)
} Mp3Info;

// 0, or -1 when the file cannot be read or holds no MP3 frame.
int mp3Inspect(const char* path, Mp3Info* out);

// The length in seconds a player gets from the first frame's bitrate alone:
// every byte from the first frame on, at that bitrate.
double mp3FirstFrameGuess(const Mp3Info* info);

// Writes the frames of `in` to `out` behind a frame carrying their count, a
// Xing tag (Info when every frame has one bitrate), as LAME and ffmpeg put one
// in front when they can seek back to fill it in; a streamed transcode cannot.
// An ID3v2 tag in front is left out. `info` is mp3Inspect's for `in`, which
// must not have such a header already. 0, or -1.
int mp3WriteWithHeader(const char* in, const Mp3Info* info, const char* out);
