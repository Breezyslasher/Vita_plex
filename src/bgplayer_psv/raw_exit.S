/*
 * Import stub for _sceKernelExitProcessForUser, the system call that
 * sceKernelExitProcess (SceLibKernel) ends with, under a name of its own:
 * vitaplexRawExit(int status). On some ways out of an app that has started its
 * background program, and of that program, sceKernelExitProcess did not
 * return, and this did (docs/DESIGN_NOTES.md, BG Tests 18 to 21).
 *
 * The layout of lib/libshellaudio/shellsvc_stub.S, as src/bgtest_psv/exit_stub.S
 * has it. Library SceProcessmgr is 0x2DD91812 and the function 0xC053DC6B in
 * vita-headers' db for firmware 3.60 (db/360/SceProcessmgr.yml); the name is
 * this project's, so that it cannot clash with a declaration in the SDK's
 * headers.
 */

	.arm
	.section .vitalink.fstubs.SceProcessmgr,"ax",%progbits
	.align 4
	.global vitaplexRawExit
	.type vitaplexRawExit, %function
vitaplexRawExit:
	.word 0x00000000
	.word 0x2DD91812
	.word 0xC053DC6B
	.inst 0xE320F000
	.size vitaplexRawExit, 16

	.section .note.GNU-stack,"",%progbits
