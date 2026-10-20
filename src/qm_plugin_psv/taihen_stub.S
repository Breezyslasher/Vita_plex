/*
 * The plugin's one taiHEN import, under a name of its own:
 *
 *   vitaplexQmHookFunctionImport(tai_hook_ref_t* hook, tai_hook_args_t* args)
 *       taiHookFunctionImportForUser, taihen 0x203D1A28, function 0x53636E5D,
 *       as libtaihen_stub.a has them in vitasdk's taihen package.
 *
 * The layout of src/bgplayer_psv/qm_link_stub.S.
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

	.section .note.GNU-stack,"",%progbits
