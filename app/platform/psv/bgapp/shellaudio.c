/*
 * libShellAudio — Application BGM subset, adapted for vitasdk.
 *
 * Verbatim logic from GrapheneCt's libShellAudio (ShellAudio.c), trimmed to the
 * Application-BGM functions VitaPlex's background helper uses to play a local
 * file through the SceShell music service. Only the includes and the appmgr
 * symbol name were adapted for vitasdk:
 *   - <kernel.h>/<shellsvc.h> -> psp2 headers + an extern for sceShellSvcGetSvcObj
 *     (resolved by shellsvc_stub.S, since vitasdk lacks that NID).
 *   - sceAppMgrSetBgmProxyApp -> thin wrapper over vitasdk's _sceAppMgrSetBgmProxyApp.
 *
 * Original: https://github.com/GrapheneCt/libShellAudio  (MIT)
 */

#include <stdint.h>
#include <stddef.h>
#include <psp2/types.h>
#include <string.h>

#include "shellaudio.h"

/* Not in vitasdk's db — provided by shellsvc_stub.S. */
extern void *sceShellSvcGetSvcObj(void);

/* vitasdk exports this AppMgr BGM-proxy call with a leading underscore. */
extern int _sceAppMgrSetBgmProxyApp(const char *);
static int sceAppMgrSetBgmProxyApp(const char *a) { return _sceAppMgrSetBgmProxyApp(a); }

typedef struct SceShellSvcCustomAudioSubParams1 {
	int unk_00;      /* not used (set to 0) */
	int tracking1;
	int tracking2;
} SceShellSvcCustomAudioSubParams1;

typedef struct SceShellSvcCustomAudioSubParams2 {
	int flag;
	int unk_04;
	int unk_08;
	int unk_0C;
} SceShellSvcCustomAudioSubParams2;

typedef struct SceShellSvcCustomAudioSubParams3 {
	int param1;
	int param2;
} SceShellSvcCustomAudioSubParams3;

typedef struct SceShellSvcAudioCustomParams {
	void *params1;
	SceSize params1Size;
	void *params2;
	SceSize params2Size;
	void *params3;
	SceSize params3Size;
	void *params4;
	SceSize params4Size;
} SceShellSvcAudioCustomParams;

typedef struct SceShellSvcTable {
	void *pFunc_0x00;
	void *pFunc_0x04;
	void *pFunc_0x08;
	void *pFunc_0x0C;
	void *pFunc_0x10;
	int (*sceShellSvcAudioControl)(void *obj, int flag, SceShellSvcAudioCustomParams *, int numOfArg, int *pRes, SceShellSvcAudioCustomParams *, int numOfOut);
	void *pFunc_0x18;
	int (*sceShellSvcAsyncMethod)(void *obj, int asyncMethodId);
} SceShellSvcTable;

static int isInitialized = 0;
static int isReady = 0;
static int global_1 = 0;
static int global_2 = 0;

int shellAudioFinishInternal(int eventId)
{
	void *tptr = sceShellSvcGetSvcObj();

	SceShellSvcCustomAudioSubParams1 params1;
	params1.unk_00 = 0;
	params1.tracking1 = global_1;
	params1.tracking2 = global_2;

	SceShellSvcAudioCustomParams mainParams;
	mainParams.params1 = &params1;
	mainParams.params1Size = 0xC;

	int res = 0;
	int ret = ((SceShellSvcTable *)(*(uint32_t *)tptr))->sceShellSvcAudioControl(tptr, eventId, &mainParams, 1, &res, 0, 0);
	if (ret != 0)
		res = ret;
	return res;
}

int shellAudioSendCommandInternal(int eventId, int commandId, int param_2)
{
	void *tptr = sceShellSvcGetSvcObj();

	SceShellSvcCustomAudioSubParams1 params1;
	params1.unk_00 = 0;
	params1.tracking1 = global_1;
	params1.tracking2 = global_2;

	SceShellSvcCustomAudioSubParams3 params2;
	params2.param1 = commandId;
	params2.param2 = param_2;

	SceShellSvcAudioCustomParams mainParams;
	mainParams.params1 = &params2;
	mainParams.params1Size = 0x8;
	mainParams.params2 = &params1;
	mainParams.params2Size = 0xC;

	int res = 0;
	int ret = ((SceShellSvcTable *)(*(uint32_t *)tptr))->sceShellSvcAudioControl(tptr, eventId, &mainParams, 2, &res, 0, 0);
	if (ret != 0)
		res = ret;
	return res;
}

