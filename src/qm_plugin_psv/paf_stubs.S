/*
 * PAF imports of VitaPlex's quick menu plugin, written by
 * scripts/vita/gen_paf_stubs.py from ScePaf-RE's NIDs. Not weak, so a
 * firmware without one of them refuses to load the plugin.
 */

	.arch armv7a

	.section .vitalink.fstubs.ScePafCommon,"ax",%progbits

	.align 4
	.global _ZN3paf15common_internal8on_errorEv
	.type _ZN3paf15common_internal8on_errorEv, %function
_ZN3paf15common_internal8on_errorEv:
	.word 0x00000000
	.word 0x3C1965CD
	.word 0x6A687A77
	.align 4

	.align 4
	.global _ZN3paf6common18MainThreadCallList8RegisterEPFvPvES2_
	.type _ZN3paf6common18MainThreadCallList8RegisterEPFvPvES2_, %function
_ZN3paf6common18MainThreadCallList8RegisterEPFvPvES2_:
	.word 0x00000000
	.word 0x3C1965CD
	.word 0x6735B503
	.align 4

	.section .vitalink.fstubs.ScePafGraphics,"ax",%progbits

	.align 4
	.global _ZN3paf5graph7Surface4LoadEPNS0_11SurfacePoolENS_6common9SharedPtrINS_4FileEEEPNS1_10LoadOptionE
	.type _ZN3paf5graph7Surface4LoadEPNS0_11SurfacePoolENS_6common9SharedPtrINS_4FileEEEPNS1_10LoadOptionE, %function
_ZN3paf5graph7Surface4LoadEPNS0_11SurfacePoolENS_6common9SharedPtrINS_4FileEEEPNS1_10LoadOptionE:
	.word 0x00000000
	.word 0xA070D6A7
	.word 0xB228A257
	.align 4

	.section .vitalink.fstubs.ScePafMisc,"ax",%progbits

	.align 4
	.global _ZN3paf9LocalFile4OpenEPKcjjPi
	.type _ZN3paf9LocalFile4OpenEPKcjjPi, %function
_ZN3paf9LocalFile4OpenEPKcjjPi:
	.word 0x00000000
	.word 0x3D643CE8
	.word 0xB9FB9BD6
	.align 4

	.section .vitalink.fstubs.ScePafStdc,"ax",%progbits

	.align 4
	.global _ZdlPv
	.type _ZdlPv, %function
_ZdlPv:
	.word 0x00000000
	.word 0xA7D28DAE
	.word 0x1B77082E
	.align 4

	.align 4
	.global _Znwj
	.type _Znwj, %function
_Znwj:
	.word 0x00000000
	.word 0xA7D28DAE
	.word 0xFC5CD359
	.align 4

	.align 4
	.global sce_paf_free
	.type sce_paf_free, %function
sce_paf_free:
	.word 0x00000000
	.word 0xA7D28DAE
	.word 0x347AFDD9
	.align 4

	.align 4
	.global sce_paf_malloc
	.type sce_paf_malloc, %function
sce_paf_malloc:
	.word 0x00000000
	.word 0xA7D28DAE
	.word 0x0886C1E3
	.align 4

	.align 4
	.global sce_paf_wcslen
	.type sce_paf_wcslen, %function
sce_paf_wcslen:
	.word 0x00000000
	.word 0xA7D28DAE
	.word 0xBED4F3D0
	.align 4

	.align 4
	.global sce_paf_wmemcpy
	.type sce_paf_wmemcpy, %function
sce_paf_wmemcpy:
	.word 0x00000000
	.word 0xA7D28DAE
	.word 0x19B5B91D
	.align 4

	.section .vitalink.fstubs.ScePafThread,"ax",%progbits

	.align 4
	.global _ZN3paf6thread5SleepEi
	.type _ZN3paf6thread5SleepEi, %function
_ZN3paf6thread5SleepEi:
	.word 0x00000000
	.word 0x54276D19
	.word 0x651F4F81
	.align 4

	.align 4
	.global _ZN3paf6thread6Thread10IsCanceledEv
	.type _ZN3paf6thread6Thread10IsCanceledEv, %function
_ZN3paf6thread6Thread10IsCanceledEv:
	.word 0x00000000
	.word 0x54276D19
	.word 0x4CE6D88B
	.align 4

	.align 4
	.global _ZN3paf6thread6Thread5StartEv
	.type _ZN3paf6thread6Thread5StartEv, %function
