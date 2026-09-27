/*
 * Import stub for sceShellSvcGetSvcObj, which vitasdk's SceShellSvc stub
 * library does not carry (it has SceShellUtil and SceShellUtilLaunchApp only).
 *
 * The same layout vita-libs-gen emits and vita-elf-create reads back: the
 * import's library name is the section suffix, then three words of library
 * flags, library NID and function NID, then an ARM nop that the loader
 * replaces with the branch to the real function.
 *
 * Library SceShellSvc is 0x773242C7 in vs0:sys/external/libshellsvc.suprx on
 * firmware 3.60, the module that also exports SceShellUtil. The function NID
 * 0xB31E7F1C matches Vita3K's table for sceShellSvcGetSvcObj.
 */

	.arm
	.section .vitalink.fstubs.SceShellSvc,"ax",%progbits
	.align 4
	.global sceShellSvcGetSvcObj
	.type sceShellSvcGetSvcObj, %function
sceShellSvcGetSvcObj:
	.word 0x00000000
	.word 0x773242C7
	.word 0xB31E7F1C
	.inst 0xE320F000
	.size sceShellSvcGetSvcObj, 16

	.section .note.GNU-stack,"",%progbits