int shellAudioSetParam2Internal(int eventId, int8_t param)
{
	void *tptr = sceShellSvcGetSvcObj();

	SceShellSvcCustomAudioSubParams1 params1;
	params1.unk_00 = 0;
	params1.tracking1 = global_1;
	params1.tracking2 = global_2;

	SceShellSvcAudioCustomParams mainParams;
	mainParams.params1 = &param;
	mainParams.params1Size = 0x1;
	mainParams.params2 = &params1;
	mainParams.params2Size = 0xC;

	int res = 0;
	int ret = ((SceShellSvcTable *)(*(uint32_t *)tptr))->sceShellSvcAudioControl(tptr, eventId, &mainParams, 2, &res, 0, 0);
	if (ret != 0)
		res = ret;
	return res;
}

int sceMusicInternalAppInitialize(int init_type)
{
	int ret;

	if (isInitialized) {
		ret = SCE_MUSIC_ERROR_ALREADY_INITIALIZED;
		goto end;
	}

	if (init_type != 0 && init_type != 1) {
		ret = SCE_MUSIC_ERROR_INVALID_ARG;
		goto end;
	}

	if (init_type == 0)
		sceAppMgrSetBgmProxyApp("NPXS19999");

	void *tptr = sceShellSvcGetSvcObj();

	SceShellSvcCustomAudioSubParams1 params1;
	params1.unk_00 = 0;
	params1.tracking1 = init_type;
	params1.tracking2 = 0;

	SceShellSvcAudioCustomParams mainParams;
	mainParams.params1 = &params1;
	mainParams.params1Size = 0xC;

	int res = 0;
	ret = ((SceShellSvcTable *)(*(uint32_t *)tptr))->sceShellSvcAudioControl(tptr, 0xD0000, &mainParams, 1, &res, 0, 0);
	if (ret != 0)
		goto end;

	global_1 = params1.tracking1;
	global_2 = params1.tracking2;
	isInitialized = 1;
	isReady = 0;
	return res;

end:
	return ret;
}

int sceMusicInternalAppTerminate(void)
{
	int ret;

	if (!isInitialized) {
		ret = SCE_MUSIC_ERROR_NOT_INITIALIZED;
		goto end;
	}

	ret = shellAudioFinishInternal(0xD0001);
	if (ret != 0)
		goto end;

	global_1 = 0;
	global_2 = 0;
	isInitialized = 0;
	isReady = 0;

end:
	return ret;
}

int sceMusicInternalAppSetUri(char *path, SceMusicOpt *optParams)
{
	int ret, pathlen;

	if (!isInitialized) {
		ret = SCE_MUSIC_ERROR_NOT_INITIALIZED;
		goto end;
	}
	if (path == NULL) {
		ret = SCE_MUSIC_ERROR_INVALID_ARG;
		goto end;
	}
	if (optParams->param1 != 0 && optParams->param1 != 1) {
		ret = SCE_MUSIC_ERROR_INVALID_ARG;
		goto end;
	}

	pathlen = strnlen(path, 0x401);

	void *tptr = sceShellSvcGetSvcObj();

	SceShellSvcCustomAudioSubParams1 params1;
	params1.unk_00 = 0;
	params1.tracking1 = global_1;
	params1.tracking2 = global_2;

	SceShellSvcCustomAudioSubParams2 params2;
	memset(&params2, 0, 0x10);
	if (optParams->flag != 0)
		memcpy(&params2, optParams, 0x10);

	SceShellSvcAudioCustomParams mainParams;
	mainParams.params1 = &params1;
	mainParams.params1Size = 0xC;
	mainParams.params2 = path;
	mainParams.params2Size = pathlen;
	mainParams.params3 = &params2;
	mainParams.params3Size = 0x10;

	int res = 0;
	ret = ((SceShellSvcTable *)(*(uint32_t *)tptr))->sceShellSvcAudioControl(tptr, 0xD0002, &mainParams, 3, &res, 0, 0);
	if (ret != 0)
		goto end;

	isReady = 1;
	return res;

end:
	return ret;
}

