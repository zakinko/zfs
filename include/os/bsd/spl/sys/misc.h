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

#ifndef _SPL_SYS_MISC_H
#define	_SPL_SYS_MISC_H

#include <sys/syslimits.h>
#include <sys/filio.h>
#include <sys/lwp.h>

#define	MAXUID			UID_MAX

/*
 * What an id with no mapping becomes: NetBSD's own nobody and nogroup,
 * not illumos's 60001 or FreeBSD's 65534.
 */
#define	UID_NOBODY		32767
#define	GID_NOBODY		32766

#define	_ACL_ACLENT_ENABLED	0x1
#define	_ACL_ACE_ENABLED	0x2

#define	_FIOFFS			(INT_MIN)
#define	_FIOGDIO		(INT_MIN + 1)
#define	_FIOSDIO		(INT_MIN + 2)

#define	F_SEEK_DATA		FIOSEEKDATA
#define	F_SEEK_HOLE		FIOSEEKHOLE

struct opensolaris_utsname {
	const char	*sysname;
	const char	*nodename;
	const char	*release;
	char		version[32];
	const char	*machine;
};

/* Linux's per-task I/O accounting, which NetBSD does not keep. */
#define	task_io_account_read(n)
#define	task_io_account_write(n)

/*
 * Whether this thread is the one memory reclaim runs on: NetBSD's page
 * daemon, which ARC must not wait on itself from.
 */
extern int current_is_reclaim_thread(void);

/*
 * NetBSD's kernel sorts with kheapsort(); qsort() is the libc name the
 * common code uses.  See spl_misc.c.
 */
extern void qsort(void *, size_t, size_t,
    int (*)(const void *, const void *));

/*
 * NetBSD's kernel has no sscanf().  This one reads integers, %d, %u,
 * %x with h, l and ll, and %n: what the common code asks of it.
 */
extern int sscanf(const char *, const char *, ...)
    __attribute__((format(__scanf__, 2, 3)));

#endif	/* _SPL_SYS_MISC_H */
