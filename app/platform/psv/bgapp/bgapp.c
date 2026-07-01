/*
 * VitaPlex PS Vita background helper — Phase-1a audio proof.
 *
 * Builds on the proven Phase-0 foundation: this eboot2.bin keeps running after
 * VitaPlex is sent to LiveArea (verified on hardware — bgapp.log kept ticking
 * while the app was backgrounded). Phase 1a answers the next make-or-break
 * question for background music:
 *
 *     Can this background process produce AUDIBLE sound via sceAudioOut while
 *     VitaPlex is backgrounded, or does the OS mute non-foreground audio?
 *
 * If the tone is audible in LiveArea, real decoded audio can replace it next.
 * If it's muted, we learn cheaply that we need the SceShell music service.
 *
 * Mechanism — the real feature's skeleton, with a test tone instead of a track.
 * The main app and this helper are separate processes that can't share memory,
 * so VitaPlex hands us state through two tiny files under ux0:data/VitaPlex:
 *
 *   bgm_tick   : a counter bumped ~4x/sec by a RepeatingTimer while VitaPlex's
 *                main loop runs. It FREEZES the instant the app is suspended
 *                (LiveArea) — that frozen counter is how we detect backgrounding
 *                without the app being able to act at suspend time.
 *   bgm_status : first byte '1'/'0' = music playing flag, refreshed on every
 *                play/pause/track change.
 *
 * When music was playing AND the tick has stalled (app backgrounded), the audio
 * thread plays a 440 Hz tone. When the tick resumes (app foregrounded) it goes
 * silent so the app's own mpv output takes back over. Resident while(1)+PowerTick
 * follows GrapheneCt's BGFTP pattern.
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

/* In SceNotificationUtil's stub (which vitasdk ships); declared here so we
   don't depend on the exact header path — the stub resolves it at link time. */
extern int sceNotificationUtilBgAppInitialize(void);

#define DIR_DATA     "ux0:data"
#define DIR_APP      "ux0:data/VitaPlex"
#define LOG_PATH     "ux0:data/VitaPlex/bgapp.log"
#define TICK_PATH    "ux0:data/VitaPlex/bgm_tick"
#define STATUS_PATH  "ux0:data/VitaPlex/bgm_status"

#define AUDIO_FREQ    48000
#define AUDIO_GRAIN   1024      /* samples per sceAudioOutOutput call */
#define TONE_HZ       440
#define TONE_AMP      6000      /* of 32767 — moderate, clearly audible */

#define POLL_US       200000    /* 200 ms watchdog poll */
#define STALL_TAKEOVER 6        /* ~1.2 s of frozen tick => app backgrounded */

static void log_append(const char *s)
{
    SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0)
    {
        sceIoWrite(fd, s, strlen(s));
        sceIoClose(fd);
    }
}

/* --- shared control between the watchdog (main) and the audio thread --- */
static volatile int g_should_play = 0;

/* One cycle of the tone, precomputed once so the audio loop needs no libm. */
static int16_t g_sine[AUDIO_FREQ / TONE_HZ + 2];
static int g_sine_len = 0;

static void build_sine(void)
{
    g_sine_len = AUDIO_FREQ / TONE_HZ;   /* ~109 samples for one 440 Hz cycle */
    for (int i = 0; i < g_sine_len; i++)
    {
        float t = (float) i / (float) g_sine_len;   /* 0..1 over one cycle */
        g_sine[i] = (int16_t)(sinf(t * 2.0f * 3.14159265f) * TONE_AMP);
    }
}

