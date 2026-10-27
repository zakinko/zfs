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

#include <errno.h>
#include <paths.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/disklabel.h>
#include <sys/time.h>

#include <libzutil.h>

/*
 * A vdev is named by the wedge or disk it lives on, and there is no
 * partition suffix to add or take away.
 */

/*
 * Note: The caller must free the returned string.
 */
char *
zfs_strip_partition(const char *dev)
{
	return (strdup(dev));
}

int
zfs_append_partition(char *path, size_t max_len)
{
	return (strnlen(path, max_len));
}

/*
 * Strip the path from a device name: only a leading "/dev/", if the
 * name has one.
 */
const char *
zfs_strip_path(const char *path)
{
	if (strncmp(path, _PATH_DEV, sizeof (_PATH_DEV) - 1) == 0)
		return (path + sizeof (_PATH_DEV) - 1);
	else
		return (path);
}

char *
zfs_get_underlying_path(const char *dev_name)
{

	if (dev_name == NULL)
		return (NULL);

	return (realpath(dev_name, NULL));
}

/*
 * A disk's raw partition covers all of it; a dk(4) wedge, another
 * partition, or a file is not a whole disk.
 */
boolean_t
zfs_dev_is_whole_disk(const char *dev_name)
{
	struct stat st;

	if (stat(dev_name, &st) != 0 ||
	    !(S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)))
		return (B_FALSE);
	if (strncmp(zfs_strip_path(dev_name), "dk", 2) == 0)
		return (B_FALSE);
	return (DISKPART(st.st_rdev) == RAW_PART ? B_TRUE : B_FALSE);
}

int
zpool_label_disk_wait(const char *path, int timeout_ms)
{
	int settle_ms = 50;
	long sleep_ms = 10;
	hrtime_t start, settle;
	struct stat64 statbuf;

	start = gethrtime();
	settle = 0;

	do {
		errno = 0;
		if ((stat64(path, &statbuf) == 0) && (errno == 0)) {
			if (settle == 0)
				settle = gethrtime();
			else if (NSEC2MSEC(gethrtime() - settle) >= settle_ms)
				return (0);
		} else if (errno != ENOENT) {
			return (errno);
		}

		usleep(sleep_ms * MILLISEC);
	} while (NSEC2MSEC(gethrtime() - start) < timeout_ms);

	return (ENODEV);
}

boolean_t
is_mpath_whole_disk(const char *path)
{
	(void) path;
	return (B_FALSE);
}
