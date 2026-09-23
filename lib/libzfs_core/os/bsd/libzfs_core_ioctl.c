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
#include <sys/zfs_ioctl.h>
#include <libzfs_core.h>
#include "libzfs_core_impl.h"

/*
 * The zfs_cmd_t goes to the kernel as it stands.  FreeBSD wraps it in a
 * zfs_iocparm_t to keep its older ioctl ABI working; there is no such
 * older ABI here to keep.
 */
int
lzc_ioctl_fd_os(int fd, unsigned long request, zfs_cmd_t *zc)
{
	return (ioctl(fd, request, zc));
}
