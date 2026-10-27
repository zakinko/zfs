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

#ifndef _SPL_SYS_MUTEX_H
#define	_SPL_SYS_MUTEX_H

/*
 * illumos's kernel mutex is NetBSD's: kmutex_t, mutex_enter(),
 * mutex_exit(), mutex_tryenter() and mutex_owned() are the same calls
 * with the same meaning, and a MUTEX_DEFAULT mutex at IPL_NONE is an
 * adaptive sleep lock as illumos's is.  Only mutex_init() differs, in
 * its arguments.
 */
#include_next <sys/mutex.h>
#include <sys/intr.h>
#include <sys/systm.h>

#define	MUTEX_HELD(x)		(mutex_owned(x))
#define	MUTEX_NOT_HELD(x)	(!mutex_owned(x) || panicstr != NULL)

#define	mutex_init(lock, name, type, arg)	\
	mutex_init((lock), MUTEX_DEFAULT, IPL_NONE)

/*
 * NetBSD has no lock-order annotation to tell nested acquisition of a
 * lock class apart, so this is an ordinary enter.
 */
#define	mutex_enter_nested(lock, class)	mutex_enter(lock)

/*
 * Linux and FreeBSD can wait for a mutex with signals let through, and
 * spa_namespace_lock is taken that way so that a zpool command stuck
 * behind another can be interrupted.  A NetBSD mutex cannot be waited
 * on interruptibly, so here the wait is not interruptible and the
 * result is always success.
 */
#define	mutex_enter_interruptible(lock)	(mutex_enter(lock), 0)

#endif	/* _SPL_SYS_MUTEX_H */
