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

#ifndef _SPL_SYS_TIMER_H
#define	_SPL_SYS_TIMER_H

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/systm.h>

#define	ddi_time_after(a, b)	((a) > (b))
#define	ddi_time_after64(a, b)	((a) > (b))

/*
 * NetBSD sleeps in clock ticks, so the shortest wait asked for is
 * rounded up to whole ticks, and to at least one: kpause() with no
 * ticks would not sleep at all.
 */
#define	usleep_range(wakeup, wakeupepsilon)				\
	(void) kpause("usleep", false,					\
	    MAX(1, (int)howmany((uint64_t)(wakeup) * hz, 1000000)), NULL)

#endif	/* _SPL_SYS_TIMER_H */
