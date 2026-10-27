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

#ifndef _SPL_SYS_CALLB_H
#define	_SPL_SYS_CALLB_H

/*
 * illumos's callbacks are for checkpoint and resume, which NetBSD does
 * not do: nothing ever asks a thread to stop, so the CPR state a
 * thread keeps is kept and never consulted, and no callback is
 * registered.
 */
#include <sys/mutex.h>
#include <sys/condvar.h>

#define	CB_CL_CPR_DAEMON	0

typedef void *callb_id_t;

typedef struct callb_cpr {
	kmutex_t	*cc_lockp;
	char		cc_events;
	callb_id_t	cc_id;
	kcondvar_t	cc_callb_cv;
	kcondvar_t	cc_stop_cv;
} callb_cpr_t;

#define	CALLB_CPR_START		1
#define	CALLB_CPR_SAFE		2
#define	CALLB_CPR_ALWAYS_SAFE	4

#define	CALLB_CPR_INIT(cp, lockp, func, name)	do {			\
	memset((cp), 0, sizeof (callb_cpr_t));				\
	(cp)->cc_lockp = (lockp);					\
	(cp)->cc_id = NULL;						\
	cv_init(&(cp)->cc_callb_cv, NULL, CV_DEFAULT, NULL);		\
	cv_init(&(cp)->cc_stop_cv, NULL, CV_DEFAULT, NULL);		\
} while (0)

#define	CALLB_CPR_ASSERT(cp)	ASSERT(MUTEX_HELD((cp)->cc_lockp))

#define	CALLB_CPR_SAFE_BEGIN(cp)	do {				\
	CALLB_CPR_ASSERT(cp);						\
	(cp)->cc_events |= CALLB_CPR_SAFE;				\
} while (0)

#define	CALLB_CPR_SAFE_END(cp, lockp)	do {				\
	CALLB_CPR_ASSERT(cp);						\
	(cp)->cc_events &= ~CALLB_CPR_SAFE;				\
} while (0)

#define	CALLB_CPR_EXIT(cp)	do {					\
	CALLB_CPR_ASSERT(cp);						\
	mutex_exit((cp)->cc_lockp);					\
	cv_destroy(&(cp)->cc_callb_cv);					\
	cv_destroy(&(cp)->cc_stop_cv);					\
} while (0)

#endif	/* _SPL_SYS_CALLB_H */
