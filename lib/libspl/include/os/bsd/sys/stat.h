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

#ifndef	_LIBSPL_SYS_STAT_H
#define	_LIBSPL_SYS_STAT_H

#include_next <sys/stat.h>

#include <sys/dkio.h>
#include <sys/ioctl.h>


/*
 * fstat(2) reports no size for a disk device, so ask the driver.
 */
static inline int
fstat64(int fd, struct stat *sb)
{
	off_t dsize;

	if (fstat(fd, sb) == -1)
		return (-1);
	if (sb->st_size == 0 &&
	    (S_ISCHR(sb->st_mode) || S_ISBLK(sb->st_mode)) &&
	    ioctl(fd, DIOCGMEDIASIZE, &dsize) == 0)
		sb->st_size = dsize;
	return (0);
}

static inline int
fstat64_blk(int fd, struct stat64 *st)
{
	return (fstat64(fd, st));
}

#endif	/* _LIBSPL_SYS_STAT_H */
