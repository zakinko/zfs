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

/*
 * The .zfs directory.
 *
 * Not yet: on NetBSD there is no way yet for a lookup to mount a
 * snapshot under .zfs/snapshot, which the directory is for, so no file
 * system has one and z_ctldir stays NULL; zfs_has_ctldir() is false
 * and the root directory shows no .zfs.  A snapshot is reached by
 * mounting it, mount -t zfs pool/fs@snap, which works as for any other
 * dataset, and what follows unmounts such a mount when the snapshot
 * goes away.
 */

#include <sys/zfs_context.h>
#include <sys/mount.h>
#include <sys/zfs_ctldir.h>
#include <sys/zfs_ioctl.h>
#include <sys/zfs_vfsops.h>
#include <sys/dsl_pool.h>
#include <sys/dmu_objset.h>

void
zfsctl_init(void)
{
}

void
zfsctl_fini(void)
{
}

void
zfsctl_create(zfsvfs_t *zfsvfs)
{

	ASSERT0P(zfsvfs->z_ctldir);
}

void
zfsctl_destroy(zfsvfs_t *zfsvfs)
{

	zfsvfs->z_ctldir = NULL;
}

boolean_t
zfsctl_is_node(vnode_t *vp)
{

	(void) vp;
	return (B_FALSE);
}

int
zfsctl_root(zfsvfs_t *zfsvfs, int flags, vnode_t **vpp)
{

	(void) zfsvfs, (void) flags, (void) vpp;
	return (SET_ERROR(ENOENT));
}

/* Nothing is mounted under a .zfs that is not there. */
int
zfsctl_umount_snapshots(vfs_t *vfsp, int fflags, cred_t *cr)
{

	(void) vfsp, (void) fflags, (void) cr;
	return (0);
}

int
zfsctl_lookup_objset(vfs_t *vfsp, uint64_t objsetid, zfsvfs_t **zfsvfsp)
{

	(void) vfsp, (void) objsetid, (void) zfsvfsp;
	return (SET_ERROR(ENOENT));
}

/*
 * A snapshot being destroyed or renamed is unmounted first, wherever
 * it is mounted.
 */
int
zfsctl_snapshot_unmount(const char *snapname)
{
	zfsvfs_t *zfsvfs = NULL;
	vfs_t *vfsp;

	if (strchr(snapname, '@') == NULL)
		return (0);
	if (getzfsvfs(snapname, &zfsvfs) != 0)
		return (0);
	vfsp = zfsvfs->z_vfs;
	ASSERT(!dsl_pool_config_held(dmu_objset_pool(zfsvfs->z_os)));
	vfs_ref(vfsp);
	vfs_unbusy(vfsp);
	return (dounmount(vfsp, MNT_FORCE, curlwp));
}
