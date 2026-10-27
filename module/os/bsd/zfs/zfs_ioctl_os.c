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
#include <sys/systm.h>
#include <sys/mount.h>
#include <sys/zfs_context.h>
#include <sys/zfs_ioctl.h>
#include <sys/zfs_vfsops.h>
#include <sys/zfs_ioctl_impl.h>

/*
 * Hold a mounted file system's mount busy, so that it cannot be
 * unmounted under the ioctl; vfs_busy() fails once an unmount has
 * begun.
 */
int
zfs_vfs_ref(zfsvfs_t **zfvp)
{

	if (*zfvp == NULL)
		return (SET_ERROR(ESRCH));
	if (vfs_busy((*zfvp)->z_vfs) != 0) {
		*zfvp = NULL;
		return (SET_ERROR(ESRCH));
	}
	return (0);
}

boolean_t
zfs_vfs_held(zfsvfs_t *zfsvfs)
{

	return (zfsvfs->z_vfs != NULL);
}

void
zfs_vfs_rele(zfsvfs_t *zfsvfs)
{

	vfs_unbusy(zfsvfs->z_vfs);
}

/* Refresh the statistics df(1) reads from the mount. */
void
zfs_ioctl_update_mount_cache(const char *dsname)
{
	zfsvfs_t *zfsvfs;

	if (getzfsvfs(dsname, &zfsvfs) == 0) {
		struct mount *mp = zfsvfs->z_vfs;

		(void) VFS_STATVFS(mp, &mp->mnt_stat);
		zfs_vfs_rele(zfsvfs);
	}
}

/*
 * The largest packed nvlist an ioctl may pass in, unless the tunable
 * says otherwise: a quarter of memory, and no more than 128MiB, as on
 * Linux.  It is copied into kernel memory whole.
 */
uint64_t
zfs_max_nvlist_src_size_os(void)
{

	if (zfs_max_nvlist_src_size != 0)
		return (zfs_max_nvlist_src_size);
	return (MIN(ptob((uint64_t)physmem) / 4, 128 * 1024 * 1024));
}

/* NetBSD has no ioctls of its own beyond the common ones. */
void
zfs_ioctl_init_os(void)
{
}
