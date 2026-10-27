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

#ifndef _SYS_ZFS_DEBUG_OS_H
#define	_SYS_ZFS_DEBUG_OS_H

/*
 * An error fires the sdt:::set-error probe where the kernel has DTrace
 * hooks, as NetBSD's own file systems' errors do, and is also written
 * to the debug log when vfs.zfs.flags asks, as on Linux: a NetBSD
 * kernel often has no DTrace to ask with.  <sys/sdt.h> has a SET_ERROR
 * of its own, which this replaces.
 */
#ifdef _KERNEL
#include <sys/sdt.h>
#undef SET_ERROR
#ifdef KDTRACE_HOOKS
#define	SET_ERROR(err)							\
	(__set_error(__FILE__, __func__, __LINE__, (err)),		\
	SDT_PROBE1(sdt, , , set__error, (err)), (err))
#else
#define	SET_ERROR(err)							\
	(__set_error(__FILE__, __func__, __LINE__, (err)), (err))
#endif
#else
#define	SET_ERROR(err)	(err)
#endif

#endif	/* _SYS_ZFS_DEBUG_OS_H */
