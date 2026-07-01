@ VitaPlex — custom import stub for sceShellSvcGetSvcObj.
@
@ vitasdk's SceShellSvc stubs only cover the SceShellUtil* functions, not
@ sceShellSvcGetSvcObj, which libShellAudio needs to reach the SceShell music
@ service. This is the exact stub layout vita-libs-gen emits for an import
@ (see vitasdk/vita-toolchain src/vita-libs-gen/vita-libs-gen.c): a 3-word
@ record — { flags, library NID, function NID } — in a .vitalink.fstubs.<lib>
@ section that vita-elf-create turns into a module import. Non-weak (flags 0).
@
@   library SceShellSvc : 0x773242C7
@   function sceShellSvcGetSvcObj : 0xB31E7F1C

.arch armv7a

.section .vitalink.fstubs.SceShellSvc,"ax",%progbits

	.align 4
	.global sceShellSvcGetSvcObj
	.type sceShellSvcGetSvcObj, %function
sceShellSvcGetSvcObj:
	.word 0x00000000
	.word 0x773242C7
	.word 0xB31E7F1C
	.align 4
