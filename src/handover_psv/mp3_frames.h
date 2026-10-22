/**
 * The server's MP3 of a song, put behind a frame giving its length as it
 * comes from the server. A streamed transcode has no length header, so the
 * shell works a song's length out from its first frame's bitrate: 19:17 for
 * a 2:51 song (log 1a1cef7f), which the quick menu plugin takes for some
 * other song. Behind a Xing frame giving the frame count, the shell has the
 * right length (log e822493b).
 *
 * Fed the stream a piece at a time (mp3fFeed), this passes on its Layer III
 * frames from the first, without the ID3 tag before them or anything after
 * the last, behind room left for that frame; once the stream has ended,
 * mp3fFinish makes the frame, for the caller to write over the room at the
 * file's start: a Xing tag, or Info when every frame has one bitrate. A
 * stream whose first frame has such a tag of its own goes on as it is from
 * that frame. Up to build 2047 the plugin did this in two passes over the
 * whole file once it had come: 9.6 s for a 9.58 MB song in SceShell (log
 * 8e7a4b23).
 *
 * Plain C with no library calls, in 32 bits and with no division but by
 * 32-bit values, for the plugin, which has neither a C runtime nor 64-bit
 * division. VitaPlex (readyFetch) and its plugin (fetchMp3) both use it.
 */

#ifndef VITAPLEX_MP3_FRAMES_H
#define VITAPLEX_MP3_FRAMES_H

#include <stdint.h>

// A Layer III frame's header.
typedef struct Mp3Frame {
    int mpeg1;
    uint32_t kbps, rate;
    uint32_t length;    // bytes, header included
    uint32_t samples;   // per channel
    uint32_t sideInfo;  // bytes between the header and where a Xing/Info tag goes
} Mp3Frame;

static const uint16_t kMp3fKbps[2][16] = {
    {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},  // MPEG-1
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},      // MPEG-2, 2.5
};
static const uint16_t kMp3fRates[3][3] = {
    {44100, 48000, 32000},  // MPEG-1
    {22050, 24000, 16000},  // MPEG-2
    {11025, 12000, 8000},   // MPEG-2.5
};

// The header in these 4 bytes; 0 if they are not one. A free-format frame
// (bitrate index 0) has no length to follow, so it counts as none.
static inline int mp3fHeader(const uint8_t* h, Mp3Frame* f) {
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return 0;
    const int v = (h[1] >> 3) & 3;      // 0: 2.5, 1: reserved, 2: 2, 3: 1
    const int layer = (h[1] >> 1) & 3;  // 1: Layer III
    const int bi = h[2] >> 4, si = (h[2] >> 2) & 3;
    if (v == 1 || layer != 1 || bi == 0 || bi == 15 || si == 3) return 0;
    const int mono = (h[3] >> 6) == 3;
    f->mpeg1 = v == 3;
    f->kbps = kMp3fKbps[f->mpeg1 ? 0 : 1][bi];
    f->rate = kMp3fRates[f->mpeg1 ? 0 : v == 2 ? 1 : 2][si];
    f->samples = f->mpeg1 ? 1152 : 576;
    f->length = (f->mpeg1 ? 144000u : 72000u) * f->kbps / f->rate + ((h[2] >> 1) & 1);
    f->sideInfo = f->mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
    return 1;
}

static inline int mp3fBytesAre(const uint8_t* p, const char* s, int n) {
    for (int i = 0; i < n; i++)
        if (p[i] != (uint8_t)s[i]) return 0;
    return 1;
}

static inline void mp3fBe32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// Where the stream is: in its first bytes, which may be an ID3 tag; in that
// tag; before its first frame; among its frames; past the last; or with no
// frame found.
enum { MP3F_ID3, MP3F_TAG, MP3F_FIRST, MP3F_FRAMES, MP3F_END, MP3F_NONE };
// What is held between pieces: whole frames are passed on from it, a frame
// being at most 1441 bytes.
#define MP3F_HOLD 16384
// How far past the tag the first frame is looked for.
#define MP3F_SEARCH (64u * 1024u)

// Where what is passed on goes: 0, or below 0 to stop.
typedef int (*Mp3fWrite)(void* ctx, const uint8_t* data, uint32_t n);

