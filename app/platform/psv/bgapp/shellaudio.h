/*
 * libShellAudio — Application BGM subset, adapted for vitasdk.
 *
 * Source: GrapheneCt's libShellAudio (reverse-engineered from Sony's static
 * library used by the Music application), trimmed to the "Application BGM" API
 * that plays a local audio file through the SceShell music service. Unlike
 * sceAudioOut, this path plays with real background-audio focus (audible in
 * LiveArea) and the shell decodes MP3/AAC/AT9/WAV itself.
 *
 * Original: https://github.com/GrapheneCt/libShellAudio  (MIT)
 */

#ifndef VITAPLEX_SHELLAUDIO_H
#define VITAPLEX_SHELLAUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SceMusicOpt {
	int flag;      /* set to 0 if not used */
	int param1;
	int param2;
	int param3;
} SceMusicOpt;

typedef struct SceMusicInternalAppResult {
	int state;
	int time;
} SceMusicInternalAppResult;

typedef struct SceShellAudioBGMState {
	int bgmPortOwnerId;
	int bgmPortStatus;
	int someStatus1;
	int currentState;
	int someStatus2;
} SceShellAudioBGMState;

typedef enum SceMusicEventId {
	SCE_MUSIC_EVENTID_DEFAULT  = 0,
	SCE_MUSIC_EVENTID_PLAY     = 0x1,
	SCE_MUSIC_EVENTID_STOP     = 0x2,
	SCE_MUSIC_EVENTID_NEXT     = 0x3,
	SCE_MUSIC_EVENTID_PREVIOUS = 0x4,
	SCE_MUSIC_EVENTID_SEEK     = 0x11
} SceMusicEventId;

typedef enum SceMusicRepeatMode {
	SCE_MUSIC_REPEAT_ALL,
	SCE_MUSIC_REPEAT_ONE,
	SCE_MUSIC_REPEAT_DISABLE
} SceMusicRepeatMode;

typedef enum SceMusicErrorCodes {
	SCE_MUSIC_ERROR_INVALID_ARG         = 0x80101900,
	SCE_MUSIC_ERROR_NOT_INITIALIZED     = 0x80101901,
	SCE_MUSIC_ERROR_ALREADY_INITIALIZED = 0x80101902,
	SCE_MUSIC_ERROR_NOT_READY           = 0x80101903,
	SCE_MUSIC_ERROR_INVALID_SERVICE_ARG = 0x80100E00
} SceMusicErrorCodes;

/* Application BGM — init_type 0 = BGM proxy, 1 = SceShell. */
int sceMusicInternalAppInitialize(int init_type);
int sceMusicInternalAppTerminate(void);
int sceMusicInternalAppSetUri(char *path, SceMusicOpt *optParams);
int sceMusicInternalAppSetVolume(unsigned int volume);
int sceMusicInternalAppSetRepeatMode(int mode);
int sceMusicInternalAppSetPlaybackCommand(int eventId, int param_2);   /* eventId = SceMusicEventId */
int sceMusicInternalAppGetLastResult(SceMusicInternalAppResult *resBuffer);
int sceMusicInternalAppGetPlaybackStatus(void *resBuffer);

#ifdef __cplusplus
}
#endif

#endif /* VITAPLEX_SHELLAUDIO_H */
