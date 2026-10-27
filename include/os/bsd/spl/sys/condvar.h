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

#ifndef _SPL_SYS_CONDVAR_H
#define	_SPL_SYS_CONDVAR_H

/*
 * kcondvar_t, cv_wait(), cv_signal() and cv_broadcast() are NetBSD's
 * own and mean what illumos's do.  The waits that can end early do
 * not: NetBSD returns an errno, illumos returns 0 when a signal ended
 * the wait, -1 when the time ran out, and a positive value otherwise;
 * and illumos's timed waits take an absolute time, NetBSD's a relative
 * one.  Those are wrapped here under the illumos names.
 */
#include_next <sys/condvar.h>
#include <sys/errno.h>
#include <sys/mutex.h>
#include <sys/time.h>
#include <machine/limits.h>

/*
 * <sys/time.h> here defines these, but it can be part way through
 * including this file when the functions below are reached.
 */
static __inline hrtime_t gethrtime(void);
static __inline int64_t ddi_get_lbolt64(void);

typedef enum {
	CV_DEFAULT,
	CV_DRIVER
} kcv_type_t;

#define	CALLOUT_FLAG_ABSOLUTE	0x1

/*
 * The name a waiting thread shows in ps(1) is the variable the
 * condition lives in, without the "&" or the "->" path before it.
 */
static __inline const char *
spl_cv_name(const char *s)
{
	const char *p;

	for (p = s; *p != '\0'; p++)
		if (*p >= 'a' && *p <= 'z')
			return (p);
	return (s);
}
#define	cv_init(cv, name, type, arg)	cv_init((cv), spl_cv_name(#cv))

static __inline int
spl_cv_wait_sig(kcondvar_t *cvp, kmutex_t *mp)
{

	return (cv_wait_sig(cvp, mp) == 0);
}
#define	cv_wait_sig(cvp, mp)	spl_cv_wait_sig((cvp), (mp))

/* illumos's timeout is an absolute lbolt. */
static __inline int
spl_cv_timedwait_common(kcondvar_t *cvp, kmutex_t *mp, clock_t timo,
    int sig)
{
	int64_t delta;
	int rc;

	delta = (int64_t)timo - ddi_get_lbolt64();
	/* NetBSD waits forever when given no ticks. */
	if (delta <= 0)
		return (-1);
	if (delta > INT_MAX)
		delta = INT_MAX;
	rc = sig ? (cv_timedwait_sig)(cvp, mp, (int)delta) :
	    (cv_timedwait)(cvp, mp, (int)delta);
	if (rc == EWOULDBLOCK)
		return (-1);
	if (rc == EINTR || rc == ERESTART)
		return (0);
	return (1);
}
#define	cv_timedwait(cvp, mp, timo)		\
	spl_cv_timedwait_common((cvp), (mp), (timo), 0)
#define	cv_timedwait_sig(cvp, mp, timo)		\
	spl_cv_timedwait_common((cvp), (mp), (timo), 1)

/*
 * The high resolution waits take nanoseconds, absolute when flag has
 * CALLOUT_FLAG_ABSOLUTE and relative otherwise; res is how late the
 * wakeup may be, which cv_timedwaitbt() takes as its epsilon.
 */
static __inline int
spl_cv_timedwait_hires_common(kcondvar_t *cvp, kmutex_t *mp, hrtime_t tim,
    hrtime_t res, int flag, int sig)
{
	struct bintime bt, eps;
	hrtime_t now;
	int rc;

	now = gethrtime();
	if (flag & CALLOUT_FLAG_ABSOLUTE)
		tim -= now;
	if (tim <= 0)
		return (-1);
	bt = ns2bintime((uint64_t)tim);
	eps = ns2bintime(res > 0 ? (uint64_t)res : 0);
	rc = sig ? cv_timedwaitbt_sig(cvp, mp, &bt, &eps) :
	    cv_timedwaitbt(cvp, mp, &bt, &eps);
	if (rc == EWOULDBLOCK)
		return (-1);
	if (rc == EINTR || rc == ERESTART)
		return (0);
	return (1);
}
#define	cv_timedwait_hires(cvp, mp, tim, res, flag)	\
	spl_cv_timedwait_hires_common((cvp), (mp), (tim), (res), (flag), 0)
#define	cv_timedwait_sig_hires(cvp, mp, tim, res, flag)	\
	spl_cv_timedwait_hires_common((cvp), (mp), (tim), (res), (flag), 1)

/*
 * Linux tells the scheduler whether a waiting thread counts as busy
 * on I/O or as idle; NetBSD keeps no such account, so these are the
 * plain waits.
 */
#define	cv_wait_io(cvp, mp)		cv_wait((cvp), (mp))
#define	cv_wait_idle(cvp, mp)		cv_wait((cvp), (mp))
#define	cv_wait_io_sig(cvp, mp)		cv_wait_sig((cvp), (mp))
#define	cv_timedwait_io(cvp, mp, t)	cv_timedwait((cvp), (mp), (t))
#define	cv_timedwait_idle(cvp, mp, t)	cv_timedwait((cvp), (mp), (t))
#define	cv_timedwait_sig_io(cvp, mp, t)	cv_timedwait_sig((cvp), (mp), (t))
#define	cv_timedwait_io_hires(cvp, mp, t, r, f)		\
	cv_timedwait_hires((cvp), (mp), (t), (r), (f))
#define	cv_timedwait_idle_hires(cvp, mp, t, r, f)	\
	cv_timedwait_hires((cvp), (mp), (t), (r), (f))

#endif	/* _SPL_SYS_CONDVAR_H */
