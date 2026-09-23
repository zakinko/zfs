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

#include <sys/mntent.h>

#include <libintl.h>
#include <paths.h>
#include <stdio.h>
#include <string.h>

#include <sys/zfs_context.h>

#include "zpool_util.h"

int
check_device(const char *name, boolean_t force, boolean_t isspare,
    boolean_t iswholedisk)
{
	(void) iswholedisk;
	char path[MAXPATHLEN];

	if (strncmp(name, _PATH_DEV, sizeof (_PATH_DEV) - 1) != 0)
		snprintf(path, sizeof (path), "%s%s", _PATH_DEV, name);
	else
		strlcpy(path, name, sizeof (path));

	return (check_file(path, force, isspare));
}

boolean_t
check_sector_size_database(char *path, int *sector_size)
{
	(void) path, (void) sector_size;
	return (0);
}

/*
 * The boot loader cannot read ZFS here, so a root pool is reached
 * through a kernel, modules and a ramdisk kept on a separate file
 * system.  Enabling features the modules over there do not know about
 * leaves a pool they can no longer import.
 */
void
after_zpool_upgrade(zpool_handle_t *zhp)
{
	char bootfs[ZPOOL_MAXPROPLEN];

	if (zpool_get_prop(zhp, ZPOOL_PROP_BOOTFS, bootfs,
	    sizeof (bootfs), NULL, B_FALSE) == 0 &&
	    strcmp(bootfs, "-") != 0) {
		(void) printf(gettext("Pool '%s' has the bootfs property "
		    "set; the kernel and the zfs module on the boot\n"
		    "file system have to be new enough to import it.\n"),
		    zpool_get_name(zhp));
	}
}

int
check_file(const char *file, boolean_t force, boolean_t isspare)
{
	return (check_file_generic(file, force, isspare));
}

int
zpool_power_current_state(zpool_handle_t *zhp, char *vdev)
{
	(void) zhp, (void) vdev;

	/* No enclosure slot power control here. */
	return (-1);
}

int
zpool_power(zpool_handle_t *zhp, char *vdev, boolean_t turn_on)
{
	(void) zhp, (void) vdev, (void) turn_on;

	return (ENOTSUP);
}
