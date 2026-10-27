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

#ifndef _SYS_ZFS_CONTEXT_OS_H
#define	_SYS_ZFS_CONTEXT_OS_H

#include <sys/types.h>
#include <sys/condvar.h>
#include <sys/rwlock.h>
#include <sys/misc.h>
#include <sys/proc.h>
#include <sys/lwp.h>
#include <sys/signalvar.h>
#include <sys/systm.h>
#include <sys/ccompat.h>
#include <sys/sig.h>
#include <sys/sdt.h>

/*
 * HAVE_LARGE_STACKS is left undefined.  NetBSD/amd64 gives a kernel
 * thread 12KiB of stack (UPAGES_STACK), less than the 16KiB at which
 * FreeBSD defines it; without it the I/O pipeline hands work to its
 * task queues rather than recursing on the caller's stack.
 */

#define	taskq_create_sysdc(a, b, d, e, p, dc, f)			\
	((void) sizeof (dc), taskq_create(a, b, maxclsyspri, d, e, f))

/*
 * illumos's thread-specific data is NetBSD's lwp specificdata; a key
 * is an unsigned int in both.
 */
#define	tsd_create(keyp, destructor)					\
	VERIFY0(lwp_specific_key_create((keyp), (destructor)))
#define	tsd_destroy(keyp)	lwp_specific_key_delete(*(keyp))
#define	tsd_get(key)		lwp_getspecific(key)
#define	tsd_set(key, value)	(lwp_setspecific((key), (value)), 0)

#define	fm_panic		panic

extern int zfs_debug_level;
#define	ZFS_LOG(lvl, ...) do {						\
	if (((lvl) & 0xff) <= zfs_debug_level) {			\
		printf("%s:%u[%d]: ", __func__, __LINE__, (lvl));	\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

#define	MSEC_TO_TICK(msec)	(howmany((hrtime_t)(msec) * hz, MILLISEC))

/* Linux's marking of a thread doing filesystem work, unneeded here. */
typedef int fstrans_cookie_t;
#define	spl_fstrans_mark()	(0)
#define	spl_fstrans_unmark(x)	((void)(x))

#define	signal_pending(l)	(sigispending((l), 0) != 0)
#define	current			curlwp
#define	thread_join(x)

typedef struct opensolaris_utsname	utsname_t;
extern utsname_t *utsname(void);

#endif	/* _SYS_ZFS_CONTEXT_OS_H */
