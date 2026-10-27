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

#ifndef _SYS_ZFS_MOUNT_ARGS_H
#define	_SYS_ZFS_MOUNT_ARGS_H

#include <sys/param.h>

/*
 * What mount(2) is handed for a ZFS file system: the dataset, and the
 * options as zfs(8) gives them, a comma-separated string.  This is the
 * layout NetBSD's own ZFS has always used, so that its mount_zfs keeps
 * working; the kernel takes its size as vfs_min_mount_data.
 */
struct zfs_args {
	char	fspec[MAXNAMELEN - 1];
	char	dataptr[MAXPATHLEN];
	char	optptr[MAXPATHLEN];
	char	*fstype;
	int	mflag;
	int	datalen;
	int	optlen;
	int	flags;
};

#endif	/* _SYS_ZFS_MOUNT_ARGS_H */
