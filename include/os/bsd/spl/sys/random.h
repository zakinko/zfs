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

#ifndef _SPL_SYS_RANDOM_H
#define	_SPL_SYS_RANDOM_H

/*
 * illumos's random_get_bytes() is the strong generator and
 * random_get_pseudo_bytes() the fast one; NetBSD's are cprng_strong()
 * on kern_cprng and cprng_fast().
 */
#include_next <sys/random.h>
#include <sys/cprng.h>

static __inline int
random_get_bytes(uint8_t *p, size_t s)
{

	(void) cprng_strong(kern_cprng, p, s, 0);
	return (0);
}

static __inline int
random_get_pseudo_bytes(uint8_t *p, size_t s)
{

	cprng_fast(p, s);
	return (0);
}

/* A value in [0, range), as illumos takes it, by the remainder. */
static __inline uint32_t
random_in_range(uint32_t range)
{

	if (range <= 1)
		return (0);
	return (cprng_fast32() % range);
}

#endif	/* _SPL_SYS_RANDOM_H */