typedef struct Mp3Frames {
    int state;
    uint8_t held[MP3F_HOLD];
    uint32_t heldLen;
    uint32_t tagLeft;   // bytes of the ID3 tag still to pass over
    uint32_t id3Size;   // the tag's, 0 for none
    uint32_t firstAt;   // where in the stream the first frame begins
    uint8_t head[4];    // its header
    uint32_t rate, firstKbps, minKbps, maxKbps;
    char tag[5];        // "Xing", "Info" or "VBRI" when the first frame is one, else ""
    // The frames passed on, the first one counted only if it is not a tag's.
    uint32_t passed, frames, audioBytes, samples;
    uint32_t room;      // bytes left at the start for the frame made, 0 for none
    uint32_t roomBi;    // its bitrate index
    uint8_t frame[1536];  // that frame: zeros, then made at the end
} Mp3Frames;

static inline void mp3fInit(Mp3Frames* m) {
    uint8_t* p = (uint8_t*)m;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*m); i++) p[i] = 0;
    m->state = MP3F_ID3;
}

// The first frame at pos of what is held, its header in f: what it says of
// the stream, and the room for the frame to be made, passed on as zeros.
static inline int mp3fBegin(Mp3Frames* m, uint32_t pos, const Mp3Frame* f, Mp3fWrite write,
                            void* ctx) {
    const uint8_t* const p = m->held + pos;
    for (int i = 0; i < 4; i++) m->head[i] = p[i];
    m->rate = f->rate;
    m->firstKbps = f->kbps;
    // A Xing or Info tag sits after the first frame's side information; VBRI
    // (Fraunhofer's) 32 bytes after the header, whatever the channels.
    const uint8_t* const x = p + 4 + f->sideInfo;
    if (mp3fBytesAre(x, "Xing", 4) || mp3fBytesAre(x, "Info", 4)) {
        for (int i = 0; i < 4; i++) m->tag[i] = (char)x[i];
    } else if (mp3fBytesAre(p + 4 + 32, "VBRI", 4)) {
        for (int i = 0; i < 4; i++) m->tag[i] = "VBRI"[i];
    }
    m->state = MP3F_FRAMES;
    if (m->tag[0]) return 0;
    // The smallest frame of the stream's own kind that holds the tag: the
    // header, the side information, then the tag's id, flags, frame count
    // and byte count.
    const uint32_t need = 4 + f->sideInfo + 16;
    uint32_t bi = 1, len = 0;
    for (; bi < 15; bi++) {
        len = (f->mpeg1 ? 144000u : 72000u) * kMp3fKbps[f->mpeg1 ? 0 : 1][bi] / f->rate;
        if (len >= need) break;
    }
    if (bi == 15 || len > sizeof(m->frame)) {
        m->state = MP3F_NONE;
        return 0;
    }
    m->room = len;
    m->roomBi = bi;
    return write(ctx, m->frame, len);
}

