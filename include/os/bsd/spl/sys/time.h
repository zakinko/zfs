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

#ifndef _SPL_SYS_TIME_H
#define	_SPL_SYS_TIME_H

#include_next <sys/time.h>
#include <sys/timevar.h>
#include <sys/kernel.h>

#define	SEC		1
#define	MILLISEC	1000UL
#define	MICROSEC	1000000UL
#define	NANOSEC		1000000000UL
#define	TIME_MAX	INT64_MAX

#define	MSEC2NSEC(m)	((hrtime_t)(m) * (NANOSEC / MILLISEC))
#define	NSEC2MSEC(n)	((n) / (NANOSEC / MILLISEC))
#define	USEC2NSEC(m)	((hrtime_t)(m) * (NANOSEC / MICROSEC))
#define	NSEC2USEC(n)	((n) / (NANOSEC / MICROSEC))
#define	NSEC2SEC(n)	((n) / (NANOSEC / SEC))
#define	SEC2NSEC(m)	((hrtime_t)(m) * (NANOSEC / SEC))

#ifdef _LP64
#define	TIMESPEC_OVERFLOW(ts)	0
#else
#define	TIMESPEC_OVERFLOW(ts)						\
	((ts)->tv_sec < INT32_MIN || (ts)->tv_sec > INT32_MAX)
#endif

#define	SEC_TO_TICK(sec)	((sec) * hz)
#define	NSEC_TO_TICK(nsec)	((nsec) / (NANOSEC / hz))

/*
 * gethrtime() is illumos's monotonic clock in nanoseconds, which is
 * NetBSD's uptime; getlrtime() is the cheaper, tick-resolution read of
 * the same clock, as getnanouptime() is of nanouptime().
 */
static __inline hrtime_t
gethrtime(void)
{
	struct timespec ts;

	nanouptime(&ts);
	return ((hrtime_t)ts.tv_sec * NANOSEC + ts.tv_nsec);
}

static __inline hrtime_t
getlrtime(void)
{
	struct timespec ts;

	getnanouptime(&ts);
	return ((hrtime_t)ts.tv_sec * NANOSEC + ts.tv_nsec);
}

#define	gethrtime_waitfree()	gethrtime()
#define	gethrestime_sec()	(time_second)
#define	gethrestime(ts)		getnanotime(ts)

/*
 * illumos's lbolt counts clock ticks since boot.  NetBSD's own count,
 * getticks(), is an int, which at hz=100 wraps after 248 days, and the
 * common code adds intervals to lbolt and compares; so the count is
 * taken from the uptime instead, at the same rate.
 */
static __inline int64_t
ddi_get_lbolt64(void)
{
	struct bintime bt;

	getbinuptime(&bt);
	return ((int64_t)bt.sec * hz + (int64_t)(((bt.frac >> 32) * hz) >> 32));
}
#define	ddi_get_lbolt()		((clock_t)ddi_get_lbolt64())

#endif	/* _SPL_SYS_TIME_H */
