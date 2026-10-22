/*
 * Import stubs for two SceAppMgr calls this SDK's headers leave out, under
 * names of their own, so that a later SDK declaring them cannot clash:
 *
 *   vitaplexGetCurrentBgmState(SceUID* process, unsigned* state)
 *       _sceAppMgrGetCurrentBgmState, 0x81569A58: which process the system
 *       has holding the BGM port, and in what state, as vita-headers'
 *       psp2/appmgr.h documents it.
 *   vitaplexAcquireBgmPortForMusicPlayer(int)
 *       sceAppMgrAcquireBgmPortForMusicPlayer, 0x44EC36FA: the port taken as
 *       a music player takes it. Its arguments are not written down anywhere
 *       found; callers pass a single 0, which a call taking none ignores.
 *
 * Library SceAppMgr is 0x8AF17416 in vita-headers' db for firmware 3.60
 * (db/360/SceAppMgr.yml). The layout of lib/libshellaudio/shellsvc_stub.S.
 */

	.arm
	.section .vitalink.fstubs.SceAppMgr,"ax",%progbits
	.align 4
	.global vitaplexGetCurrentBgmState
	.type vitaplexGetCurrentBgmState, %function
vitaplexGetCurrentBgmState:
	.word 0x00000000
	.word 0x8AF17416
	.word 0x81569A58
	.inst 0xE320F000
	.size vitaplexGetCurrentBgmState, 16

	.align 4
	.global vitaplexAcquireBgmPortForMusicPlayer
	.type vitaplexAcquireBgmPortForMusicPlayer, %function
vitaplexAcquireBgmPortForMusicPlayer:
	.word 0x00000000
	.word 0x8AF17416
	.word 0x44EC36FA
	.inst 0xE320F000
	.size vitaplexAcquireBgmPortForMusicPlayer, 16

	.section .note.GNU-stack,"",%progbits
