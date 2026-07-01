/*
 * VitaPlex PS Vita background helper — Phase-1c: SceShell music service.
 *
 * Phase 0 proved this eboot2.bin keeps running after VitaPlex is backgrounded.
 * Phases 1a/1b tried to play audio ourselves via sceAudioOut — but the system
 * MUTES a background app's sceAudioOut (LiveArea holds audio focus and plays the
 * theme music), so nothing we decoded was audible.
 *
 * This plays the song the way the Vita's own Music app does: hand the local file
 * to the SceShell music service (GrapheneCt's reverse-engineered libShellAudio,
 * vendored in shellaudio.c). That service owns background audio, decodes
 * MP3/AAC/AT9/WAV itself, and shows the LiveArea music widget. Scope stays
 * "downloaded tracks only": the main app hands us a local file path via
 * bgm_status only for completed downloads.
 *
 * IPC with the main app (separate process) via ux0:data/VitaPlex:
 *   bgm_tick   : counter bumped ~4x/sec while VitaPlex's main loop runs; FREEZES
 *                when the app is suspended (how we detect backgrounding).
 *   bgm_status : line 1 '1'/'0' playing, line 2 title, line 3 local file path
 *                (downloaded track, else empty), line 4 position ms (unused yet).
 *
 * On takeover (playing AND tick stalled) we start the shell playing line-3's
 * file; on release (foregrounded) we stop it so the app's own mpv resumes.
 * Resident while(1)+PowerTick follows GrapheneCt's BGFTP pattern.
 */

#include <psp2/types.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shellaudio.h"

extern int sceNotificationUtilBgAppInitialize(void);

#define DIR_DATA     "ux0:data"
#define DIR_APP      "ux0:data/VitaPlex"
#define LOG_PATH     "ux0:data/VitaPlex/bgapp.log"
#define TICK_PATH    "ux0:data/VitaPlex/bgm_tick"
#define STATUS_PATH  "ux0:data/VitaPlex/bgm_status"

#define POLL_US        200000   /* 200 ms watchdog poll */
#define STALL_TAKEOVER 6        /* ~1.2 s of frozen tick => app backgrounded */

static void log_append(const char *s)
{
    SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) { sceIoWrite(fd, s, strlen(s)); sceIoClose(fd); }
}

/* Reads bgm_status: *playing (may be NULL) + the line-3 file path (may be NULL). */
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

int main(void)
{
    sceIoMkdir(DIR_DATA, 0777);
    sceIoMkdir(DIR_APP, 0777);

    sceSysmoduleLoadModule(SCE_SYSMODULE_NOTIFICATION_UTIL);
    sceNotificationUtilBgAppInitialize();

    log_append("[bgapp] ===== started (phase-1c: SceShell music service) =====\n");

    /* opt.flag = -1 is required — matches the known-good BetterHomebrewBrowser /
       libShellAudio BGM recipe; with flag 0 the shell accepted the file but
       played nothing. */
    SceMusicOpt opt;
    memset(&opt, 0, sizeof opt);
    opt.flag = -1;

    int last_tick = -1, stall = 0, tick = 0, active = 0, hb = 0, warned_nofile = 0;
    char path[300];

    for (;;)
    {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);

        int have_tick = read_tick(&tick);
        int playing = 0;
        path[0] = 0;
        read_status(&playing, path, sizeof path);

        if (!have_tick) { stall = 0; last_tick = -1; }
        else if (tick != last_tick) { last_tick = tick; stall = 0; }
        else if (stall < 1000000) stall++;

        int backgrounded = have_tick && (stall >= STALL_TAKEOVER);
        int want = (playing && backgrounded) ? 1 : 0;

        if (want && !active)
        {
            if (path[0])
            {
                /* Exact known-good sequence from BetterHomebrewBrowser:
                   Initialize(0) [app, not plugin] -> SetUri(flag=-1) ->
                   SetVolume(0dB) -> SetRepeatMode(ONE) -> SendCommand(DEFAULT).
                   Requires the ENABLE_BGM_PROXY (0x40) attribute in param2.sfo. */
                int r0 = sceMusicInternalAppInitialize(0);
                int r1 = sceMusicInternalAppSetUri(path, &opt);
                sceMusicInternalAppSetVolume(0x8000);                 /* SCE_AUDIO_VOLUME_0DB */
                sceMusicInternalAppSetRepeatMode(SCE_MUSIC_REPEAT_ONE);
                int r2 = sceMusicInternalAppSetPlaybackCommand(SCE_MUSIC_EVENTID_DEFAULT, 0);

                /* Diagnostics: what does the shell think the playback state is? */
                char st[0x40];
                memset(st, 0, sizeof st);
                int rs = sceMusicInternalAppGetPlaybackStatus(st);
                const int *si = (const int *) st;
                SceMusicInternalAppResult lr;
                memset(&lr, 0, sizeof lr);
                int rl = sceMusicInternalAppGetLastResult(&lr);

                char b[512];
                snprintf(b, sizeof b,
                         "[bgapp] shell: init=%#x uri=%#x cmd=%#x  status(%#x)=[%d %d %d %d]  last(%#x)=[%d %d]  %s\n",
                         (unsigned) r0, (unsigned) r1, (unsigned) r2,
                         (unsigned) rs, si[0], si[1], si[2], si[3],
                         (unsigned) rl, lr.state, lr.time, path);
                log_append(b);
                active = 1;
                warned_nofile = 0;
            }
            else if (!warned_nofile)
            {
                log_append("[bgapp] shell: no local file (streamed track not in scope)\n");
                warned_nofile = 1;
            }
        }
        else if (!want && active)
        {
            sceMusicInternalAppSetPlaybackCommand(SCE_MUSIC_EVENTID_STOP, 0);
            sceMusicInternalAppTerminate();
            active = 0;
            log_append("[bgapp] shell: stopped + terminated (released)\n");
        }

        if (++hb >= 50)
        {
            hb = 0;
            if (active)
            {
                char st[0x40];
                memset(st, 0, sizeof st);
                int rs = sceMusicInternalAppGetPlaybackStatus(st);
                const int *si = (const int *) st;
                char b[200];
                snprintf(b, sizeof b, "[bgapp] hb: tick=%d stall=%d active=1  status(%#x)=[%d %d %d %d]\n",
                         tick, stall, (unsigned) rs, si[0], si[1], si[2], si[3]);
                log_append(b);
            }
            else
            {
                char b[160];
                snprintf(b, sizeof b, "[bgapp] hb: tick=%d stall=%d playing=%d bg=%d active=0\n",
                         tick, stall, playing, backgrounded);
                log_append(b);
            }
        }

        sceKernelDelayThread(POLL_US);
    }
    return 0;
}
