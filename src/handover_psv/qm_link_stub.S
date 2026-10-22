/*
 * Import stubs for qm_link.c, under names of its own so that a later SDK
 * declaring these cannot clash with them:
 *
 *   vitaplexOpenMsgPipe(const char* name)
 *       sceKernelOpenMsgPipe, SceThreadmgr 0x859A24B1, function 0x0E1CB9F6
 *   vitaplexCloseMsgPipe(SceUID uid)
 *       sceKernelCloseMsgPipe, SceThreadmgr 0x859A24B1, function 0x1305A065
 *       Both from vita-headers' db for firmware 3.60
 *       (db/360/SceKernelThreadMgr.yml); the SDK's headers declare neither.
 *   vitaplexTaiLoadStartModuleForPidForUser(const char* path, args)
 *       taiLoadStartModuleForPidForUser, taihenUnsafe 0xB2B67326, function
 *       0xE853ABCB, as libtaihen_stub.a has them in vitasdk's taihen package
 *       and in taiHEN v0.11's release alike. The library only unsafe
 *       homebrew may call, which VitaPlex is, as ElevenMPV-A is (it links
 *       taihenUnsafe_stub for the same call).
 *
 * Each stub as vitasdk's own stub libraries lay one out: a word for the
 * loader, the library's NID, the function's NID, and a nop.
 */

	.arm
	.section .vitalink.fstubs.SceThreadmgr,"ax",%progbits
	.align 4
	.global vitaplexOpenMsgPipe
	.type vitaplexOpenMsgPipe, %function
vitaplexOpenMsgPipe:
	.word 0x00000000
	.word 0x859A24B1
	.word 0x0E1CB9F6
	.inst 0xE320F000
	.size vitaplexOpenMsgPipe, 16

	.align 4
	.global vitaplexCloseMsgPipe
	.type vitaplexCloseMsgPipe, %function
vitaplexCloseMsgPipe:
	.word 0x00000000
	.word 0x859A24B1
	.word 0x1305A065
	.inst 0xE320F000
	.size vitaplexCloseMsgPipe, 16

	.section .vitalink.fstubs.taihenUnsafe,"ax",%progbits
	.align 4
	.global vitaplexTaiLoadStartModuleForPidForUser
	.type vitaplexTaiLoadStartModuleForPidForUser, %function
vitaplexTaiLoadStartModuleForPidForUser:
	.word 0x00000000
	.word 0xB2B67326
	.word 0xE853ABCB
	.inst 0xE320F000
	.size vitaplexTaiLoadStartModuleForPidForUser, 16

	.section .note.GNU-stack,"",%progbits
