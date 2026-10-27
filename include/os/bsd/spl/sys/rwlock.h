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

#ifndef _SPL_SYS_RWLOCK_H
#define	_SPL_SYS_RWLOCK_H

/*
 * illumos's reader/writer lock is NetBSD's: krwlock_t, krw_t with
 * RW_READER and RW_WRITER, rw_enter(), rw_exit(), rw_tryenter(),
 * rw_tryupgrade() and rw_downgrade() are the same calls.  Neither
 * system's is recursive for readers that a writer is waiting behind,
 * which is why the common code has rrwlock_t of its own.
 */
#include_next <sys/rwlock.h>

typedef enum {
	RW_DRIVER = 2,
	RW_DEFAULT = 4
} krw_type_t;

/*
 * The common code passes RW_NONE where no lock is wanted; NetBSD's
 * krw_t has 0 and 1 only, so it is a value outside both.
 */
#define	RW_NONE			((krw_t)-1)

#define	RW_READ_HELD(x)		(rw_read_held(x))
#define	RW_WRITE_HELD(x)	(rw_write_held(x))
#define	RW_LOCK_HELD(x)		(rw_lock_held(x))
#define	RW_ISWRITER(x)		(rw_write_held(x))

#define	rw_init(lock, name, type, arg)	rw_init(lock)

#endif	/* _SPL_SYS_RWLOCK_H */
