/*
 * VitaPlex PS Vita background helper — Phase-1b: real downloaded-track playback.
 *
 * Phase 0 proved this eboot2.bin keeps running after VitaPlex is backgrounded;
 * Phase 1a proved it can produce audible sound via sceAudioOut in LiveArea.
 * Phase 1b plays the ACTUAL song: when VitaPlex is backgrounded while a
 * DOWNLOADED music track is playing, this helper decodes that track's local
 * MP3 (Plex downloads are transcoded to mp3) with SceAudiodec and outputs it
 * through the same sceAudioOut path. Streamed (non-downloaded) tracks are out of
 * scope for now — they still fall back to the Phase-1a tone.
 *
 * The main app and this helper are separate processes that can't share memory,
 * so VitaPlex hands us state through files under ux0:data/VitaPlex:
 *   bgm_tick   : a counter bumped ~4x/sec while VitaPlex's main loop runs; it
 *                FREEZES when the app is suspended (how we detect backgrounding).
 *   bgm_status : line 1 = '1'/'0' playing flag
 *                line 2 = track title
 *                line 3 = local file path (downloaded track), or empty
 *                line 4 = position ms (currently informational; playback starts
 *                         from the beginning — position-resume is a follow-up).
 *
 * On takeover (playing AND tick stalled) we try to decode+play line-3's file;
 * if there's no local file or decode setup fails, we play the 440 Hz tone so the
 * takeover is still audible + the log says why. Resident while(1)+PowerTick
 * follows GrapheneCt's BGFTP pattern.
 */

#include <psp2/types.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/audioout.h>
#include <psp2/audiodec.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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

#define INBUF_SZ      32768     /* MP3 read/parse buffer */

static void log_append(const char *s)
{
    SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) { sceIoWrite(fd, s, strlen(s)); sceIoClose(fd); }
}

/* --- shared control between the watchdog (main) and the audio thread --- */
static volatile int g_should_play = 0;

/* ---- Phase-1a fallback tone --------------------------------------------- */

static int16_t g_sine[TONE_FREQ / TONE_HZ + 2];
static int g_sine_len = 0;

static void build_sine(void)
{
    g_sine_len = TONE_FREQ / TONE_HZ;
    for (int i = 0; i < g_sine_len; i++)
        g_sine[i] = (int16_t)(sinf((float) i / (float) g_sine_len * 2.0f * 3.14159265f) * TONE_AMP);
}

/* Plays the tone until the watchdog clears g_should_play. */
static void play_tone_session(void)
{
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, TONE_GRAIN, TONE_FREQ,
                                   SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0)
        port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, TONE_GRAIN, TONE_FREQ,
                                   SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) { log_append("[bgapp] tone: open port FAILED\n"); return; }
    int vol[2] = { SCE_AUDIO_OUT_MAX_VOL, SCE_AUDIO_OUT_MAX_VOL };
    sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
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

/* ---- MP3 frame parsing --------------------------------------------------- */

/* MPEG1 Layer III bitrate table (kbps); index 0 and 15 are invalid. */
static const int mp3_srate_tab[4][3] = {
    { 11025, 12000, 8000  },   /* MPEG 2.5 (version 0) */
    { 0, 0, 0 },               /* reserved (1)         */
    { 22050, 24000, 16000 },   /* MPEG 2   (version 2) */
    { 44100, 48000, 32000 },   /* MPEG 1   (version 3) */
};

/* A valid Layer-III frame header: sync, non-reserved version, layer III,
   valid bitrate index, valid sample-rate index. */
static int mp3_is_sync(const unsigned char *p)
{
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return 0;
    int ver   = (p[1] >> 3) & 3;
    int layer = (p[1] >> 1) & 3;
    int brate = (p[2] >> 4) & 0xF;
    int srate = (p[2] >> 2) & 3;
    if (ver == 1) return 0;      /* reserved MPEG version */
    if (layer != 1) return 0;    /* 1 == Layer III        */
    if (brate == 0 || brate == 0xF) return 0;
    if (srate == 3) return 0;
    return 1;
}

