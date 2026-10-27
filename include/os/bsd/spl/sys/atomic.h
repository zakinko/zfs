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

#ifndef _SPL_SYS_ATOMIC_H
#define	_SPL_SYS_ATOMIC_H

/*
 * NetBSD's atomic_ops(3) are illumos's: the same names, arguments and
 * return values for add, and, or, cas, swap, inc and dec, in 32 and 64
 * bit, int, long and pointer widths, and membar_producer(),
 * membar_consumer() and membar_sync().  What illumos lacks and the
 * common code takes from Linux and FreeBSD is added here.
 */
#include_next <sys/atomic.h>

#define	atomic_sub_32(p, v)	atomic_add_32((p), -(int32_t)(v))
#define	atomic_sub_32_nv(p, v)	atomic_add_32_nv((p), -(int32_t)(v))
#define	atomic_sub_64(p, v)	atomic_add_64((p), -(int64_t)(v))
#define	atomic_sub_64_nv(p, v)	atomic_add_64_nv((p), -(int64_t)(v))

static __inline uint32_t
atomic_load_32(volatile uint32_t *p)
{

	return (atomic_load_relaxed(p));
}

static __inline void
atomic_store_32(volatile uint32_t *p, uint32_t v)
{

	atomic_store_relaxed(p, v);
}

/*
 * A 64 bit load or store is a single access only where the machine
 * has one; atomic_load_relaxed() will not take a 64 bit object
 * elsewhere.  There the compare-and-swap does it whole, as FreeBSD's
 * i386 does.
 */
#ifdef __HAVE_ATOMIC64_LOADSTORE
static __inline uint64_t
atomic_load_64(volatile uint64_t *p)
{

	return (atomic_load_relaxed(p));
}

static __inline void
atomic_store_64(volatile uint64_t *p, uint64_t v)
{

	atomic_store_relaxed(p, v);
}
#else
static __inline uint64_t
atomic_load_64(volatile uint64_t *p)
{

	return (atomic_cas_64(p, 0, 0));
}

static __inline void
atomic_store_64(volatile uint64_t *p, uint64_t v)
{

	(void) atomic_swap_64(p, v);
}
#endif

#define	atomic_load_ptr(p)	atomic_load_relaxed(p)
#define	atomic_store_ptr(p, v)	atomic_store_relaxed((p), (v))

#endif	/* _SPL_SYS_ATOMIC_H */
