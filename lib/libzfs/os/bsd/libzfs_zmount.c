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

#include <sys/param.h>
#include <sys/mount.h>
#include <sys/mntent.h>
#include <sys/mnttab.h>
#include <sys/errno.h>

#include <assert.h>
#include <string.h>

#include <libzfs.h>

#include "../../libzfs_impl.h"

/*
 * mount(2) takes the file system its own argument struct, rather than
 * a string of options as nmount(2) does on FreeBSD.  This is the one
 * NetBSD's ZFS has always used, and the kernel side declares its size
 * as vfs_min_mount_data, so the two have to agree.
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

int
do_mount(zfs_handle_t *zhp, const char *mntpt, const char *opts, int flags)
{
	struct zfs_args za;
	const char *spec = zfs_get_name(zhp);

	assert(spec != NULL);
	assert(mntpt != NULL);
	assert(opts != NULL);

	memset(&za, 0, sizeof (za));
	(void) strlcpy(za.fspec, spec, sizeof (za.fspec));
	(void) strlcpy(za.optptr, opts, sizeof (za.optptr));
	za.optlen = (int)strlen(za.optptr);
	za.mflag = flags;

	if (mount(MNTTYPE_ZFS, mntpt, flags & MS_RDONLY ? MNT_RDONLY : 0,
	    &za, sizeof (za)) == -1)
		return (errno);
	return (0);
}

int
do_unmount(zfs_handle_t *zhp, const char *mntpt, int flags)
{
	(void) zhp;

	/*
	 * MS_CRYPT and MS_OVERLAY are libzfs's own; unmount(2) must not
	 * see them.  MS_FORCE is a real one and stays.
	 */
	flags &= ~(MS_CRYPT | MS_OVERLAY);

	if (unmount(mntpt, flags) == -1)
		return (errno);
	return (0);
}

int
zfs_mount_setattr(zfs_handle_t *zhp, uint32_t nspflags)
{
	(void) nspflags;
	return (zfs_mount(zhp, MNTOPT_REMOUNT, 0));
}

int
zfs_mount_delegation_check(void)
{
	return (0);
}

/* Called from the tail end of zpool_disable_datasets() */
void
zpool_disable_datasets_os(zpool_handle_t *zhp, boolean_t force)
{
	(void) zhp, (void) force;
}

/* Called from the tail end of zfs_unmount() */
void
zpool_disable_volume_os(const char *name)
{
	(void) name;
}
