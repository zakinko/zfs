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
 * The kernel's use of the vector units.  Not yet wired to NetBSD's
 * fpu_kern_enter(): the module is built without HAVE_TOOLCHAIN_*, so
 * no vector implementation is compiled in, and these report every unit
 * absent so that the generic ones are chosen.
 */
#include <sys/types.h>

#define	kfpu_allowed()		0
#define	kfpu_initialize(tsk)	do {} while (0)
#define	kfpu_begin()		do {} while (0)
#define	kfpu_end()		do {} while (0)
#define	kfpu_init()		(0)
#define	kfpu_fini()		do {} while (0)

#define	simd_stat_init()	do {} while (0)
#define	simd_stat_fini()	do {} while (0)

#define	zfs_aes_available()	B_FALSE
#define	zfs_altivec_available()	B_FALSE
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
#define	zfs_isa207_available()	B_FALSE
#define	zfs_movbe_available()	B_FALSE
#define	zfs_neon_available()	B_FALSE
#define	zfs_pclmulqdq_available()	B_FALSE
#define	zfs_sha256_available()	B_FALSE
#define	zfs_sha512_available()	B_FALSE
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
#define	zfs_vsx_available()	B_FALSE

#endif	/* _SPL_SYS_SIMD_H */
