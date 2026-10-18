/*
 * Import stub for _sceKernelExitProcessForUser, the system call that
 * sceKernelExitProcess (SceLibKernel) ends with, under a name of its own:
 * bgtestRawExit(int status). main.c's rescue calls it when that function has
 * not ended the process 3 s after it was called, as VPLXBGT18's did not.
 *
 * The layout of lib/libshellaudio/shellsvc_stub.S. Library SceProcessmgr is
 * 0x2DD91812 and the function 0xC053DC6B in vita-headers' db for firmware
 * 3.60 (db/360/SceProcessmgr.yml); the name is this project's, so that it
 * cannot clash with a declaration in the SDK's headers.
 */

	.arm
	.section .vitalink.fstubs.SceProcessmgr,"ax",%progbits
	.align 4
	.global bgtestRawExit
	.type bgtestRawExit, %function
bgtestRawExit:
	.word 0x00000000
	.word 0x2DD91812
	.word 0xC053DC6B
	.inst 0xE320F000
	.size bgtestRawExit, 16

	.section .note.GNU-stack,"",%progbits
