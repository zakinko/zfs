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

#ifndef _SYS_ZFS_IOCTL_COMPAT_H
#define	_SYS_ZFS_IOCTL_COMPAT_H

#include <sys/types.h>
#include <sys/ioccom.h>

/*
 * A zfs_cmd_t is larger than NetBSD's ioctl(2) will copy in for a
 * command (IOCPARM_MAX, a page), so the command carries where it is
 * instead, as FreeBSD's does and NetBSD's own ZFS did: the ioctl is
 * _IOWR('Z', n, zfs_iocparm_t), and the kernel copies the zfs_cmd_t in
 * and back out itself.  The version and size let the kernel refuse a
 * zfs_cmd_t of another layout instead of misreading it.  The version is
 * FreeBSD's for the same layout, which there is no reason to differ
 * from.
 */
#define	ZFS_IOCVER_OZFS		15

#define	ZFS_IOCREQ(ioreq)	((ioreq) & 0xff)

typedef struct zfs_iocparm {
	uint32_t	zfs_ioctl_version;
	uint64_t	zfs_cmd;
	uint64_t	zfs_cmd_size;
} zfs_iocparm_t;

#endif	/* _SYS_ZFS_IOCTL_COMPAT_H */
