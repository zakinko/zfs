// SPDX-License-Identifier: CDDL-1.0
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * https://opensource.org/license/CDDL-1.0.
 */

#ifndef _SPL_SYS_SIMD_H
#define	_SPL_SYS_SIMD_H

/*
 * The kernel's use of the vector units.
 *
 * On x86_64 a section that uses them is bracketed by NetBSD's
 * fpu_kern_enter() and fpu_kern_leave(), which save the state of the
 * lwp's own and hand the unit over clean.  They hold the CPU at IPL_VM
 * between, so the section must not sleep, which none of OpenZFS's do,
 * as Linux's kernel_fpu_begin() asks the same; nor may one nest in
 * another, which NetBSD asserts and Linux warns of.
 *
 * Which units there are is read from the CPUID leaves NetBSD keeps in
 * cpu_feature[] -- 0 and 1 are leaf 1's %edx and %ecx, 5 and 6 leaf 7's
 * %ebx and %ecx, as identcpu.c fills them -- and, for the AVX and
 * AVX-512 registers, from whether XCR0 has the kernel saving them.
 * Leaf 7's subleaf 1, where the SHA512 extension is, is not kept.
 *
 * XGETBV may be used once CR4.OSXSAVE is set, which cpu_init() does for
 * a CPU that has XSAVE.  CPUID2_OSXSAVE, which FreeBSD tests, is not to
 * be had from cpu_feature[]: the copy is taken from the boot CPU before
 * cpu_init() sets CR4, so it has the bit clear even where XCR0 has AVX
 * on, and AVX would never be chosen.
 *
 * Elsewhere there are no vector implementations to choose.
 */
#include <sys/types.h>

#if defined(_KERNEL) && defined(__x86_64__)
#include <machine/cpufunc.h>
#include <machine/cpuvar.h>
#include <machine/fpu.h>
#include <machine/specialreg.h>

#define	kfpu_allowed()		1
#define	kfpu_initialize(tsk)	do {} while (0)
#define	kfpu_begin()		fpu_kern_enter()
#define	kfpu_end()		fpu_kern_leave()
#define	kfpu_init()		(0)
#define	kfpu_fini()		do {} while (0)

static inline boolean_t
__simd_state_enabled(uint64_t state)
{

	if ((cpu_feature[1] & CPUID2_XSAVE) == 0)
		return (B_FALSE);
	return ((rdxcr(0) & state) == state);
}

#define	__ymm_enabled()	__simd_state_enabled(XCR0_SSE | XCR0_YMM_Hi128)
#define	__zmm_enabled()	__simd_state_enabled(XCR0_SSE | XCR0_YMM_Hi128 | \
	XCR0_Opmask | XCR0_ZMM_Hi256 | XCR0_Hi16_ZMM)

#define	__cpuid1_edx(b)		((cpu_feature[0] & (b)) != 0)
#define	__cpuid1_ecx(b)		((cpu_feature[1] & (b)) != 0)
#define	__cpuid7_ebx(b)		((cpu_feature[5] & (b)) != 0)
#define	__cpuid7_ecx(b)		((cpu_feature[6] & (b)) != 0)

#define	zfs_sse_available()	__cpuid1_edx(CPUID_SSE)
#define	zfs_sse2_available()	__cpuid1_edx(CPUID_SSE2)
#define	zfs_sse3_available()	__cpuid1_ecx(CPUID2_SSE3)
#define	zfs_ssse3_available()	__cpuid1_ecx(CPUID2_SSSE3)
#define	zfs_sse4_1_available()	__cpuid1_ecx(CPUID2_SSE41)
#define	zfs_sse4_2_available()	__cpuid1_ecx(CPUID2_SSE42)
#define	zfs_aes_available()	__cpuid1_ecx(CPUID2_AESNI)
#define	zfs_pclmulqdq_available()	__cpuid1_ecx(CPUID2_PCLMULQDQ)
#define	zfs_movbe_available()	__cpuid1_ecx(CPUID2_MOVBE)
#define	zfs_avx_available()	(__cpuid1_ecx(CPUID2_AVX) && __ymm_enabled())
#define	zfs_avx2_available()	(__cpuid7_ebx(CPUID_SEF_AVX2) && \
	__ymm_enabled())
