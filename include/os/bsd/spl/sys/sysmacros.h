// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#ifndef _SYS_SYSMACROS_H
#define	_SYS_SYSMACROS_H

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/cpu.h>
#include <sys/isa_defs.h>
#include <sys/zone.h>
#include <sys/condvar.h>

#ifdef	__cplusplus
extern "C" {
#endif

#define	dtob(DD)	((DD) << DEV_BSHIFT)
#define	btod(BB)	(((BB) + DEV_BSIZE - 1) >> DEV_BSHIFT)
#define	btodt(BB)	((BB) >> DEV_BSHIFT)
#define	lbtod(BB)	(((offset_t)(BB) + DEV_BSIZE - 1) >> DEV_BSHIFT)

/* MIN, MAX, howmany and roundup are NetBSD's, from <sys/param.h>. */
#ifndef ABS
#define	ABS(a)		((a) < 0 ? -(a) : (a))
#endif
#ifndef	SIGNOF
#define	SIGNOF(a)	((a) < 0 ? -1 : (a) > 0)
#endif
#ifndef	ARRAY_SIZE
#define	ARRAY_SIZE(a) (sizeof (a) / sizeof (a[0]))
#endif
#ifndef	DIV_ROUND_UP
#define	DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))
#endif

/*
 * kpreempt_disable() and kpreempt_enable() are NetBSD's own, with the
 * illumos meaning: the thread stays on this CPU until it re-enables.
 */
#define	boot_ncpus		ncpu
#define	CPU_SEQID		cpu_index(curcpu())
#define	CPU_SEQID_UNSTABLE	cpu_index(curcpu())

#define	is_system_labeled()		0

#define	IS_P2ALIGNED(v, a) ((((uintptr_t)(v)) & ((uintptr_t)(a) - 1)) == 0)
#define	ISP2(x)		(((x) & ((x) - 1)) == 0)

#define	P2PHASE(x, align)		((x) & ((align) - 1))
#define	P2NPHASE(x, align)		(-(x) & ((align) - 1))
#define	P2ROUNDUP(x, align)		(-(-(x) & -(align)))
#define	P2END(x, align)			(-(~(x) & -(align)))
#define	P2PHASEUP(x, align, phase)	((phase) - (((phase) - (x)) & -(align)))
#define	P2BOUNDARY(off, len, align) \
	(((off) ^ ((off) + (len) - 1)) > (align) - 1)
#define	P2CROSS(x, y, align)		(((x) ^ (y)) > (align) - 1)
#define	P2SAMEHIGHBIT(x, y)		(((x) ^ (y)) < ((x) & (y)))

#define	P2ALIGN_TYPED(x, align, type)	\
	((type)(x) & -(type)(align))
#define	P2PHASE_TYPED(x, align, type)	\
	((type)(x) & ((type)(align) - 1))
#define	P2NPHASE_TYPED(x, align, type)	\
	(-(type)(x) & ((type)(align) - 1))
#define	P2ROUNDUP_TYPED(x, align, type)	\
	(-(-(type)(x) & -(type)(align)))
#define	P2END_TYPED(x, align, type)	\
	(-(~(type)(x) & -(type)(align)))
#define	P2PHASEUP_TYPED(x, align, phase, type)	\
	((type)(phase) - (((type)(phase) - (type)(x)) & -(type)(align)))
#define	P2CROSS_TYPED(x, y, align, type)	\
	(((type)(x) ^ (type)(y)) > (type)(align) - 1)
#define	P2SAMEHIGHBIT_TYPED(x, y, type) \
	(((type)(x) ^ (type)(y)) < ((type)(x) & (type)(y)))

#define	INCR_COUNT(var, mutex) mutex_enter(mutex), (*(var))++, mutex_exit(mutex)
#define	DECR_COUNT(var, mutex) mutex_enter(mutex), (*(var))--, mutex_exit(mutex)

/*
 * illumos's highbit() and lowbit() number bits from 1 and return 0 for
 * 0, as fls and ffs do.  NetBSD's are in <sys/bitops.h>, a name the
 * common code's own include/sys/bitops.h takes first on the include
 * path, so the compiler's builtins are used instead.
 */
static __inline int
highbit64(uint64_t x)
{

	return (x == 0 ? 0 : 64 - __builtin_clzll(x));
}

static __inline int
lowbit64(uint64_t x)
{

	return (__builtin_ffsll((long long)x));
}

static __inline int
highbit(ulong_t x)
{

	return (highbit64(x));
}

static __inline int
lowbit(ulong_t x)
{

	return (lowbit64(x));
}

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_SYSMACROS_H */