_ZN3paf6thread6Thread5StartEv:
	.word 0x00000000
	.word 0x54276D19
	.word 0x60910895
	.align 4

	.align 4
	.global _ZN3paf6thread6ThreadC2EijPKcPKNS1_6OptionE
	.type _ZN3paf6thread6ThreadC2EijPKcPKNS1_6OptionE, %function
_ZN3paf6thread6ThreadC2EijPKcPKNS1_6OptionE:
	.word 0x00000000
	.word 0x54276D19
	.word 0x6E829BD8
	.align 4

	.align 4
	.global _ZN3paf6thread6ThreadD2Ev
	.type _ZN3paf6thread6ThreadD2Ev, %function
_ZN3paf6thread6ThreadD2Ev:
	.word 0x00000000
	.word 0x54276D19
	.word 0x7EA6B501
	.align 4

	.section .vitalink.fstubs.ScePafToplevel,"ax",%progbits

	.align 4
	.global _ZN3paf6Plugin4FindEPKc
	.type _ZN3paf6Plugin4FindEPKc, %function
_ZN3paf6Plugin4FindEPKc:
	.word 0x00000000
	.word 0x4D9A9DD0
	.word 0x004D98CC
	.align 4

	.align 4
	.global _ZNK3paf6Plugin12GetInterfaceEi
	.type _ZNK3paf6Plugin12GetInterfaceEi, %function
_ZNK3paf6Plugin12GetInterfaceEi:
	.word 0x00000000
	.word 0x4D9A9DD0
	.word 0x1DF2C6FD
	.align 4

	.section .vitalink.fstubs.ScePafWidget,"ax",%progbits

	.align 4
	.global _ZN3paf2ui10ButtonBase6EnableEb
	.type _ZN3paf2ui10ButtonBase6EnableEb, %function
_ZN3paf2ui10ButtonBase6EnableEb:
	.word 0x00000000
	.word 0x073F8C68
	.word 0x8F1A3A59
	.align 4

	.align 4
	.global _ZN3paf2ui10ButtonBase8TypeNameEv
	.type _ZN3paf2ui10ButtonBase8TypeNameEv, %function
_ZN3paf2ui10ButtonBase8TypeNameEv:
	.word 0x00000000
	.word 0x073F8C68
	.word 0xA3254FB6
	.align 4

	.align 4
	.global _ZN3paf2ui4Text8TypeNameEv
	.type _ZN3paf2ui4Text8TypeNameEv, %function
_ZN3paf2ui4Text8TypeNameEv:
	.word 0x00000000
	.word 0x073F8C68
	.word 0x723F6A4C
	.align 4

	.align 4
	.global _ZN3paf2ui6Widget9FindChildERKNS_7IDParamEi
	.type _ZN3paf2ui6Widget9FindChildERKNS_7IDParamEi, %function
_ZN3paf2ui6Widget9FindChildERKNS_7IDParamEi:
	.word 0x00000000
	.word 0x073F8C68
	.word 0x9D304F4A
	.align 4

	.align 4
	.global _ZN3paf2ui7Handler16AddEventListenerEiPNS0_13EventListenerEb
	.type _ZN3paf2ui7Handler16AddEventListenerEiPNS0_13EventListenerEb, %function
_ZN3paf2ui7Handler16AddEventListenerEiPNS0_13EventListenerEb:
	.word 0x00000000
	.word 0x073F8C68
	.word 0xFB7FE189
	.align 4

	.align 4
	.global _ZNK3paf2ui6Widget8GetChildEi
	.type _ZNK3paf2ui6Widget8GetChildEi, %function
_ZNK3paf2ui6Widget8GetChildEi:
	.word 0x00000000
	.word 0x073F8C68
	.word 0xB8CA08DA
	.align 4

	.section .vitalink.vstubs.ScePafStdc,"",%progbits

	.align 4
	.global _ZN3paf12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE12empty_stringE
	.type _ZN3paf12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE12empty_stringE, %object
_ZN3paf12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE12empty_stringE:
	.word 0x00000000
	.word 0xA7D28DAE
	.word 0x610F2F91
	.align 4

	.align 4
	.global _ZN3paf12basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEE12empty_stringE
	.type _ZN3paf12basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEE12empty_stringE, %object
_ZN3paf12basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEE12empty_stringE:
	.word 0x00000000
	.word 0xA7D28DAE
	.word 0xB4A712A7
	.align 4

	.section .note.GNU-stack,"",%progbits
