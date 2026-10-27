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
#include <sys/types.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/zfs_ioctl.h>
#include <os/bsd/zfs/sys/zfs_ioctl_compat.h>
#include <errno.h>
#include <libzfs_core.h>
#include "libzfs_core_impl.h"

/*
 * The zfs_cmd_t is too large to be an ioctl(2) argument, so the
 * argument says where it is; see <os/bsd/zfs/sys/zfs_ioctl_compat.h>.
 * A result that did not fit in the caller's buffer comes back with the
 * size it needed, as on FreeBSD, and is ENOMEM.
 */
int
lzc_ioctl_fd_os(int fd, unsigned long request, zfs_cmd_t *zc)
{
	zfs_iocparm_t zp;
	size_t oldsize;
	int ret;

	zp.zfs_ioctl_version = ZFS_IOCVER_OZFS;
	zp.zfs_cmd = (uint64_t)(uintptr_t)zc;
	zp.zfs_cmd_size = sizeof (zfs_cmd_t);

	oldsize = zc->zc_nvlist_dst_size;
	ret = ioctl(fd, _IOWR('Z', ZFS_IOCREQ(request), zfs_iocparm_t), &zp);
	if (ret == 0 && oldsize < zc->zc_nvlist_dst_size) {
		ret = -1;
		errno = ENOMEM;
	}
	return (ret);
}