static int audio_thread(SceSize argc, void *argv)
{
    (void) argc; (void) argv;

    int port = -1;
    int16_t buf[AUDIO_GRAIN * 2];   /* interleaved stereo */
    int idx = 0;

    for (;;)
    {
        if (g_should_play)
        {
            if (port < 0)
            {
                int type = SCE_AUDIO_OUT_PORT_TYPE_BGM;
                port = sceAudioOutOpenPort(type, AUDIO_GRAIN, AUDIO_FREQ,
                                           SCE_AUDIO_OUT_MODE_STEREO);
                if (port < 0)
                {
                    /* BGM port unavailable — fall back to the main port. */
                    type = SCE_AUDIO_OUT_PORT_TYPE_MAIN;
                    port = sceAudioOutOpenPort(type, AUDIO_GRAIN, AUDIO_FREQ,
                                               SCE_AUDIO_OUT_MODE_STEREO);
                }
                if (port < 0)
                {
                    char b[96];
                    snprintf(b, sizeof b, "[bgapp] audio: open port FAILED (%#x)\n",
                             (unsigned) port);
                    log_append(b);
                    sceKernelDelayThread(500000);
                    continue;
                }
                int vol[2] = { SCE_AUDIO_OUT_MAX_VOL, SCE_AUDIO_OUT_MAX_VOL };
                sceAudioOutSetVolume(port,
                    SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
                char b[96];
                snprintf(b, sizeof b, "[bgapp] audio: port open ok (type=%d)\n", type);
                log_append(b);
            }

            for (int i = 0; i < AUDIO_GRAIN; i++)
            {
                int16_t s = g_sine[idx];
                if (++idx >= g_sine_len) idx = 0;
                buf[2 * i]     = s;
                buf[2 * i + 1] = s;
            }
            sceAudioOutOutput(port, buf);   /* blocks ~grain/freq s (paces us) */
        }
        else
        {
            if (port >= 0)
            {
                sceAudioOutReleasePort(port);
                port = -1;
                log_append("[bgapp] audio: port released\n");
            }
            sceKernelDelayThread(50000);
        }
    }
    return 0;
}

/* Reads a small integer written as decimal text. Returns 1 on success. */
static int read_int_file(const char *path, int *out)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    char b[32];
    int n = sceIoRead(fd, b, sizeof(b) - 1);
    sceIoClose(fd);
    if (n <= 0) return 0;
    b[n] = 0;
    *out = (int) strtol(b, NULL, 10);
    return 1;
}

/* bgm_status first byte: '1' playing, '0'/absent paused/stopped. */
static int read_status_playing(void)
{
    SceUID fd = sceIoOpen(STATUS_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    char c = 0;
    int n = sceIoRead(fd, &c, 1);
    sceIoClose(fd);
    return (n > 0 && c == '1');
}

int main(void)
{
    sceIoMkdir(DIR_DATA, 0777);
    sceIoMkdir(DIR_APP, 0777);

    /* Register as a background app (matches BGFTP / BG-App-PSV). */
    sceSysmoduleLoadModule(SCE_SYSMODULE_NOTIFICATION_UTIL);
    sceNotificationUtilBgAppInitialize();

    build_sine();
    log_append("[bgapp] ===== started (phase-1a audio proof) =====\n");

    SceUID th = sceKernelCreateThread("vitaplex_bgm_audio", audio_thread,
                                      0x10000100, 0x10000, 0, 0, NULL);
    if (th >= 0)
        sceKernelStartThread(th, 0, NULL);
    else
        log_append("[bgapp] audio: thread create FAILED\n");

    int last_tick = -1, stall = 0, tick = 0;
    int want_tone = 0, hb = 0;

    for (;;)
    {
        /* Keep the system from auto-suspending us while we're the BG owner. */
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);

        int have_tick = read_int_file(TICK_PATH, &tick);
        int playing = read_status_playing();

        if (!have_tick)
        {
            /* App hasn't started writing ticks yet (no music this session). */
            stall = 0;
            last_tick = -1;
        }
        else if (tick != last_tick)
        {
            last_tick = tick;   /* app main loop alive => foreground */
            stall = 0;
        }
        else if (stall < 1000000)
        {
            stall++;            /* tick frozen => app may be backgrounded */
        }

        int backgrounded = have_tick && (stall >= STALL_TAKEOVER);
        int want = (playing && backgrounded) ? 1 : 0;

        if (want != want_tone)
        {
            want_tone = want;
            log_append(want ? "[bgapp] >>> taking over playback (tone ON)\n"
                            : "[bgapp] <<< releasing playback (tone OFF)\n");
        }
        g_should_play = want;

        if (++hb >= 50)   /* ~ every 10 s */
        {
            hb = 0;
            char b[160];
            snprintf(b, sizeof b,
                     "[bgapp] hb: tick=%d stall=%d playing=%d bg=%d tone=%d\n",
                     tick, stall, playing, backgrounded, want);
            log_append(b);
        }

        sceKernelDelayThread(POLL_US);
    }
    return 0;
}
