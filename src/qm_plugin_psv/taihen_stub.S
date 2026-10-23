/*
 * The plugin's taiHEN imports, under names of their own, with the library
 * and function NIDs libtaihen_stub.a has in vitasdk's taihen package:
 *
 *   vitaplexQmHookFunctionImport(tai_hook_ref_t* hook, tai_hook_args_t* args)
 *       taiHookFunctionImportForUser, taihen 0x203D1A28, function 0x53636E5D
 *   vitaplexQmGetModuleExportFunc(const char* module, uint32_t libnid,
 *                                 uint32_t funcnid, uintptr_t* func)
 *       taiGetModuleExportFunc, taihen 0x203D1A28, function 0x6231AAD1
 *
 * The layout of src/handover_psv/qm_link_stub.S.
 */

	.arm
	.section .vitalink.fstubs.taihen,"ax",%progbits
	.align 4
	.global vitaplexQmHookFunctionImport
	.type vitaplexQmHookFunctionImport, %function
vitaplexQmHookFunctionImport:
	.word 0x00000000
	.word 0x203D1A28
	.word 0x53636E5D
	.inst 0xE320F000
	.size vitaplexQmHookFunctionImport, 16

	.align 4
	.global vitaplexQmGetModuleExportFunc
	.type vitaplexQmGetModuleExportFunc, %function
vitaplexQmGetModuleExportFunc:
	.word 0x00000000
	.word 0x203D1A28
	.word 0x6231AAD1
	.inst 0xE320F000
	.size vitaplexQmGetModuleExportFunc, 16

	.section .note.GNU-stack,"",%progbits