#define	zfs_bmi1_available()	__cpuid7_ebx(CPUID_SEF_BMI1)
#define	zfs_bmi2_available()	__cpuid7_ebx(CPUID_SEF_BMI2)
#define	zfs_shani_available()	(__cpuid7_ebx(CPUID_SEF_SHA) && \
	__ymm_enabled())
#define	zfs_sha512ext_available()	B_FALSE
#define	zfs_vaes_available()	(__cpuid7_ecx(CPUID_SEF_VAES) && \
	__ymm_enabled())
#define	zfs_vpclmulqdq_available()	(__cpuid7_ecx(CPUID_SEF_VPCLMULQDQ) && \
	__ymm_enabled())

/* AVX-512: the foundation and the subset, as FreeBSD and Linux ask. */
#define	__avx512(b)	(__cpuid7_ebx(CPUID_SEF_AVX512F) && \
	__cpuid7_ebx(b) && __zmm_enabled())
#define	__avx512_ecx(b)	(__cpuid7_ebx(CPUID_SEF_AVX512F) && \
	__cpuid7_ecx(b) && __zmm_enabled())
#define	zfs_avx512f_available()	__avx512(CPUID_SEF_AVX512F)
#define	zfs_avx512cd_available()	__avx512(CPUID_SEF_AVX512CD)
#define	zfs_avx512er_available()	__avx512(CPUID_SEF_AVX512ER)
#define	zfs_avx512pf_available()	__avx512(CPUID_SEF_AVX512PF)
#define	zfs_avx512bw_available()	__avx512(CPUID_SEF_AVX512BW)
#define	zfs_avx512dq_available()	__avx512(CPUID_SEF_AVX512DQ)
#define	zfs_avx512vl_available()	__avx512(CPUID_SEF_AVX512VL)
#define	zfs_avx512ifma_available()	__avx512(CPUID_SEF_AVX512_IFMA)
#define	zfs_avx512vbmi_available()	__avx512_ecx(CPUID_SEF_AVX512_VBMI)

#else	/* !(_KERNEL && __x86_64__) */

#define	kfpu_allowed()		0
#define	kfpu_initialize(tsk)	do {} while (0)
#define	kfpu_begin()		do {} while (0)
#define	kfpu_end()		do {} while (0)
#define	kfpu_init()		(0)
#define	kfpu_fini()		do {} while (0)

#define	zfs_aes_available()	B_FALSE
#define	zfs_avx2_available()	B_FALSE
#define	zfs_avx512bw_available()	B_FALSE
#define	zfs_avx512cd_available()	B_FALSE
#define	zfs_avx512dq_available()	B_FALSE
#define	zfs_avx512er_available()	B_FALSE
#define	zfs_avx512f_available()	B_FALSE
#define	zfs_avx512ifma_available()	B_FALSE
#define	zfs_avx512pf_available()	B_FALSE
#define	zfs_avx512vbmi_available()	B_FALSE
#define	zfs_avx512vl_available()	B_FALSE
#define	zfs_avx_available()	B_FALSE
#define	zfs_bmi1_available()	B_FALSE
#define	zfs_bmi2_available()	B_FALSE
#define	zfs_movbe_available()	B_FALSE
#define	zfs_pclmulqdq_available()	B_FALSE
#define	zfs_sha512ext_available()	B_FALSE
#define	zfs_shani_available()	B_FALSE
#define	zfs_sse2_available()	B_FALSE
#define	zfs_sse3_available()	B_FALSE
#define	zfs_sse4_1_available()	B_FALSE
#define	zfs_sse4_2_available()	B_FALSE
#define	zfs_sse_available()	B_FALSE
#define	zfs_ssse3_available()	B_FALSE
#define	zfs_vaes_available()	B_FALSE
#define	zfs_vpclmulqdq_available()	B_FALSE

#endif	/* _KERNEL && __x86_64__ */

#define	zfs_altivec_available()	B_FALSE
#define	zfs_isa207_available()	B_FALSE
#define	zfs_neon_available()	B_FALSE
#define	zfs_sha256_available()	B_FALSE
#define	zfs_sha512_available()	B_FALSE
#define	zfs_vsx_available()	B_FALSE

#define	simd_stat_init()	do {} while (0)
#define	simd_stat_fini()	do {} while (0)

#endif	/* _SPL_SYS_SIMD_H */
