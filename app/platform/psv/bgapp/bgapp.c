/*
 * VitaPlex PS Vita background helper — Phase-1b: real downloaded-track playback.
 *
 * Phase 0 proved this eboot2.bin keeps running after VitaPlex is backgrounded;
 * Phase 1a proved it can output audible audio via sceAudioOut in LiveArea. This
 * plays the ACTUAL song when VitaPlex is backgrounded while a DOWNLOADED music
 * track is playing (streamed tracks stay out of scope — they fall back to a tone).
 *
 * Decoding is done in software on the CPU, NOT via the Vita's hardware audio
 * decoder (SceAudiodec): sceAudiodecCreateDecoder returns UNSUPPORTED from a
 * background app (the shared codec engine isn't available to us), whereas
 * sceAudioOut works fine. So:
 *   - WAV : parse the RIFF header and stream the PCM straight to sceAudioOut.
 *   - MP3 : decode with the tiny, dependency-free minimp3 (vendored here).
 * Format is detected by magic bytes because a Plex download keeps its source
 * container (a file.wav part downloads as .wav, file.mp3 as .mp3).
 *
 * IPC with the main app (separate process, no shared memory) via ux0:data/VitaPlex:
 *   bgm_tick   : counter bumped ~4x/sec while VitaPlex's main loop runs; FREEZES
 *                when the app is suspended (how we detect backgrounding).
 *   bgm_status : line 1 '1'/'0' playing, line 2 title, line 3 local file path
 *                (downloaded track, else empty), line 4 position ms (unused yet —
 *                playback starts from the beginning; position-resume is a follow-up).
 *
 * On takeover we decode+play line-3's file; if there's no local file or the
 * format is unsupported we play the Phase-1a tone so takeovers stay audible +
 * the log says why. Resident while(1)+PowerTick follows GrapheneCt's BGFTP pattern.
 */

#include <psp2/types.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/audioout.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

/* In SceNotificationUtil's stub (which vitasdk ships). */
extern int sceNotificationUtilBgAppInitialize(void);

#define DIR_DATA     "ux0:data"
#define DIR_APP      "ux0:data/VitaPlex"
#define LOG_PATH     "ux0:data/VitaPlex/bgapp.log"
#define TICK_PATH    "ux0:data/VitaPlex/bgm_tick"
#define STATUS_PATH  "ux0:data/VitaPlex/bgm_status"

#define TONE_FREQ     48000
#define TONE_GRAIN    1024
#define TONE_HZ       440
#define TONE_AMP      6000

#define POLL_US       200000    /* 200 ms watchdog poll */
#define STALL_TAKEOVER 6        /* ~1.2 s of frozen tick => app backgrounded */

#define INBUF_SZ      65536      /* MP3 read buffer */
#define WAV_GRAIN     1024

static void log_append(const char *s)
{
    SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) { sceIoWrite(fd, s, strlen(s)); sceIoClose(fd); }
}

/* --- shared control between the watchdog (main) and the audio thread --- */
static volatile int g_should_play = 0;

/* Opens a stereo BGM port (MAIN fallback) at `freq` with `grain` samples and
   sets it to full volume. Returns the port, or < 0 on failure. */
static int open_port(int freq, int grain)
{
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, grain, freq, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0)
        port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, grain, freq, SCE_AUDIO_OUT_MODE_STEREO);
    if (port >= 0)
    {
        int vol[2] = { SCE_AUDIO_OUT_MAX_VOL, SCE_AUDIO_OUT_MAX_VOL };
        sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    }
    return port;
}

/* ---- Phase-1a fallback tone --------------------------------------------- */

static int16_t g_sine[TONE_FREQ / TONE_HZ + 2];
static int g_sine_len = 0;

static void build_sine(void)
{
    g_sine_len = TONE_FREQ / TONE_HZ;
    for (int i = 0; i < g_sine_len; i++)
        g_sine[i] = (int16_t)(sinf((float) i / (float) g_sine_len * 2.0f * 3.14159265f) * TONE_AMP);
}

static void play_tone_session(void)
{
    int port = open_port(TONE_FREQ, TONE_GRAIN);
    if (port < 0) { log_append("[bgapp] tone: open port FAILED\n"); return; }
    log_append("[bgapp] tone: playing (fallback)\n");

    static int16_t buf[TONE_GRAIN * 2];
    int idx = 0;
    while (g_should_play)
    {
        for (int i = 0; i < TONE_GRAIN; i++)
        {
            int16_t s = g_sine[idx];
            if (++idx >= g_sine_len) idx = 0;
            buf[2 * i] = s; buf[2 * i + 1] = s;
        }
        sceAudioOutOutput(port, buf);
    }
    sceAudioOutReleasePort(port);
}

/* ---- WAV playback (raw PCM, no decoder) --------------------------------- */

static unsigned int rd_u32(const unsigned char *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned) p[3] << 24);
}

