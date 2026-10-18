/**
 * MP3 inspection for the background test's player (mp3_info.h). Plain C, so
 * it builds and is checked on a host too.
 */

#include "mp3_info.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Layer III bitrates in kbps by index; 0 and 15 are not bitrates.
static const int kKbps[2][16] = {
    {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},  // MPEG-1
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},      // MPEG-2, 2.5
};
static const int kRates[3][3] = {
    {44100, 48000, 32000},  // MPEG-1
    {22050, 24000, 16000},  // MPEG-2
    {11025, 12000, 8000},   // MPEG-2.5
};

typedef struct Frame {
    int version;   // 10, 20 or 25
    int kbps;
    int rate;
    int length;    // bytes, header included
    int samples;   // per channel
    int sideInfo;  // bytes between the header and where a Xing/Info tag goes
} Frame;

// The Layer III frame header in these 4 bytes; 0 if they are not one. A
// free-format frame (bitrate index 0) has no length to follow, so it counts as
// none.
static int parseFrame(const unsigned char* h, Frame* f) {
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return 0;
    const int v = (h[1] >> 3) & 3;      // 0: 2.5, 1: reserved, 2: 2, 3: 1
    const int layer = (h[1] >> 1) & 3;  // 1: Layer III
    const int bi = h[2] >> 4;
    const int si = (h[2] >> 2) & 3;
    if (v == 1 || layer != 1 || bi == 0 || bi == 15 || si == 3) return 0;
    const int mpeg1 = v == 3;
    const int mono = (h[3] >> 6) == 3;
    f->version = mpeg1 ? 10 : v == 2 ? 20 : 25;
    f->kbps = kKbps[mpeg1 ? 0 : 1][bi];
    f->rate = kRates[mpeg1 ? 0 : v == 2 ? 1 : 2][si];
    f->samples = mpeg1 ? 1152 : 576;
    f->length = (mpeg1 ? 144000 : 72000) * f->kbps / f->rate + ((h[2] >> 1) & 1);
    f->sideInfo = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
    return 1;
}

