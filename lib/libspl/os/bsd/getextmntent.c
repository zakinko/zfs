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

#include <stdio.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/mnttab.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libzutil.h>

int
getextmntent(const char *path, struct mnttab *entry, struct stat64 *statbuf)
{
	struct statvfs sfs;

	if (strlen(path) >= MAXPATHLEN) {
		(void) fprintf(stderr, "invalid object; pathname too long\n");
		return (-1);
	}

	if (stat64(path, statbuf) != 0) {
		(void) fprintf(stderr, "cannot open '%s': %s\n",
		    path, zfs_strerror(errno));
		return (-1);
	}

	if (statvfs(path, &sfs) != 0) {
		(void) fprintf(stderr, "%s: %s\n", path,
		    zfs_strerror(errno));
		return (-1);
	}
	statvfs2mnttab(&sfs, entry);
	return (0);
}