/* Returns 1 if it set up an audio port and streamed, 0 on unsupported format. */
static int play_wav_session(SceUID fd)
{
    unsigned char hdr[4096];
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    int hn = sceIoRead(fd, hdr, sizeof hdr);
    if (hn < 44 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) return 0;

    int fmt = 0, ch = 0, srate = 0, bits = 0;
    long data_off = 0, data_size = 0;
    int p = 12;
    while (p + 8 <= hn)
    {
        const unsigned char *c = hdr + p;
        unsigned int csz = rd_u32(c + 4);
        if (!memcmp(c, "fmt ", 4) && csz >= 16 && p + 8 + 16 <= hn)
        {
            const unsigned char *f = c + 8;
            fmt   = f[0] | (f[1] << 8);
            ch    = f[2] | (f[3] << 8);
            srate = (int) rd_u32(f + 4);
            bits  = f[14] | (f[15] << 8);
        }
        else if (!memcmp(c, "data", 4))
        {
            data_off = p + 8;
            data_size = (long) csz;
            break;
        }
        p += 8 + csz + (csz & 1);
    }

    if (fmt != 1 || bits != 16 || ch < 1 || ch > 2 || srate <= 0 || data_off == 0)
    {
        char b[128];
        snprintf(b, sizeof b, "[bgapp] wav: unsupported fmt=%d ch=%d bits=%d sr=%d\n", fmt, ch, bits, srate);
        log_append(b);
        return 0;
    }

    int port = open_port(srate, WAV_GRAIN);
    if (port < 0) { char b[64]; snprintf(b, sizeof b, "[bgapp] wav: port open FAILED sr=%d\n", srate); log_append(b); return 0; }

    { char b[96]; snprintf(b, sizeof b, "[bgapp] wav: playing sr=%d ch=%d\n", srate, ch); log_append(b); }

    static int16_t rdbuf[WAV_GRAIN * 2];
    static int16_t out[WAV_GRAIN * 2];
    sceIoLseek(fd, data_off, SCE_SEEK_SET);
    long remaining = data_size;

    while (g_should_play && remaining > 0)
    {
        int bytes = WAV_GRAIN * ch * 2;
        if (bytes > remaining) bytes = (int) remaining;
        int got = sceIoRead(fd, rdbuf, bytes);
        if (got <= 0) break;
        remaining -= got;

        int frames = got / (ch * 2);
        if (ch == 2)
            memcpy(out, rdbuf, (size_t) frames * 4);
        else
            for (int i = 0; i < frames; i++) { out[2 * i] = rdbuf[i]; out[2 * i + 1] = rdbuf[i]; }
        for (int i = frames; i < WAV_GRAIN; i++) { out[2 * i] = 0; out[2 * i + 1] = 0; }

        sceAudioOutOutput(port, out);
    }

    if (remaining <= 0) log_append("[bgapp] wav: track EOF\n");
    sceAudioOutReleasePort(port);
    while (g_should_play) sceKernelDelayThread(100000);
    return 1;
}

/* ---- MP3 playback (minimp3 software decode) ----------------------------- */

static unsigned char g_inbuf[INBUF_SZ];
static mp3dec_t g_mp3d;
static short g_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
static short g_out[MINIMP3_MAX_SAMPLES_PER_FRAME];

static int play_mp3_session(SceUID fd)
{
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    int in_len = sceIoRead(fd, g_inbuf, INBUF_SZ), in_pos = 0, eof = 0;
    if (in_len < 4) return 0;

    mp3dec_init(&g_mp3d);
    mp3dec_frame_info_t fi;
    int port = -1, grain = 0, frames = 0;

    while (g_should_play)
    {
        if (in_len - in_pos < 16384 && !eof)
        {
            int rem = in_len - in_pos;
            if (rem > 0 && in_pos > 0) memmove(g_inbuf, g_inbuf + in_pos, rem);
            in_pos = 0; in_len = rem;
            int got = sceIoRead(fd, g_inbuf + in_len, INBUF_SZ - in_len);
            if (got > 0) in_len += got; else eof = 1;
        }
        if (in_len - in_pos <= 0) break;   /* EOF */

        int samples = mp3dec_decode_frame(&g_mp3d, g_inbuf + in_pos, in_len - in_pos, g_pcm, &fi);
        if (fi.frame_bytes == 0) break;    /* not enough data for a frame + EOF */
        in_pos += fi.frame_bytes;
        if (samples <= 0) continue;        /* skipped ID3 / junk */

        if (port < 0)
        {
            grain = samples;
            port = open_port(fi.hz, grain);
            if (port < 0) { char b[64]; snprintf(b, sizeof b, "[bgapp] mp3: port open FAILED hz=%d\n", fi.hz); log_append(b); return 0; }
            char b[96]; snprintf(b, sizeof b, "[bgapp] mp3: playing hz=%d ch=%d spf=%d\n", fi.hz, fi.channels, grain);
            log_append(b);
        }

        int n = samples < grain ? samples : grain;
        if (fi.channels == 2)
            memcpy(g_out, g_pcm, (size_t) n * 4);
        else
            for (int i = 0; i < n; i++) { g_out[2 * i] = g_pcm[i]; g_out[2 * i + 1] = g_pcm[i]; }
        for (int i = n; i < grain; i++) { g_out[2 * i] = 0; g_out[2 * i + 1] = 0; }

        sceAudioOutOutput(port, g_out);
        if (((++frames) % 1000) == 0) { char b[64]; snprintf(b, sizeof b, "[bgapp] mp3: %d frames\n", frames); log_append(b); }
    }

    if (eof && in_len - in_pos <= 0) log_append("[bgapp] mp3: track EOF\n");
    if (port >= 0) sceAudioOutReleasePort(port);
    while (g_should_play) sceKernelDelayThread(100000);
    return port >= 0 ? 1 : 0;
}

