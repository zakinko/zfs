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

#ifndef	_LIBSPL_SYS_MOUNT_H
#define	_LIBSPL_SYS_MOUNT_H

#include_next <sys/mount.h>

#define	MS_RDONLY	MNT_RDONLY
#define	MS_NOSUID	MNT_NOSUID
#define	MS_NOEXEC	MNT_NOEXEC
#define	MS_REMOUNT	MNT_UPDATE
#define	MS_FORCE	MNT_FORCE

/* There is no nodev mount flag, and nothing to stand in for one. */
#define	MS_NODEV	0

/* Not mount flags; the tree uses them to carry its own requests. */
#define	MS_DETACH	0x00000002
#define	MS_OVERLAY	0x20000000
#define	MS_CRYPT	0x40000000

#endif	/* _LIBSPL_SYS_MOUNT_H */