static unsigned long be32(const unsigned char* p) {
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

// The file through a small window that only moves forward: the system-mode
// test app has 3 MB free, and a song is bigger than that.
typedef struct Reader {
    FILE* f;
    long start;  // file offset of buf[0]
    size_t len;
    unsigned char buf[16 * 1024];
} Reader;

// n bytes at pos, n no more than the window; NULL past the end of the file.
static const unsigned char* bytesAt(Reader* r, long pos, size_t n) {
    if (pos < r->start) return NULL;
    const long end = r->start + (long)r->len;
    if (pos + (long)n <= end) return r->buf + (pos - r->start);
    size_t keep = 0;
    if (pos < end) {
        keep = (size_t)(end - pos);
        memmove(r->buf, r->buf + (pos - r->start), keep);
    } else if (fseek(r->f, pos, SEEK_SET) != 0) {
        return NULL;
    }
    r->start = pos;
    r->len = keep + fread(r->buf + keep, 1, sizeof(r->buf) - keep, r->f);
    return r->len >= n ? r->buf : NULL;
}

int mp3Inspect(const char* path, Mp3Info* out) {
    memset(out, 0, sizeof(*out));
    out->headerFrames = -1;
    Reader* r = (Reader*)malloc(sizeof(Reader));
    if (!r) return -1;
    r->start = 0;
    r->len = 0;
    r->f = fopen(path, "rb");
    if (!r->f) {
        free(r);
        return -1;
    }

    long pos = 0;
    const unsigned char* p = bytesAt(r, 0, 10);
    if (p && memcmp(p, "ID3", 3) == 0) {
        const long size = ((long)(p[6] & 0x7F) << 21) | ((long)(p[7] & 0x7F) << 14) |
                          ((long)(p[8] & 0x7F) << 7) | (long)(p[9] & 0x7F);
        out->id3Size = 10 + size + ((p[5] & 0x10) ? 10 : 0);  // 0x10: a footer follows
        pos = out->id3Size;
    }

    // The first frame: a header, and another where its length says the next
    // one starts, so a stray 0xFF in the tag's padding is not taken for one.
    Frame f, next;
    int found = 0;
    // Both are read through one window from pos, since the window only moves
    // forward and the search may still need pos + 1.
    for (const long limit = pos + 64 * 1024; pos < limit; pos++) {
        p = bytesAt(r, pos, 4);
        if (!p) break;
        if (!parseFrame(p, &f)) continue;
        p = bytesAt(r, pos, (size_t)f.length + 4);
        if (p && parseFrame(p + f.length, &next)) {
            found = 1;
            break;
        }
    }
    if (!found) {
        fclose(r->f);
        free(r);
        return -1;
    }
    out->firstFrame = pos;
    out->version = f.version;
    out->sampleRate = f.rate;
    out->firstKbps = f.kbps;

    // A Xing or Info tag sits after the first frame's side information; VBRI
    // (Fraunhofer's) 32 bytes after the header, whatever the channels.
    p = bytesAt(r, pos, 4 + 32 + 18);
    if (p) {
        const unsigned char* x = p + 4 + f.sideInfo;
        if (memcmp(x, "Xing", 4) == 0 || memcmp(x, "Info", 4) == 0) {
            memcpy(out->header, x, 4);
            if (be32(x + 4) & 1) out->headerFrames = (long)be32(x + 8);  // flag 1: frames
        } else if (memcmp(p + 4 + 32, "VBRI", 4) == 0) {
            memcpy(out->header, "VBRI", 4);
            out->headerFrames = (long)be32(p + 4 + 32 + 14);
        }
    }

    // Every frame after that, until something that is not one.
    long at = pos;
    for (int first = 1;; first = 0) {
        p = bytesAt(r, at, 4);
        if (!p || !parseFrame(p, &f)) break;
        if (!(first && out->header[0])) {
            out->frames++;
            out->audioBytes += f.length;
            out->seconds += (double)f.samples / f.rate;
            if (out->minKbps == 0 || f.kbps < out->minKbps) out->minKbps = f.kbps;
            if (f.kbps > out->maxKbps) out->maxKbps = f.kbps;
        }
        at += f.length;
    }
    fseek(r->f, 0, SEEK_END);
    out->fileSize = ftell(r->f);
    out->trailing = out->fileSize > at ? out->fileSize - at : 0;
    fclose(r->f);
    free(r);
    return 0;
}

double mp3FirstFrameGuess(const Mp3Info* info) {
    if (info->firstKbps <= 0) return 0;
    return (double)(info->fileSize - info->firstFrame) * 8.0 / (info->firstKbps * 1000.0);
}

static void putBe32(unsigned char* p, unsigned long v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

int mp3WriteWithHeader(const char* in, const Mp3Info* info, const char* out) {
    if (info->header[0] || info->frames <= 0) return -1;
    FILE* src = fopen(in, "rb");
    if (!src) return -1;
    unsigned char h[4];
    Frame f;
    if (fseek(src, info->firstFrame, SEEK_SET) != 0 || fread(h, 1, 4, src) != 4 ||
        !parseFrame(h, &f)) {
        fclose(src);
        return -1;
    }

    // The smallest frame of the stream's own kind that holds the tag: the
    // header, the side information (zero), then the tag's id, flags, frame
    // count and byte count. Same version, sample rate and channels; no CRC
    // and no padding, so the tag sits where mp3Inspect looks for it.
    const int mpeg1 = f.version == 10;
    const int need = 4 + f.sideInfo + 16;
    int bi = 1, len = 0;
    for (; bi < 15; bi++) {
        len = (mpeg1 ? 144000 : 72000) * kKbps[mpeg1 ? 0 : 1][bi] / f.rate;
        if (len >= need) break;
    }
    unsigned char frame[1536];
    if (bi == 15 || len > (int)sizeof(frame)) {
        fclose(src);
        return -1;
    }
    memset(frame, 0, (size_t)len);
    frame[0] = 0xFF;
    frame[1] = (unsigned char)(h[1] | 0x01);
    frame[2] = (unsigned char)((bi << 4) | (h[2] & 0x0C));
    frame[3] = h[3];
    unsigned char* x = frame + 4 + f.sideInfo;
    memcpy(x, info->minKbps == info->maxKbps ? "Info" : "Xing", 4);
    putBe32(x + 4, 0x3);  // frame and byte counts follow
    putBe32(x + 8, (unsigned long)info->frames);
    putBe32(x + 12, (unsigned long)(len + info->audioBytes));

    FILE* dst = fopen(out, "wb");
    unsigned char* buf = (unsigned char*)malloc(16 * 1024);
    int ok = dst && buf && fwrite(frame, 1, (size_t)len, dst) == (size_t)len &&
             fseek(src, info->firstFrame, SEEK_SET) == 0;
    for (long left = info->audioBytes; ok && left > 0;) {
        const size_t n = fread(buf, 1, left < 16 * 1024 ? (size_t)left : 16 * 1024, src);
        ok = n > 0 && fwrite(buf, 1, n, dst) == n;
        left -= (long)n;
    }
    free(buf);
    fclose(src);
    if (dst && fclose(dst) != 0) ok = 0;
    return ok ? 0 : -1;
}