/* Detects format by magic bytes and dispatches. Returns 1 if a decoder/port was
   set up (caller must NOT also play the tone), 0 to fall back to the tone. */
static int play_file_session(const char *path)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) { log_append("[bgapp] file: open failed\n"); return 0; }

    unsigned char magic[12];
    int mn = sceIoRead(fd, magic, sizeof magic);
    int ret = 0;
    if (mn >= 12 && !memcmp(magic, "RIFF", 4) && !memcmp(magic + 8, "WAVE", 4))
        ret = play_wav_session(fd);
    else if (mn >= 3 && (!memcmp(magic, "ID3", 3) || (magic[0] == 0xFF && (magic[1] & 0xE0) == 0xE0)))
        ret = play_mp3_session(fd);
    else
    {
        char b[96];
        snprintf(b, sizeof b, "[bgapp] file: unknown format %02x %02x %02x %02x\n",
                 magic[0], magic[1], magic[2], magic[3]);
        log_append(b);
    }

    sceIoClose(fd);
    return ret;
}

/* ---- status / tick files ------------------------------------------------- */

static void read_status(int *playing, char *path, int cap)
{
    if (playing) *playing = 0;
    if (path && cap) path[0] = 0;

    SceUID fd = sceIoOpen(STATUS_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) return;
    char b[512];
    int n = sceIoRead(fd, b, sizeof(b) - 1);
    sceIoClose(fd);
    if (n <= 0) return;
    b[n] = 0;

    if (playing) *playing = (b[0] == '1');

    if (path && cap)
    {
        int line = 0;
        char *s = b;
        while (*s && line < 2) { if (*s == '\n') line++; s++; }
        int i = 0;
        while (s[i] && s[i] != '\n' && i < cap - 1) { path[i] = s[i]; i++; }
        path[i] = 0;
    }
}

static int read_tick(int *out)
{
    SceUID fd = sceIoOpen(TICK_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    char b[32];
    int n = sceIoRead(fd, b, sizeof(b) - 1);
    sceIoClose(fd);
    if (n <= 0) return 0;
    b[n] = 0;
    *out = (int) strtol(b, NULL, 10);
    return 1;
}

/* ---- threads ------------------------------------------------------------- */

static int audio_thread(SceSize argc, void *argv)
{
    (void) argc; (void) argv;
    for (;;)
    {
        if (!g_should_play) { sceKernelDelayThread(50000); continue; }

        char path[300] = { 0 };
        read_status(NULL, path, sizeof path);

        int played = 0;
        if (path[0]) played = play_file_session(path);
        if (!played) play_tone_session();
    }
    return 0;
}

int main(void)
{
    sceIoMkdir(DIR_DATA, 0777);
    sceIoMkdir(DIR_APP, 0777);

    sceSysmoduleLoadModule(SCE_SYSMODULE_NOTIFICATION_UTIL);
    sceNotificationUtilBgAppInitialize();

    build_sine();
    log_append("[bgapp] ===== started (phase-1b: wav/mp3 software decode) =====\n");

    SceUID th = sceKernelCreateThread("vitaplex_bgm_audio", audio_thread,
                                      0x10000100, 0x20000, 0, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
    else log_append("[bgapp] audio: thread create FAILED\n");

    int last_tick = -1, stall = 0, tick = 0, want_tone = 0, hb = 0;

    for (;;)
    {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);

        int have_tick = read_tick(&tick);
        int playing = 0;
        read_status(&playing, NULL, 0);

        if (!have_tick) { stall = 0; last_tick = -1; }
        else if (tick != last_tick) { last_tick = tick; stall = 0; }
        else if (stall < 1000000) stall++;

        int backgrounded = have_tick && (stall >= STALL_TAKEOVER);
        int want = (playing && backgrounded) ? 1 : 0;

        if (want != want_tone)
        {
            want_tone = want;
            log_append(want ? "[bgapp] >>> taking over playback\n"
                            : "[bgapp] <<< releasing playback\n");
        }
        g_should_play = want;

        if (++hb >= 50)
        {
            hb = 0;
            char b[160];
            snprintf(b, sizeof b, "[bgapp] hb: tick=%d stall=%d playing=%d bg=%d play=%d\n",
                     tick, stall, playing, backgrounded, want);
            log_append(b);
        }

        sceKernelDelayThread(POLL_US);
    }
    return 0;
}