int sceMusicInternalAppSetVolume(unsigned int volume)
{
	int ret;

	if (!isInitialized) {
		ret = SCE_MUSIC_ERROR_NOT_INITIALIZED;
		goto end;
	}
	if (volume > 0x8000) {
		ret = SCE_MUSIC_ERROR_INVALID_ARG;
		goto end;
	}

	void *tptr = sceShellSvcGetSvcObj();

	SceShellSvcCustomAudioSubParams1 params1;
	params1.unk_00 = 0;
	params1.tracking1 = global_1;
	params1.tracking2 = global_2;

	SceShellSvcAudioCustomParams mainParams;
	mainParams.params1 = &volume;
	mainParams.params1Size = 0x4;
	mainParams.params2 = &params1;
	mainParams.params2Size = 0xC;

	int res = 0;
	ret = ((SceShellSvcTable *)(*(uint32_t *)tptr))->sceShellSvcAudioControl(tptr, 0xD0006, &mainParams, 2, &res, 0, 0);
	if (ret != 0)
		goto end;

	return res;

end:
	return ret;
}

int sceMusicInternalAppSetRepeatMode(int mode)
{
	int ret;

	if (!isInitialized) {
		ret = SCE_MUSIC_ERROR_NOT_INITIALIZED;
		goto end;
	}
	if (mode != 0 && mode != 1 && mode != 2) {
		ret = SCE_MUSIC_ERROR_INVALID_ARG;
		goto end;
	}

	ret = shellAudioSetParam2Internal(0xD0007, mode);

end:
	return ret;
}

int sceMusicInternalAppSetPlaybackCommand(int eventId, int param_2)
{
	int ret;

	if (!isInitialized) {
		ret = SCE_MUSIC_ERROR_NOT_INITIALIZED;
		goto end;
	}
	if (!isReady) {
		ret = SCE_MUSIC_ERROR_NOT_READY;
		goto end;
	}
	if (eventId > 0x11 || eventId < 0) {
		ret = SCE_MUSIC_ERROR_INVALID_ARG;
		goto end;
	}

	ret = shellAudioSendCommandInternal(0xD0004, eventId, param_2);

end:
	return ret;
}

int sceMusicInternalAppGetLastResult(SceMusicInternalAppResult *resBuffer)
{
	int ret;

	if (!isInitialized) {
		ret = SCE_MUSIC_ERROR_NOT_INITIALIZED;
		goto end;
	}
	if (resBuffer == NULL) {
		ret = SCE_MUSIC_ERROR_INVALID_ARG;
		goto end;
	}

	void *tptr = sceShellSvcGetSvcObj();

	SceShellSvcCustomAudioSubParams1 params1;
	params1.unk_00 = 0;
	params1.tracking1 = global_1;
	params1.tracking2 = global_2;

	SceShellSvcAudioCustomParams mainParams;
	mainParams.params1 = &params1;
	mainParams.params1Size = 0xC;

	char buffer[0x8];
	memset(buffer, 0, sizeof(buffer));

	SceShellSvcAudioCustomParams outputInfo;
	outputInfo.params1 = buffer;
	outputInfo.params1Size = sizeof(buffer);

	int res = 0;
	ret = ((SceShellSvcTable *)(*(uint32_t *)tptr))->sceShellSvcAudioControl(tptr, 0xD000E, &mainParams, 1, &res, &outputInfo, 1);
	if (ret != 0)
		goto end;

	memcpy(resBuffer, buffer, sizeof(buffer));
	return res;

end:
	return ret;
}

int sceMusicInternalAppGetPlaybackStatus(void *resBuffer)
{
	int ret;

	if (!isInitialized) {
		ret = SCE_MUSIC_ERROR_NOT_INITIALIZED;
		goto end;
	}
	if (resBuffer == NULL) {
		ret = SCE_MUSIC_ERROR_INVALID_ARG;
		goto end;
	}

	void *tptr = sceShellSvcGetSvcObj();

	SceShellSvcCustomAudioSubParams1 params1;
	params1.unk_00 = 0;
	params1.tracking1 = global_1;
	params1.tracking2 = global_2;

	SceShellSvcAudioCustomParams mainParams;
	mainParams.params1 = &params1;
	mainParams.params1Size = 0xC;

	char buffer[0x40];
	memset(buffer, 0, sizeof(buffer));

	SceShellSvcAudioCustomParams outputInfo;
	outputInfo.params1 = buffer;
	outputInfo.params1Size = sizeof(buffer);

	int res = 0;
	ret = ((SceShellSvcTable *)(*(uint32_t *)tptr))->sceShellSvcAudioControl(tptr, 0xD0005, &mainParams, 1, &res, &outputInfo, 1);
	if (ret != 0)
		goto end;

	memcpy(resBuffer, buffer, sizeof(buffer));
	return res;

end:
	return ret;
}