static int mp3_srate(const unsigned char *p)
{
    int ver = (p[1] >> 3) & 3, si = (p[2] >> 2) & 3;
    return mp3_srate_tab[ver][si];
}

static int mp3_version(const unsigned char *p) { return (p[1] >> 3) & 3; }         /* 3=MPEG1 */
static int mp3_samples_per_frame(const unsigned char *p) { return ((p[1] >> 3) & 3) == 3 ? 1152 : 576; }

/* ---- SceAudiodec MP3 playback ------------------------------------------- */

static unsigned char g_inbuf[INBUF_SZ];
static int16_t       g_pcm[1152 * 2];   /* one MPEG1 frame, stereo 16-bit */

/* Decodes and plays `path` until the watchdog clears g_should_play or EOF.
   Returns 1 if a decoder + audio port were set up (so the caller must NOT also
   play the tone); 0 if setup failed early (caller falls back to the tone). */
static int play_file_session(const char *path)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) { log_append("[bgapp] mp3: file open failed\n"); return 0; }

    int in_pos = 0, in_len = sceIoRead(fd, g_inbuf, INBUF_SZ), eof = 0;
    if (in_len < 4) { sceIoClose(fd); return 0; }

    /* Skip an ID3v2 tag if present (syncsafe 28-bit size). */
    if (g_inbuf[0] == 'I' && g_inbuf[1] == 'D' && g_inbuf[2] == '3')
    {
        int tag = ((g_inbuf[6] & 0x7F) << 21) | ((g_inbuf[7] & 0x7F) << 14) |
                  ((g_inbuf[8] & 0x7F) << 7)  |  (g_inbuf[9] & 0x7F);
        sceIoLseek(fd, 10 + tag, SCE_SEEK_SET);
        in_len = sceIoRead(fd, g_inbuf, INBUF_SZ);
        in_pos = 0;
        if (in_len < 4) { sceIoClose(fd); return 0; }
    }

    /* Find the first frame sync. */
    while (in_pos < in_len - 1 && !mp3_is_sync(g_inbuf + in_pos)) in_pos++;
    if (in_pos >= in_len - 1) { log_append("[bgapp] mp3: no frame sync\n"); sceIoClose(fd); return 0; }

    int srate = mp3_srate(g_inbuf + in_pos);
    int spf   = mp3_samples_per_frame(g_inbuf + in_pos);
    if (srate <= 0) { sceIoClose(fd); return 0; }

    SceAudiodecInitParam ip;
    memset(&ip, 0, sizeof ip);
    ip.mp3.size = sizeof(ip.mp3);
    ip.mp3.totalStreams = 1;
    int r = sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_MP3, &ip);
    if (r < 0 && (unsigned) r != SCE_AUDIODEC_ERROR_ALREADY_INITIALIZED)
    {
        char b[80]; snprintf(b, sizeof b, "[bgapp] mp3: InitLibrary failed %#x\n", (unsigned) r);
        log_append(b); sceIoClose(fd); return 0;
    }

    SceAudiodecInfo info;
    memset(&info, 0, sizeof info);
    info.mp3.size = sizeof(info.mp3);
    info.mp3.ch = 2;
    info.mp3.version = mp3_version(g_inbuf + in_pos);

    SceAudiodecCtrl ctrl;
    memset(&ctrl, 0, sizeof ctrl);
    ctrl.size = sizeof(ctrl);
    ctrl.wordLength = SCE_AUDIODEC_WORD_LENGTH_16BITS;
    ctrl.pInfo = &info;
    ctrl.maxEsSize = SCE_AUDIODEC_MP3_MAX_ES_SIZE;
    ctrl.maxPcmSize = sizeof(g_pcm);

    r = sceAudiodecCreateDecoder(&ctrl, SCE_AUDIODEC_TYPE_MP3);
    if (r < 0)
    {
        char b[80]; snprintf(b, sizeof b, "[bgapp] mp3: CreateDecoder failed %#x\n", (unsigned) r);
        log_append(b);
        sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_MP3);
        sceIoClose(fd); return 0;
    }

    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, spf, srate, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0)
        port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, spf, srate, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0)
    {
        log_append("[bgapp] mp3: audio port open FAILED\n");
        sceAudiodecDeleteDecoder(&ctrl);
        sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_MP3);
        sceIoClose(fd); return 0;
    }
    int vol[2] = { SCE_AUDIO_OUT_MAX_VOL, SCE_AUDIO_OUT_MAX_VOL };
    sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);

    {
        char b[128];
        snprintf(b, sizeof b, "[bgapp] mp3: decoding (srate=%d spf=%d) %s\n", srate, spf, path);
        log_append(b);
    }

    int frames = 0, errs = 0;
    while (g_should_play)
    {
        if (in_len - in_pos < SCE_AUDIODEC_MP3_MAX_ES_SIZE && !eof)
        {
            int rem = in_len - in_pos;
            if (rem > 0 && in_pos > 0) memmove(g_inbuf, g_inbuf + in_pos, rem);
            in_pos = 0; in_len = rem;
            int got = sceIoRead(fd, g_inbuf + in_len, INBUF_SZ - in_len);
            if (got > 0) in_len += got; else eof = 1;
        }

        int avail = in_len - in_pos;
        if (avail < 4) break;   /* EOF */

        if (!mp3_is_sync(g_inbuf + in_pos))
        {
            int k = in_pos + 1;
            while (k < in_len - 1 && !mp3_is_sync(g_inbuf + k)) k++;
            if (k >= in_len - 1) { in_pos = in_len; continue; }
            in_pos = k; avail = in_len - in_pos;
        }

        memset(g_pcm, 0, sizeof g_pcm);
        ctrl.pEs = g_inbuf + in_pos;
        ctrl.inputEsSize = 0;
        ctrl.maxEsSize = avail < SCE_AUDIODEC_MP3_MAX_ES_SIZE ? avail : SCE_AUDIODEC_MP3_MAX_ES_SIZE;
        ctrl.pPcm = g_pcm;
        ctrl.outputPcmSize = 0;

        r = sceAudiodecDecode(&ctrl);
        if (r < 0)
        {
            in_pos += 1;          /* resync past the bad byte */
            if (++errs > 2048) { log_append("[bgapp] mp3: too many decode errors\n"); break; }
            continue;
        }
        errs = 0;
        in_pos += ctrl.inputEsSize > 0 ? (int) ctrl.inputEsSize : 1;
        if (ctrl.outputPcmSize > 0) sceAudioOutOutput(port, g_pcm);

        if (((++frames) % 1000) == 0)
        {
            char b[64]; snprintf(b, sizeof b, "[bgapp] mp3: %d frames\n", frames);
            log_append(b);
        }
    }

    if (in_len - in_pos < 4 && eof) log_append("[bgapp] mp3: track EOF\n");

    sceAudioOutReleasePort(port);
    sceAudiodecDeleteDecoder(&ctrl);
    sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_MP3);
    sceIoClose(fd);

    /* If EOF was reached while still backgrounded, stay quiet until foreground
       instead of restarting the song. */
    while (g_should_play) sceKernelDelayThread(100000);
    return 1;
}

/* ---- status / tick files ------------------------------------------------- */

/* Reads bgm_status. Writes *playing (may be NULL) and copies the line-3 file
   path into `path` (capacity `cap`, may be NULL). */
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

    /* line 1: playing flag */
    if (playing) *playing = (b[0] == '1');

    /* line 3: file path */
    if (path && cap)
    {
        int line = 0;
        char *s = b;
        while (*s && line < 2) { if (*s == '\n') line++; s++; }   /* skip to line 3 */
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
    log_append("[bgapp] ===== started (phase-1b downloaded-track playback) =====\n");

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