// What is held, worked through as far as it goes: the tag passed over, the
// first frame found, whole frames passed on in one piece; what is left of a
// frame kept for the next piece. Below 0 when write said to stop.
static inline int mp3fWork(Mp3Frames* m, Mp3fWrite write, void* ctx) {
    uint32_t pos = 0;
    Mp3Frame f, next;
    if (m->state == MP3F_ID3) {
        if (m->heldLen < 10) return 0;
        const uint8_t* const p = m->held;
        if (mp3fBytesAre(p, "ID3", 3)) {
            const uint32_t size = ((uint32_t)(p[6] & 0x7F) << 21) | ((uint32_t)(p[7] & 0x7F) << 14) |
                                  ((uint32_t)(p[8] & 0x7F) << 7) | (uint32_t)(p[9] & 0x7F);
            m->id3Size = 10 + size + ((p[5] & 0x10) ? 10 : 0);  // 0x10: a footer follows
            if (m->id3Size <= m->heldLen) {
                pos = m->id3Size;
                m->state = MP3F_FIRST;
            } else {
                m->tagLeft = m->id3Size - m->heldLen;
                pos = m->heldLen;
                m->state = MP3F_TAG;
            }
        } else {
            m->state = MP3F_FIRST;
        }
        m->firstAt = m->id3Size;
    }
    // A header, and another where its length says the next one starts, so a
    // stray 0xFF in the tag's padding is not taken for one.
    while (m->state == MP3F_FIRST) {
        if (m->firstAt - m->id3Size > MP3F_SEARCH) {
            m->state = MP3F_NONE;
            break;
        }
        if (m->heldLen - pos < 4) break;
        if (!mp3fHeader(m->held + pos, &f)) {
            pos++;
            m->firstAt++;
            continue;
        }
        const uint32_t want = f.length + 4 > 54 ? f.length + 4 : 54;
        if (m->heldLen - pos < want) break;
        if (!mp3fHeader(m->held + pos + f.length, &next)) {
            pos++;
            m->firstAt++;
            continue;
        }
        const int rc = mp3fBegin(m, pos, &f, write, ctx);
        if (rc < 0) return rc;
    }
    if (m->state == MP3F_FRAMES) {
        const uint32_t start = pos;
        while (m->heldLen - pos >= 4) {
            if (!mp3fHeader(m->held + pos, &f)) {
                m->state = MP3F_END;
                break;
            }
            if (m->heldLen - pos < f.length) break;
            if (m->passed > 0 || !m->tag[0]) {
                m->frames++;
                m->audioBytes += f.length;
                m->samples += f.samples;
                if (m->minKbps == 0 || f.kbps < m->minKbps) m->minKbps = f.kbps;
                if (f.kbps > m->maxKbps) m->maxKbps = f.kbps;
            }
            m->passed++;
            pos += f.length;
        }
        if (pos > start) {
            const int rc = write(ctx, m->held + start, pos - start);
            if (rc < 0) return rc;
        }
    }
    if (m->state >= MP3F_END) {
        m->heldLen = 0;  // the rest is not passed on
        return 0;
    }
    for (uint32_t i = pos; i < m->heldLen; i++) m->held[i - pos] = m->held[i];
    m->heldLen -= pos;
    return 0;
}

// The stream's next n bytes. Below 0 when write said to stop.
static inline int mp3fFeed(Mp3Frames* m, const uint8_t* data, uint32_t n, Mp3fWrite write,
                           void* ctx) {
    while (n > 0) {
        if (m->state >= MP3F_END) return 0;
        if (m->state == MP3F_TAG) {
            const uint32_t skip = n < m->tagLeft ? n : m->tagLeft;
            data += skip;
            n -= skip;
            m->tagLeft -= skip;
            if (m->tagLeft == 0) m->state = MP3F_FIRST;
            continue;
        }
        uint32_t take = MP3F_HOLD - m->heldLen;
        if (take > n) take = n;
        for (uint32_t i = 0; i < take; i++) m->held[m->heldLen + i] = data[i];
        m->heldLen += take;
        data += take;
        n -= take;
        const int rc = mp3fWork(m, write, ctx);
        if (rc < 0) return rc;
    }
    return 0;
}

// Once the stream has ended (a frame cut short at its end is not one): 1
// with the frame to write over the room at the file's start in m->frame, of
// m->room bytes; 0 with none to write, the stream's first frame being a tag
// of its own; -1 when it had no audio frames.
static inline int mp3fFinish(Mp3Frames* m) {
    if (m->frames == 0 || m->state == MP3F_NONE) return -1;
    if (m->tag[0]) return 0;
    Mp3Frame f;
    if (!mp3fHeader(m->head, &f)) return -1;
    uint8_t* const p = m->frame;
    for (uint32_t i = 0; i < m->room; i++) p[i] = 0;
    // Same version, sample rate and channels, no CRC and no padding, so the
    // tag sits where the shell, and mp3fBegin, look for it.
    p[0] = 0xFF;
    p[1] = (uint8_t)(m->head[1] | 0x01);
    p[2] = (uint8_t)((m->roomBi << 4) | (m->head[2] & 0x0C));
    p[3] = m->head[3];
    uint8_t* const x = p + 4 + f.sideInfo;
    const char* const id = m->minKbps == m->maxKbps ? "Info" : "Xing";
    for (int i = 0; i < 4; i++) x[i] = (uint8_t)id[i];
    mp3fBe32(x + 4, 0x3);  // frame and byte counts follow
    mp3fBe32(x + 8, m->frames);
    mp3fBe32(x + 12, m->room + m->audioBytes);
    return 1;
}

// The frames' length in ms, from their samples.
static inline uint32_t mp3fMs(const Mp3Frames* m) {
    if (m->rate == 0) return 0;
    return m->samples / m->rate * 1000u + m->samples % m->rate * 1000u / m->rate;
}

#endif
