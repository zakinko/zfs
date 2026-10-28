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
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2011 Pawel Jakub Dawidek <pawel@dawidek.net>.
 * All rights reserved.
 * Copyright (c) 2012, 2015 by Delphix. All rights reserved.
 * Copyright (c) 2014 Integros [integros.com]
 * Copyright 2016 Nexenta Systems, Inc. All rights reserved.
 */

/* Portions Copyright 2010 Robert Milkowski */

/*
 * The ZFS file system's struct vfsops, for NetBSD.  The property
 * callbacks, the zfsvfs_t's life and its suspension around a rollback
 * or a receive are FreeBSD's, which are the system's own business only
 * in the mount flags they set; the rest is written for NetBSD's VFS.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/sysmacros.h>
#include <sys/kmem.h>
#include <sys/acl.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/cmn_err.h>
#include <sys/zfs_znode.h>
#include <sys/zfs_vnops.h>
#include <sys/zfs_dir.h>
#include <sys/zil.h>
#include <sys/fs/zfs.h>
#include <sys/dmu.h>
#include <sys/dsl_prop.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_deleg.h>
#include <sys/spa.h>
#include <sys/zap.h>
#include <sys/sa.h>
#include <sys/sa_impl.h>
#include <sys/policy.h>
#include <sys/atomic.h>
#include <sys/zfs_ioctl.h>
#include <sys/zfs_ctldir.h>
#include <sys/zfs_fuid.h>
#include <sys/sunddi.h>
#include <sys/dmu_objset.h>
#include <sys/dsl_dir.h>
#include <sys/vdev_impl.h>
#include <sys/zfs_quota.h>
#include <sys/zfs_mount_args.h>
#include <sys/mntent.h>
#include <miscfs/genfs/genfs.h>
#include "zfs_comutil.h"

int zfs_super_owner;
ZFS_MODULE_PARAM(zfs, zfs_, super_owner, INT, ZMOD_RW,
	"Allow a dataset's owner to administer it as the superuser can");

static uint32_t zfs_active_fs_count = 0;

extern int zfs_loadvnode(struct mount *, vnode_t *, const void *, size_t,
    const void **);
extern int zfs_newvnode(struct mount *, vnode_t *, vnode_t *, vattr_t *,
    cred_t *, void *, size_t *, const void **);
extern const struct vnodeopv_desc zfs_vnodeop_opv_desc;
extern const struct vnodeopv_desc zfs_fifoop_opv_desc;
extern const struct vnodeopv_desc zfs_specop_opv_desc;
extern const struct vnodeopv_desc zfs_sfsop_opv_desc;

/*
 * The options given at mount time, as zfs(8) passes them and as the
 * mount flags say, which a temporary property is read from.  NetBSD
 * shows a mount's options from its flags, which the property callbacks
 * set themselves, so there is nothing else to record.
 */
int
vfs_optionisset(const vfs_t *vfsp, const char *opt, char **argp)
{
	const zfsvfs_t *zfsvfs = vfsp->mnt_data;
	const char *p, *q;
	size_t len = strlen(opt);

	if (argp != NULL)
		*argp = NULL;
	if (zfsvfs == NULL || zfsvfs->z_mntopts == NULL)
		return (0);
	for (p = zfsvfs->z_mntopts; *p != '\0'; p = (*q == ',') ? q + 1 : q) {
		for (q = p; *q != '\0' && *q != ','; q++)
			continue;
		if ((size_t)(q - p) == len && strncmp(p, opt, len) == 0)
			return (1);
	}
	return (0);
}

void
vfs_setmntopt(vfs_t *vfsp, const char *name, const char *arg, int flags)
{

	(void) vfsp, (void) name, (void) arg, (void) flags;
}

void
vfs_clearmntopt(vfs_t *vfsp, const char *name)
{

	(void) vfsp, (void) name;
}

/*
 * The string vfs_optionisset() reads: the options zfs(8) passed, and
 * those mount(8) gave as flags.
 */
static char *
zfs_mntopts_make(const char *opts, int mflags)
{
	static const struct {
		int		flag;
		const char	*opt;
	} flagopts[] = {
		{ MNT_RDONLY,	MNTOPT_RO },
		{ MNT_NOSUID,	MNTOPT_NOSETUID },
		{ MNT_NOEXEC,	MNTOPT_NOEXEC },
		{ MNT_NODEV,	MNTOPT_NODEVICES },
		{ MNT_NOATIME,	MNTOPT_NOATIME },
	};
	char *s;
	size_t i;

	s = kmem_zalloc(MAXPATHLEN, KM_SLEEP);
	(void) strlcpy(s, opts, MAXPATHLEN);
	for (i = 0; i < ARRAY_SIZE(flagopts); i++) {
		if ((mflags & flagopts[i].flag) == 0)
			continue;
		if (s[0] != '\0')
			(void) strlcat(s, ",", MAXPATHLEN);
		(void) strlcat(s, flagopts[i].opt, MAXPATHLEN);
	}
	return (s);
}

static void
zfs_mntopts_free(zfsvfs_t *zfsvfs)
{

	if (zfsvfs->z_mntopts != NULL) {
		kmem_free(zfsvfs->z_mntopts, MAXPATHLEN);
		zfsvfs->z_mntopts = NULL;
	}
}

int
zfs_get_temporary_prop(dsl_dataset_t *ds, zfs_prop_t zfs_prop, uint64_t *val,
    char *setpoint)
{
	int error;
	zfsvfs_t *zfvp;
	vfs_t *vfsp;
	objset_t *os;
	uint64_t tmp = *val;

	error = dmu_objset_from_ds(ds, &os);
	if (error != 0)
		return (error);

	error = getzfsvfs_impl(os, &zfvp);
	if (error != 0)
		return (error);
	if (zfvp == NULL)
		return (ENOENT);
	vfsp = zfvp->z_vfs;
	switch (zfs_prop) {
	case ZFS_PROP_ATIME:
		if (vfs_optionisset(vfsp, MNTOPT_NOATIME, NULL))
			tmp = 0;
		if (vfs_optionisset(vfsp, MNTOPT_ATIME, NULL))
			tmp = 1;
		break;
	case ZFS_PROP_DEVICES:
		if (vfs_optionisset(vfsp, MNTOPT_NODEVICES, NULL))
			tmp = 0;
		if (vfs_optionisset(vfsp, MNTOPT_DEVICES, NULL))
			tmp = 1;
		break;
	case ZFS_PROP_EXEC:
		if (vfs_optionisset(vfsp, MNTOPT_NOEXEC, NULL))
			tmp = 0;
		if (vfs_optionisset(vfsp, MNTOPT_EXEC, NULL))
			tmp = 1;
		break;
	case ZFS_PROP_SETUID:
		if (vfs_optionisset(vfsp, MNTOPT_NOSETUID, NULL))
			tmp = 0;
		if (vfs_optionisset(vfsp, MNTOPT_SETUID, NULL))
			tmp = 1;
		break;
	case ZFS_PROP_READONLY:
		if (vfs_optionisset(vfsp, MNTOPT_RW, NULL))
			tmp = 0;
		if (vfs_optionisset(vfsp, MNTOPT_RO, NULL))
			tmp = 1;
		break;
	case ZFS_PROP_XATTR:
		if (zfvp->z_flags & ZSB_XATTR)
			tmp = zfvp->z_xattr;
		break;
	case ZFS_PROP_NBMAND:
		if (vfs_optionisset(vfsp, MNTOPT_NONBMAND, NULL))
			tmp = 0;
		if (vfs_optionisset(vfsp, MNTOPT_NBMAND, NULL))
			tmp = 1;
		break;
	default:
		vfs_unbusy(vfsp);
		return (ENOENT);
	}

	vfs_unbusy(vfsp);
	if (tmp != *val) {
		if (setpoint)
			(void) strcpy(setpoint, "temporary");
		*val = tmp;
	}
	return (0);
}

boolean_t
zfs_is_readonly(zfsvfs_t *zfsvfs)
{
	return (!!(zfsvfs->z_vfs->vfs_flag & VFS_RDONLY));
}

/*
 * sync(2) asks every file system with MNT_WAIT, and the syncer with
 * MNT_LAZY every few seconds; ZFS commits its data on its own schedule,
 * so only a sync that waits is acted on.
 */
static int
zfs_sync(struct mount *mp, int waitfor, kauth_cred_t cr)
{
	zfsvfs_t *zfsvfs = mp->mnt_data;
	dsl_pool_t *dp;
	int error;

	(void) cr;
	if (panicstr != NULL || waitfor == MNT_LAZY)
		return (0);
	if ((error = zfs_enter(zfsvfs, FTAG)) != 0)
		return (error);
	dp = dmu_objset_pool(zfsvfs->z_os);
	if (doing_shutdown && spa_suspended(dp->dp_spa)) {
		zfs_exit(zfsvfs, FTAG);
		return (0);
	}
	if (zfsvfs->z_log != NULL)
		error = zil_commit(zfsvfs->z_log, 0);
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

static void
atime_changed_cb(void *arg, uint64_t newval)
{
	zfsvfs_t *zfsvfs = arg;

	if (newval == TRUE) {
		zfsvfs->z_atime = TRUE;
		zfsvfs->z_vfs->vfs_flag &= ~MNT_NOATIME;
		vfs_clearmntopt(zfsvfs->z_vfs, MNTOPT_NOATIME);
		vfs_setmntopt(zfsvfs->z_vfs, MNTOPT_ATIME, NULL, 0);
	} else {
		zfsvfs->z_atime = FALSE;
		zfsvfs->z_vfs->vfs_flag |= MNT_NOATIME;
		vfs_clearmntopt(zfsvfs->z_vfs, MNTOPT_ATIME);
		vfs_setmntopt(zfsvfs->z_vfs, MNTOPT_NOATIME, NULL, 0);
	}
}

static void
relatime_changed_cb(void *arg, uint64_t newval)
{
	((zfsvfs_t *)arg)->z_relatime = (newval != 0);
}

static void
xattr_changed_cb(void *arg, uint64_t newval)
{
	zfsvfs_t *zfsvfs = arg;

	if (newval == ZFS_XATTR_OFF) {
		zfsvfs->z_flags &= ~ZSB_XATTR;
	} else {
		zfsvfs->z_flags |= ZSB_XATTR;

		if (newval == ZFS_XATTR_SA)
			zfsvfs->z_xattr_sa = B_TRUE;
		else
			zfsvfs->z_xattr_sa = B_FALSE;
	}
}

static void
blksz_changed_cb(void *arg, uint64_t newval)
{
	zfsvfs_t *zfsvfs = arg;
	ASSERT3U(newval, <=, spa_maxblocksize(dmu_objset_spa(zfsvfs->z_os)));
	ASSERT3U(newval, >=, SPA_MINBLOCKSIZE);
	ASSERT(ISP2(newval));

	zfsvfs->z_max_blksz = newval;
	zfsvfs->z_vfs->mnt_stat.f_iosize = newval;
}

static void
readonly_changed_cb(void *arg, uint64_t newval)
{
	zfsvfs_t *zfsvfs = arg;

	if (newval) {
		/* XXX locking on vfs_flag? */
		zfsvfs->z_vfs->vfs_flag |= VFS_RDONLY;
		vfs_clearmntopt(zfsvfs->z_vfs, MNTOPT_RW);
		vfs_setmntopt(zfsvfs->z_vfs, MNTOPT_RO, NULL, 0);
	} else {
		/* XXX locking on vfs_flag? */
		zfsvfs->z_vfs->vfs_flag &= ~VFS_RDONLY;
		vfs_clearmntopt(zfsvfs->z_vfs, MNTOPT_RO);
		vfs_setmntopt(zfsvfs->z_vfs, MNTOPT_RW, NULL, 0);
	}
}

static void
setuid_changed_cb(void *arg, uint64_t newval)
{
	zfsvfs_t *zfsvfs = arg;

	if (newval == FALSE) {
		zfsvfs->z_vfs->vfs_flag |= VFS_NOSETUID;
		vfs_clearmntopt(zfsvfs->z_vfs, MNTOPT_SETUID);
		vfs_setmntopt(zfsvfs->z_vfs, MNTOPT_NOSETUID, NULL, 0);
	} else {
		zfsvfs->z_vfs->vfs_flag &= ~VFS_NOSETUID;
		vfs_clearmntopt(zfsvfs->z_vfs, MNTOPT_NOSETUID);
		vfs_setmntopt(zfsvfs->z_vfs, MNTOPT_SETUID, NULL, 0);
	}
}

static void
exec_changed_cb(void *arg, uint64_t newval)
{
	zfsvfs_t *zfsvfs = arg;

	if (newval == FALSE) {
		zfsvfs->z_vfs->vfs_flag |= VFS_NOEXEC;
		vfs_clearmntopt(zfsvfs->z_vfs, MNTOPT_EXEC);
		vfs_setmntopt(zfsvfs->z_vfs, MNTOPT_NOEXEC, NULL, 0);
	} else {
		zfsvfs->z_vfs->vfs_flag &= ~VFS_NOEXEC;
		vfs_clearmntopt(zfsvfs->z_vfs, MNTOPT_NOEXEC);
		vfs_setmntopt(zfsvfs->z_vfs, MNTOPT_EXEC, NULL, 0);
	}
}

/*
 * The nbmand mount option can be changed at mount time.
 * We can't allow it to be toggled on live file systems or incorrect
 * behavior may be seen from cifs clients
 *
 * This property isn't registered via dsl_prop_register(), but this callback
 * will be called when a file system is first mounted
 */
static void
nbmand_changed_cb(void *arg, uint64_t newval)
{
	zfsvfs_t *zfsvfs = arg;
	if (newval == FALSE) {
		vfs_clearmntopt(zfsvfs->z_vfs, MNTOPT_NBMAND);
		vfs_setmntopt(zfsvfs->z_vfs, MNTOPT_NONBMAND, NULL, 0);
	} else {
		vfs_clearmntopt(zfsvfs->z_vfs, MNTOPT_NONBMAND);
		vfs_setmntopt(zfsvfs->z_vfs, MNTOPT_NBMAND, NULL, 0);
	}
}

static void
snapdir_changed_cb(void *arg, uint64_t newval)
{
	zfsvfs_t *zfsvfs = arg;

	zfsvfs->z_show_ctldir = newval;
}

static void
acl_mode_changed_cb(void *arg, uint64_t newval)
{
	zfsvfs_t *zfsvfs = arg;

	zfsvfs->z_acl_mode = newval;
}

static void
acl_inherit_changed_cb(void *arg, uint64_t newval)
{
	zfsvfs_t *zfsvfs = arg;

	zfsvfs->z_acl_inherit = newval;
}

static void
acl_type_changed_cb(void *arg, uint64_t newval)
{
	zfsvfs_t *zfsvfs = arg;

	zfsvfs->z_acl_type = newval;
}

static void
longname_changed_cb(void *arg, uint64_t newval)
{
	zfsvfs_t *zfsvfs = arg;

	zfsvfs->z_longname = newval;
}

static int
zfs_register_callbacks(vfs_t *vfsp)
{
	struct dsl_dataset *ds = NULL;
	objset_t *os = NULL;
	zfsvfs_t *zfsvfs = NULL;
	uint64_t nbmand;
	boolean_t readonly = B_FALSE;
	boolean_t do_readonly = B_FALSE;
	boolean_t setuid = B_FALSE;
	boolean_t do_setuid = B_FALSE;
	boolean_t exec = B_FALSE;
	boolean_t do_exec = B_FALSE;
	boolean_t xattr = B_FALSE;
	boolean_t atime = B_FALSE;
	boolean_t do_atime = B_FALSE;
	boolean_t do_xattr = B_FALSE;
	int error = 0;

	ASSERT3P(vfsp, !=, NULL);
	zfsvfs = vfsp->vfs_data;
	ASSERT3P(zfsvfs, !=, NULL);
	os = zfsvfs->z_os;

	/*
	 * This function can be called for a snapshot when we update snapshot's
	 * mount point, which isn't really supported.
	 */
	if (dmu_objset_is_snapshot(os))
		return (EOPNOTSUPP);

	/*
	 * The act of registering our callbacks will destroy any mount
	 * options we may have.  In order to enable temporary overrides
	 * of mount options, we stash away the current values and
	 * restore them after we register the callbacks.
	 */
	if (vfs_optionisset(vfsp, MNTOPT_RO, NULL) ||
	    !spa_writeable(dmu_objset_spa(os))) {
		readonly = B_TRUE;
		do_readonly = B_TRUE;
	} else if (vfs_optionisset(vfsp, MNTOPT_RW, NULL)) {
		readonly = B_FALSE;
		do_readonly = B_TRUE;
	}
	if (vfs_optionisset(vfsp, MNTOPT_NOSETUID, NULL)) {
		setuid = B_FALSE;
		do_setuid = B_TRUE;
	} else if (vfs_optionisset(vfsp, MNTOPT_SETUID, NULL)) {
		setuid = B_TRUE;
		do_setuid = B_TRUE;
	}
	if (vfs_optionisset(vfsp, MNTOPT_NOEXEC, NULL)) {
		exec = B_FALSE;
		do_exec = B_TRUE;
	} else if (vfs_optionisset(vfsp, MNTOPT_EXEC, NULL)) {
		exec = B_TRUE;
		do_exec = B_TRUE;
	}
	if (vfs_optionisset(vfsp, MNTOPT_NOXATTR, NULL)) {
		zfsvfs->z_xattr = xattr = ZFS_XATTR_OFF;
		do_xattr = B_TRUE;
	} else if (vfs_optionisset(vfsp, MNTOPT_XATTR, NULL)) {
		zfsvfs->z_xattr = xattr = ZFS_XATTR_DIR;
		do_xattr = B_TRUE;
	} else if (vfs_optionisset(vfsp, MNTOPT_DIRXATTR, NULL)) {
		zfsvfs->z_xattr = xattr = ZFS_XATTR_DIR;
		do_xattr = B_TRUE;
	} else if (vfs_optionisset(vfsp, MNTOPT_SAXATTR, NULL)) {
		zfsvfs->z_xattr = xattr = ZFS_XATTR_SA;
		do_xattr = B_TRUE;
	}
	if (vfs_optionisset(vfsp, MNTOPT_NOATIME, NULL)) {
		atime = B_FALSE;
		do_atime = B_TRUE;
	} else if (vfs_optionisset(vfsp, MNTOPT_ATIME, NULL)) {
		atime = B_TRUE;
		do_atime = B_TRUE;
	}

	/*
	 * We need to enter pool configuration here, so that we can use
	 * dsl_prop_get_int_ds() to handle the special nbmand property below.
	 * dsl_prop_get_integer() can not be used, because it has to acquire
	 * spa_namespace_lock and we can not do that because we already hold
	 * z_teardown_lock.  The problem is that spa_write_cachefile() is called
	 * with spa_namespace_lock held and the function calls ZFS vnode
	 * operations to write the cache file and thus z_teardown_lock is
	 * acquired after spa_namespace_lock.
	 */
	ds = dmu_objset_ds(os);
	dsl_pool_config_enter(dmu_objset_pool(os), FTAG);

	/*
	 * nbmand is a special property.  It can only be changed at
	 * mount time.
	 *
	 * This is weird, but it is documented to only be changeable
	 * at mount time.
	 */
	if (vfs_optionisset(vfsp, MNTOPT_NONBMAND, NULL)) {
		nbmand = B_FALSE;
	} else if (vfs_optionisset(vfsp, MNTOPT_NBMAND, NULL)) {
		nbmand = B_TRUE;
	} else if ((error = dsl_prop_get_int_ds(ds, "nbmand", &nbmand)) != 0) {
		dsl_pool_config_exit(dmu_objset_pool(os), FTAG);
		return (error);
	}

	/*
	 * Register property callbacks.
	 *
	 * It would probably be fine to just check for i/o error from
	 * the first prop_register(), but I guess I like to go
	 * overboard...
	 */
	error = dsl_prop_register(ds,
	    zfs_prop_to_name(ZFS_PROP_ATIME), atime_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    zfs_prop_to_name(ZFS_PROP_RELATIME), relatime_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    zfs_prop_to_name(ZFS_PROP_XATTR), xattr_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    zfs_prop_to_name(ZFS_PROP_RECORDSIZE), blksz_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    zfs_prop_to_name(ZFS_PROP_READONLY), readonly_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    zfs_prop_to_name(ZFS_PROP_SETUID), setuid_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    zfs_prop_to_name(ZFS_PROP_EXEC), exec_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    zfs_prop_to_name(ZFS_PROP_SNAPDIR), snapdir_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    zfs_prop_to_name(ZFS_PROP_ACLTYPE), acl_type_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    zfs_prop_to_name(ZFS_PROP_ACLMODE), acl_mode_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    zfs_prop_to_name(ZFS_PROP_ACLINHERIT), acl_inherit_changed_cb,
	    zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    zfs_prop_to_name(ZFS_PROP_LONGNAME), longname_changed_cb, zfsvfs);
	dsl_pool_config_exit(dmu_objset_pool(os), FTAG);
	if (error)
		goto unregister;

	/*
	 * Invoke our callbacks to restore temporary mount options.
	 */
	if (do_readonly)
		readonly_changed_cb(zfsvfs, readonly);
	if (do_setuid)
		setuid_changed_cb(zfsvfs, setuid);
	if (do_exec)
		exec_changed_cb(zfsvfs, exec);
	if (do_xattr)
		xattr_changed_cb(zfsvfs, xattr);
	if (do_atime)
		atime_changed_cb(zfsvfs, atime);

	nbmand_changed_cb(zfsvfs, nbmand);

	return (0);

unregister:
	dsl_prop_unregister_all(ds, zfsvfs);
	return (error);
}

/*
 * Associate this zfsvfs with the given objset, which must be owned.
 * This will cache a bunch of on-disk state from the objset in the
 * zfsvfs.
 */
static int
zfsvfs_init(zfsvfs_t *zfsvfs, objset_t *os)
{
	int error;
	uint64_t val;

	zfsvfs->z_max_blksz = SPA_OLD_MAXBLOCKSIZE;
	zfsvfs->z_show_ctldir = ZFS_SNAPDIR_VISIBLE;
	zfsvfs->z_os = os;

	error = zfs_get_zplprop(os, ZFS_PROP_VERSION, &zfsvfs->z_version);
	if (error != 0)
		return (error);
	if (zfsvfs->z_version >
	    zfs_zpl_version_map(spa_version(dmu_objset_spa(os)))) {
		(void) printf("Can't mount a version %lld file system "
		    "on a version %lld pool\n. Pool must be upgraded to mount "
		    "this file system.", (u_longlong_t)zfsvfs->z_version,
		    (u_longlong_t)spa_version(dmu_objset_spa(os)));
		return (SET_ERROR(ENOTSUP));
	}
	error = zfs_get_zplprop(os, ZFS_PROP_NORMALIZE, &val);
	if (error != 0)
		return (error);
	zfsvfs->z_norm = (int)val;

	error = zfs_get_zplprop(os, ZFS_PROP_UTF8ONLY, &val);
	if (error != 0)
		return (error);
	zfsvfs->z_utf8 = (val != 0);

	error = zfs_get_zplprop(os, ZFS_PROP_CASE, &val);
	if (error != 0)
		return (error);
	zfsvfs->z_case = (uint_t)val;

	error = zfs_get_zplprop(os, ZFS_PROP_ACLTYPE, &val);
	if (error != 0)
		return (error);
	zfsvfs->z_acl_type = (uint_t)val;

	/*
	 * Fold case on file systems that are always or sometimes case
	 * insensitive.
	 */
	if (zfsvfs->z_case == ZFS_CASE_INSENSITIVE ||
	    zfsvfs->z_case == ZFS_CASE_MIXED)
		zfsvfs->z_norm |= U8_TEXTPREP_TOUPPER;

	zfsvfs->z_use_fuids = USE_FUIDS(zfsvfs->z_version, zfsvfs->z_os);
	zfsvfs->z_use_sa = USE_SA(zfsvfs->z_version, zfsvfs->z_os);

	uint64_t sa_obj = 0;
	if (zfsvfs->z_use_sa) {
		/* should either have both of these objects or none */
		error = zap_lookup(os, MASTER_NODE_OBJ, ZFS_SA_ATTRS, 8, 1,
		    &sa_obj);
		if (error != 0)
			return (error);

		error = zfs_get_zplprop(os, ZFS_PROP_XATTR, &val);
		if (error == 0 && val == ZFS_XATTR_SA)
			zfsvfs->z_xattr_sa = B_TRUE;
	}

	error = zfs_get_zplprop(os, ZFS_PROP_DEFAULTUSERQUOTA,
	    &zfsvfs->z_defaultuserquota);
	if (error != 0)
		return (error);

	error = zfs_get_zplprop(os, ZFS_PROP_DEFAULTGROUPQUOTA,
	    &zfsvfs->z_defaultgroupquota);
	if (error != 0)
		return (error);

	error = zfs_get_zplprop(os, ZFS_PROP_DEFAULTPROJECTQUOTA,
	    &zfsvfs->z_defaultprojectquota);
	if (error != 0)
		return (error);

	error = zfs_get_zplprop(os, ZFS_PROP_DEFAULTUSEROBJQUOTA,
	    &zfsvfs->z_defaultuserobjquota);
	if (error != 0)
		return (error);

	error = zfs_get_zplprop(os, ZFS_PROP_DEFAULTGROUPOBJQUOTA,
	    &zfsvfs->z_defaultgroupobjquota);
	if (error != 0)
		return (error);

	error = zfs_get_zplprop(os, ZFS_PROP_DEFAULTPROJECTOBJQUOTA,
	    &zfsvfs->z_defaultprojectobjquota);
	if (error != 0)
		return (error);

	error = sa_setup(os, sa_obj, zfs_attr_table, ZPL_END,
	    &zfsvfs->z_attr_table);
	if (error != 0)
		return (error);

	if (zfsvfs->z_version >= ZPL_VERSION_SA)
		sa_register_update_callback(os, zfs_sa_upgrade);

	error = zap_lookup(os, MASTER_NODE_OBJ, ZFS_ROOT_OBJ, 8, 1,
	    &zfsvfs->z_root);
	if (error != 0)
		return (error);
	ASSERT3U(zfsvfs->z_root, !=, 0);

	error = zap_lookup(os, MASTER_NODE_OBJ, ZFS_UNLINKED_SET, 8, 1,
	    &zfsvfs->z_unlinkedobj);
	if (error != 0)
		return (error);

	error = zap_lookup(os, MASTER_NODE_OBJ,
	    zfs_userquota_prop_prefixes[ZFS_PROP_USERQUOTA],
	    8, 1, &zfsvfs->z_userquota_obj);
	if (error == ENOENT)
		zfsvfs->z_userquota_obj = 0;
	else if (error != 0)
		return (error);

	error = zap_lookup(os, MASTER_NODE_OBJ,
	    zfs_userquota_prop_prefixes[ZFS_PROP_GROUPQUOTA],
	    8, 1, &zfsvfs->z_groupquota_obj);
	if (error == ENOENT)
		zfsvfs->z_groupquota_obj = 0;
	else if (error != 0)
		return (error);

	error = zap_lookup(os, MASTER_NODE_OBJ,
	    zfs_userquota_prop_prefixes[ZFS_PROP_PROJECTQUOTA],
	    8, 1, &zfsvfs->z_projectquota_obj);
	if (error == ENOENT)
		zfsvfs->z_projectquota_obj = 0;
	else if (error != 0)
		return (error);

	error = zap_lookup(os, MASTER_NODE_OBJ,
	    zfs_userquota_prop_prefixes[ZFS_PROP_USEROBJQUOTA],
	    8, 1, &zfsvfs->z_userobjquota_obj);
	if (error == ENOENT)
		zfsvfs->z_userobjquota_obj = 0;
	else if (error != 0)
		return (error);

	error = zap_lookup(os, MASTER_NODE_OBJ,
	    zfs_userquota_prop_prefixes[ZFS_PROP_GROUPOBJQUOTA],
	    8, 1, &zfsvfs->z_groupobjquota_obj);
	if (error == ENOENT)
		zfsvfs->z_groupobjquota_obj = 0;
	else if (error != 0)
		return (error);

	error = zap_lookup(os, MASTER_NODE_OBJ,
	    zfs_userquota_prop_prefixes[ZFS_PROP_PROJECTOBJQUOTA],
	    8, 1, &zfsvfs->z_projectobjquota_obj);
	if (error == ENOENT)
		zfsvfs->z_projectobjquota_obj = 0;
	else if (error != 0)
		return (error);

	error = zap_lookup(os, MASTER_NODE_OBJ, ZFS_FUID_TABLES, 8, 1,
	    &zfsvfs->z_fuid_obj);
	if (error == ENOENT)
		zfsvfs->z_fuid_obj = 0;
	else if (error != 0)
		return (error);

	error = zap_lookup(os, MASTER_NODE_OBJ, ZFS_SHARES_DIR, 8, 1,
	    &zfsvfs->z_shares_dir);
	if (error == ENOENT)
		zfsvfs->z_shares_dir = 0;
	else if (error != 0)
		return (error);

	/*
	 * Only use the name cache if we are looking for a
	 * name on a file system that does not require normalization
	 * or case folding.  We can also look there if we happen to be
	 * on a non-normalizing, mixed sensitivity file system IF we
	 * are looking for the exact name (which is always the case on
	 * FreeBSD).
	 */
	zfsvfs->z_use_namecache = !zfsvfs->z_norm ||
	    ((zfsvfs->z_case == ZFS_CASE_MIXED) &&
	    !(zfsvfs->z_norm & ~U8_TEXTPREP_TOUPPER));

	return (0);
}

int
zfsvfs_create(const char *osname, boolean_t readonly, zfsvfs_t **zfvp)
{
	objset_t *os;
	zfsvfs_t *zfsvfs;
	int error;
	boolean_t ro = (readonly || (strchr(osname, '@') != NULL));

	/*
	 * The name is the mount's f_mntfromname, which libzfs looks the
	 * mount up by; one that does not fit could not be found again.
	 */
	if (strlen(osname) >=
	    sizeof (((struct statvfs *)NULL)->f_mntfromname))
		return (SET_ERROR(ENAMETOOLONG));

	zfsvfs = kmem_zalloc(sizeof (zfsvfs_t), KM_SLEEP);

	error = dmu_objset_own(osname, DMU_OST_ZFS, ro, B_TRUE, zfsvfs,
	    &os);
	if (error != 0) {
		kmem_free(zfsvfs, sizeof (zfsvfs_t));
		return (error);
	}

	error = zfsvfs_create_impl(zfvp, zfsvfs, os);

	return (error);
}

int
zfsvfs_create_hold(const char *osname, zfsvfs_t **zfvp)
{
	objset_t *os;
	zfsvfs_t *zfsvfs;
	int error;

	zfsvfs = kmem_zalloc(sizeof (zfsvfs_t), KM_SLEEP);

	error = dmu_objset_hold(osname, zfsvfs, &os);
	if (error != 0) {
		kmem_free(zfsvfs, sizeof (zfsvfs_t));
		return (error);
	}

	if (dmu_objset_type(os) != DMU_OST_ZFS) {
		dmu_objset_rele(os, zfsvfs);
		kmem_free(zfsvfs, sizeof (zfsvfs_t));
		return (EINVAL);
	}

	zfsvfs->z_use_hold = B_TRUE;
	error = zfsvfs_create_impl(zfvp, zfsvfs, os);

	return (error);
}

int
zfsvfs_create_impl(zfsvfs_t **zfvp, zfsvfs_t *zfsvfs, objset_t *os)
{
	int error;

	zfsvfs->z_vfs = NULL;
	zfsvfs->z_parent = zfsvfs;

	mutex_init(&zfsvfs->z_znodes_lock, NULL, MUTEX_DEFAULT, NULL);
	mutex_init(&zfsvfs->z_lock, NULL, MUTEX_DEFAULT, NULL);
	list_create(&zfsvfs->z_all_znodes, sizeof (znode_t),
	    offsetof(znode_t, z_link_node));
	zfsvfs->z_drain_task = TASKQID_INVALID;
	ZFS_TEARDOWN_INIT(zfsvfs);
	ZFS_TEARDOWN_INACTIVE_INIT(zfsvfs);
	rw_init(&zfsvfs->z_fuid_lock, NULL, RW_DEFAULT, NULL);
	for (int i = 0; i != ZFS_OBJ_MTX_SZ; i++)
		mutex_init(&zfsvfs->z_hold_mtx[i], NULL, MUTEX_DEFAULT, NULL);

	error = zfsvfs_init(zfsvfs, os);
	if (error != 0) {
		if (zfsvfs->z_use_hold)
			dmu_objset_rele(os, zfsvfs);
		else
			dmu_objset_disown(os, B_TRUE, zfsvfs);
		*zfvp = NULL;
		kmem_free(zfsvfs, sizeof (zfsvfs_t));
		return (error);
	}

	*zfvp = zfsvfs;
	return (0);
}

static int
zfsvfs_setup(zfsvfs_t *zfsvfs, boolean_t mounting)
{
	int error;

	/*
	 * Check for a bad on-disk format version now since we
	 * lied about owning the dataset readonly before.
	 */
	if (!(zfsvfs->z_vfs->vfs_flag & VFS_RDONLY) &&
	    dmu_objset_incompatible_encryption_version(zfsvfs->z_os))
		return (SET_ERROR(EROFS));

	error = zfs_register_callbacks(zfsvfs->z_vfs);
	if (error)
		return (error);

	/*
	 * If we are not mounting (ie: online recv), then we don't
	 * have to worry about replaying the log as we blocked all
	 * operations out since we closed the ZIL.
	 */
	if (mounting) {
		boolean_t readonly;

		ASSERT0P(zfsvfs->z_kstat.dk_kstats);
		error = dataset_kstats_create(&zfsvfs->z_kstat, zfsvfs->z_os);
		if (error)
			return (error);
		zfsvfs->z_log = zil_open(zfsvfs->z_os, zfs_get_data,
		    &zfsvfs->z_kstat.dk_zil_sums);

		/*
		 * During replay we remove the read only flag to
		 * allow replays to succeed.
		 */
		readonly = zfsvfs->z_vfs->vfs_flag & VFS_RDONLY;
		if (readonly != 0) {
			zfsvfs->z_vfs->vfs_flag &= ~VFS_RDONLY;
		} else {
			dsl_dir_t *dd;
			zap_stats_t zs;

			if (zap_get_stats(zfsvfs->z_os, zfsvfs->z_unlinkedobj,
			    &zs) == 0) {
				dataset_kstats_update_nunlinks_kstat(
				    &zfsvfs->z_kstat, zs.zs_num_entries);
				dprintf_ds(zfsvfs->z_os->os_dsl_dataset,
				    "num_entries in unlinked set: %llu",
				    (u_longlong_t)zs.zs_num_entries);
			}

			zfs_unlinked_drain(zfsvfs);
			dd = zfsvfs->z_os->os_dsl_dataset->ds_dir;
			dd->dd_activity_cancelled = B_FALSE;
		}

		/*
		 * Parse and replay the intent log.
		 *
		 * Because of ziltest, this must be done after
		 * zfs_unlinked_drain().  (Further note: ziltest
		 * doesn't use readonly mounts, where
		 * zfs_unlinked_drain() isn't called.)  This is because
		 * ziltest causes spa_sync() to think it's committed,
		 * but actually it is not, so the intent log contains
		 * many txg's worth of changes.
		 *
		 * In particular, if object N is in the unlinked set in
		 * the last txg to actually sync, then it could be
		 * actually freed in a later txg and then reallocated
		 * in a yet later txg.  This would write a "create
		 * object N" record to the intent log.  Normally, this
		 * would be fine because the spa_sync() would have
		 * written out the fact that object N is free, before
		 * we could write the "create object N" intent log
		 * record.
		 *
		 * But when we are in ziltest mode, we advance the "open
		 * txg" without actually spa_sync()-ing the changes to
		 * disk.  So we would see that object N is still
		 * allocated and in the unlinked set, and there is an
		 * intent log record saying to allocate it.
		 */
		if (spa_writeable(dmu_objset_spa(zfsvfs->z_os))) {
			if (zil_replay_disable) {
				zil_destroy(zfsvfs->z_log, B_FALSE);
			} else {
				boolean_t use_nc = zfsvfs->z_use_namecache;
				zfsvfs->z_use_namecache = B_FALSE;
				zfsvfs->z_replay = B_TRUE;
				zil_replay(zfsvfs->z_os, zfsvfs,
				    zfs_replay_vector);
				zfsvfs->z_replay = B_FALSE;
				zfsvfs->z_use_namecache = use_nc;
			}
		}

		/* restore readonly bit */
		if (readonly != 0)
			zfsvfs->z_vfs->vfs_flag |= VFS_RDONLY;
	} else {
		ASSERT3P(zfsvfs->z_kstat.dk_kstats, !=, NULL);
		zfsvfs->z_log = zil_open(zfsvfs->z_os, zfs_get_data,
		    &zfsvfs->z_kstat.dk_zil_sums);
	}

	/*
	 * Set the objset user_ptr to track its zfsvfs.
	 */
	mutex_enter(&zfsvfs->z_os->os_user_ptr_lock);
	dmu_objset_set_user(zfsvfs->z_os, zfsvfs);
	mutex_exit(&zfsvfs->z_os->os_user_ptr_lock);

	return (0);
}

void
zfsvfs_free(zfsvfs_t *zfsvfs)
{
	int i;

	zfs_fuid_destroy(zfsvfs);
	zfs_mntopts_free(zfsvfs);

	mutex_destroy(&zfsvfs->z_znodes_lock);
	mutex_destroy(&zfsvfs->z_lock);
	list_destroy(&zfsvfs->z_all_znodes);
	ZFS_TEARDOWN_DESTROY(zfsvfs);
	ZFS_TEARDOWN_INACTIVE_DESTROY(zfsvfs);
	rw_destroy(&zfsvfs->z_fuid_lock);
	for (i = 0; i != ZFS_OBJ_MTX_SZ; i++)
		mutex_destroy(&zfsvfs->z_hold_mtx[i]);
	dataset_kstats_destroy(&zfsvfs->z_kstat);
	kmem_free(zfsvfs, sizeof (zfsvfs_t));
}

static void
zfs_set_fuid_feature(zfsvfs_t *zfsvfs)
{
	zfsvfs->z_use_fuids = USE_FUIDS(zfsvfs->z_version, zfsvfs->z_os);
	zfsvfs->z_use_sa = USE_SA(zfsvfs->z_version, zfsvfs->z_os);
}

static void
zfs_unregister_callbacks(zfsvfs_t *zfsvfs)
{
	objset_t *os = zfsvfs->z_os;

	if (!dmu_objset_is_snapshot(os))
		dsl_prop_unregister_all(dmu_objset_ds(os), zfsvfs);
}

int
zfs_domount(struct mount *mp, const char *osname, const char *opts)
{
	uint64_t recordsize, fsid_guid;
	zfsvfs_t *zfsvfs;
	int error;

	error = zfsvfs_create(osname, (mp->mnt_flag & MNT_RDONLY) != 0,
	    &zfsvfs);
	if (error != 0)
		return (error);
	zfsvfs->z_vfs = mp;
	zfsvfs->z_mntopts = zfs_mntopts_make(opts, mp->mnt_flag);

	if ((error = dsl_prop_get_integer(osname, "recordsize", &recordsize,
	    NULL)) != 0)
		goto out;
	mp->mnt_stat.f_bsize = SPA_MINBLOCKSIZE;
	mp->mnt_stat.f_frsize = SPA_MINBLOCKSIZE;
	mp->mnt_stat.f_iosize = recordsize;

	mp->mnt_data = zfsvfs;
	mp->mnt_flag |= MNT_LOCAL | MNT_NFS4ACLS;
	mp->mnt_iflag |= IMNT_MPSAFE | IMNT_DTYPE;
	mp->mnt_stat.f_namemax =
	    zfsvfs->z_longname ? (ZAP_MAXNAMELEN_NEW - 1) : (MAXNAMELEN - 1);
	mp->mnt_fs_bshift = SPA_MINBLOCKSHIFT;
	mp->mnt_dev_bshift = DEV_BSHIFT;

	/*
	 * The fsid is the objset's unique id, which is unique among all
	 * the objsets open on the system (unique_create()), with the file
	 * system type in its low byte, as on illumos.
	 */
	fsid_guid = dmu_objset_fsid_guid(zfsvfs->z_os);
	ASSERT3U((fsid_guid & ~((1ULL << 56) - 1)), ==, 0);
	mp->mnt_stat.f_fsidx.__fsid_val[0] = fsid_guid;
	mp->mnt_stat.f_fsidx.__fsid_val[1] = ((fsid_guid >> 32) << 8) |
	    (makefstype(mp->mnt_op->vfs_name) & 0xFF);
	mp->mnt_stat.f_fsid = mp->mnt_stat.f_fsidx.__fsid_val[0];

	zfs_set_fuid_feature(zfsvfs);

	if (dmu_objset_is_snapshot(zfsvfs->z_os)) {
		uint64_t pval;

		atime_changed_cb(zfsvfs, B_FALSE);
		readonly_changed_cb(zfsvfs, B_TRUE);
		if ((error = dsl_prop_get_integer(osname, "xattr", &pval,
		    NULL)) != 0)
			goto out;
		xattr_changed_cb(zfsvfs, pval);
		if ((error = dsl_prop_get_integer(osname, "acltype", &pval,
		    NULL)) != 0)
			goto out;
		acl_type_changed_cb(zfsvfs, pval);
		zfsvfs->z_issnap = B_TRUE;
		zfsvfs->z_os->os_sync = ZFS_SYNC_DISABLED;

		mutex_enter(&zfsvfs->z_os->os_user_ptr_lock);
		dmu_objset_set_user(zfsvfs->z_os, zfsvfs);
		mutex_exit(&zfsvfs->z_os->os_user_ptr_lock);
	} else {
		if ((error = zfsvfs_setup(zfsvfs, B_TRUE)) != 0)
			goto out;
	}

	if (!zfsvfs->z_issnap)
		zfsctl_create(zfsvfs);
out:
	if (error != 0) {
		mp->mnt_data = NULL;
		dmu_objset_disown(zfsvfs->z_os, B_TRUE, zfsvfs);
		zfsvfs_free(zfsvfs);
	} else {
		atomic_inc_32(&zfs_active_fs_count);
	}
	return (error);
}

static int
zfs_mount(struct mount *mp, const char *path, void *data, size_t *data_len)
{
	struct zfs_args *args = data;
	vnode_t *mvp = mp->mnt_vnodecovered;
	cred_t *cr = kauth_cred_get();
	zfsvfs_t *zfsvfs;
	char *osname, *opts;
	int error;

	if (args == NULL || *data_len < sizeof (*args))
		return (SET_ERROR(EINVAL));

	/* mount -p asks what a file system was mounted with. */
	if (mp->mnt_flag & MNT_GETARGS) {
		zfsvfs = mp->mnt_data;
		if (zfsvfs == NULL)
			return (SET_ERROR(EIO));
		memset(args, 0, sizeof (*args));
		(void) strlcpy(args->fspec, mp->mnt_stat.f_mntfromname,
		    sizeof (args->fspec));
		if (zfsvfs->z_mntopts != NULL)
			(void) strlcpy(args->optptr, zfsvfs->z_mntopts,
			    sizeof (args->optptr));
		*data_len = sizeof (*args);
		return (0);
	}

	osname = kmem_alloc(sizeof (args->fspec), KM_SLEEP);
	opts = kmem_alloc(sizeof (args->optptr), KM_SLEEP);
	(void) strlcpy(osname, args->fspec, sizeof (args->fspec));
	(void) strlcpy(opts, args->optptr, sizeof (args->optptr));

	/*
	 * Without the privilege to mount, the mount is allowed if the
	 * dataset delegates it, and the caller owns the mount point or
	 * may write it.  Such a mount is made nosuid and nodev.
	 */
	error = secpolicy_fs_mount(cr, mvp, mp);
	if (error != 0) {
		if (dsl_deleg_access(osname, ZFS_DELEG_PERM_MOUNT, cr) != 0)
			goto out;
		if ((mp->mnt_flag & MNT_UPDATE) == 0) {
			vattr_t vattr;

			vn_lock(mvp, LK_SHARED | LK_RETRY);
			error = VOP_GETATTR(mvp, &vattr, cr);
			if (error == 0 &&
			    secpolicy_vnode_owner(mvp, cr, vattr.va_uid) != 0)
				error = VOP_ACCESS(mvp, VWRITE, cr);
			VOP_UNLOCK(mvp);
			if (error != 0)
				goto out;
		}
		secpolicy_fs_mount_clearopts(cr, mp);
	} else if (zfs_super_owner &&
	    dsl_deleg_access(osname, ZFS_DELEG_PERM_MOUNT, cr) != ECANCELED) {
		secpolicy_fs_mount_clearopts(cr, mp);
	}

	/*
	 * A remount takes the options it is given as temporary
	 * properties again, with I/O held off while they change.
	 */
	if (mp->mnt_flag & MNT_UPDATE) {
		zfsvfs = mp->mnt_data;
		ZFS_TEARDOWN_ENTER_WRITE(zfsvfs, FTAG);
		zfs_mntopts_free(zfsvfs);
		zfsvfs->z_mntopts = zfs_mntopts_make(opts, mp->mnt_flag);
		zfs_unregister_callbacks(zfsvfs);
		error = zfs_register_callbacks(mp);
		ZFS_TEARDOWN_EXIT(zfsvfs, FTAG);
		goto out;
	}

	if ((error = zfs_domount(mp, osname, opts)) != 0)
		goto out;
	error = set_statvfs_info(path, UIO_USERSPACE, osname, UIO_SYSSPACE,
	    mp->mnt_op->vfs_name, mp, curlwp);
out:
	kmem_free(osname, sizeof (args->fspec));
	kmem_free(opts, sizeof (args->optptr));
	return (error);
}

static int
zfs_statvfs(struct mount *mp, struct statvfs *statp)
{
	zfsvfs_t *zfsvfs = mp->mnt_data;
	uint64_t refdbytes, availbytes, usedobjs, availobjs;
	int error;

	if ((error = zfs_enter(zfsvfs, FTAG)) != 0)
		return (error);
	dmu_objset_space(zfsvfs->z_os, &refdbytes, &availbytes, &usedobjs,
	    &availobjs);

	/*
	 * The pool uses blocks of many sizes; the fragment size reported
	 * is the smallest, and the block size the file system's largest.
	 */
	statp->f_bsize = SPA_MINBLOCKSIZE;
	statp->f_frsize = SPA_MINBLOCKSIZE;
	statp->f_iosize = zfsvfs->z_vfs->mnt_stat.f_iosize;
	statp->f_blocks = (refdbytes + availbytes) >> SPA_MINBLOCKSHIFT;
	statp->f_bfree = availbytes >> SPA_MINBLOCKSHIFT;
	statp->f_bavail = statp->f_bfree;	/* no root reservation */
	statp->f_bresvd = 0;

	/*
	 * Nothing is preallocated for files: f_files is as many as could
	 * fit, each taking at least a block, and f_ffree that less those
	 * in use.
	 */
	statp->f_ffree = MIN(availobjs, statp->f_bfree);
	statp->f_favail = statp->f_ffree;
	statp->f_fresvd = 0;
	statp->f_files = statp->f_ffree + usedobjs;
	statp->f_namemax =
	    zfsvfs->z_longname ? (ZAP_MAXNAMELEN_NEW - 1) : (MAXNAMELEN - 1);
	copy_statvfs_info(statp, mp);
	zfs_exit(zfsvfs, FTAG);
	return (0);
}

static int
zfs_root(struct mount *mp, int lktype, vnode_t **vpp)
{
	zfsvfs_t *zfsvfs = mp->mnt_data;
	znode_t *rootzp;
	int error;

	if ((error = zfs_enter(zfsvfs, FTAG)) != 0)
		return (error);
	error = zfs_zget(zfsvfs, zfsvfs->z_root, &rootzp);
	zfs_exit(zfsvfs, FTAG);
	if (error != 0)
		return (error);
	*vpp = ZTOV(rootzp);
	if ((error = vn_lock(*vpp, lktype)) != 0) {
		vrele(*vpp);
		*vpp = NULL;
	}
	return (error);
}

/*
 * Teardown the zfsvfs::z_os.
 *
 * Note, if 'unmounting' is FALSE, we return with the 'z_teardown_lock'
 * and 'z_teardown_inactive_lock' held.
 */
static int
zfsvfs_teardown(zfsvfs_t *zfsvfs, boolean_t unmounting)
{
	znode_t	*zp;
	dsl_dir_t *dd;

	/*
	 * If someone has not already unmounted this file system,
	 * drain the zrele_taskq to ensure all active references to the
	 * zfsvfs_t have been handled only then can it be safely destroyed.
	 */
	if (zfsvfs->z_os) {
		/*
		 * If we're unmounting we have to wait for the list to
		 * drain completely.
		 *
		 * If we're not unmounting there's no guarantee the list
		 * will drain completely, but zreles run from the taskq
		 * may add the parents of dir-based xattrs to the taskq
		 * so we want to wait for these.
		 *
		 * We can safely check z_all_znodes for being empty because the
		 * VFS has already blocked operations which add to it.
		 */
		int round = 0;
		while (!list_is_empty(&zfsvfs->z_all_znodes)) {
			taskq_wait_outstanding(dsl_pool_zrele_taskq(
			    dmu_objset_pool(zfsvfs->z_os)), 0);
			if (++round > 1 && !unmounting)
				break;
		}
	}
	ZFS_TEARDOWN_ENTER_WRITE(zfsvfs, FTAG);

	/*
	 * What the name cache holds may no longer be true of what is
	 * brought back; the vnodes stay, and are checked against it.
	 */
	if (!unmounting)
		cache_purgevfs(zfsvfs->z_vfs);

	/*
	 * Close the zil. NB: Can't close the zil while zfs_inactive
	 * threads are blocked as zil_close can call zfs_inactive.
	 */
	if (zfsvfs->z_log) {
		zil_close(zfsvfs->z_log);
		zfsvfs->z_log = NULL;
	}

	ZFS_TEARDOWN_INACTIVE_ENTER_WRITE(zfsvfs);

	/*
	 * If we are not unmounting (ie: online recv) and someone already
	 * unmounted this file system while we were doing the switcheroo,
	 * or a reopen of z_os failed then just bail out now.
	 */
	if (!unmounting && (zfsvfs->z_unmounted || zfsvfs->z_os == NULL)) {
		ZFS_TEARDOWN_INACTIVE_EXIT_WRITE(zfsvfs);
		ZFS_TEARDOWN_EXIT(zfsvfs, FTAG);
		return (SET_ERROR(EIO));
	}

	/*
	 * At this point there are no vops active, and any new vops will
	 * fail with EIO since we have z_teardown_lock for writer (only
	 * relevant for forced unmount).
	 *
	 * Release all holds on dbufs.
	 */
	mutex_enter(&zfsvfs->z_znodes_lock);
	for (zp = list_head(&zfsvfs->z_all_znodes); zp != NULL;
	    zp = list_next(&zfsvfs->z_all_znodes, zp)) {
		if (zp->z_sa_hdl != NULL) {
			zfs_znode_dmu_fini(zp);
		}
	}
	mutex_exit(&zfsvfs->z_znodes_lock);

	/*
	 * If we are unmounting, set the unmounted flag and let new vops
	 * unblock.  zfs_inactive will have the unmounted behavior, and all
	 * other vops will fail with EIO.
	 */
	if (unmounting) {
		zfsvfs->z_unmounted = B_TRUE;
		ZFS_TEARDOWN_INACTIVE_EXIT_WRITE(zfsvfs);
		ZFS_TEARDOWN_EXIT(zfsvfs, FTAG);
	}

	/*
	 * z_os will be NULL if there was an error in attempting to reopen
	 * zfsvfs, so just return as the properties had already been
	 * unregistered and cached data had been evicted before.
	 */
	if (zfsvfs->z_os == NULL)
		return (0);

	/*
	 * Unregister properties.
	 */
	zfs_unregister_callbacks(zfsvfs);

	/*
	 * Evict cached data. We must write out any dirty data before
	 * disowning the dataset.
	 */
	objset_t *os = zfsvfs->z_os;
	boolean_t os_dirty = B_FALSE;
	for (int t = 0; t < TXG_SIZE; t++) {
		if (dmu_objset_is_dirty(os, t)) {
			os_dirty = B_TRUE;
			break;
		}
	}
	if (!zfs_is_readonly(zfsvfs) && os_dirty)
		txg_wait_synced(dmu_objset_pool(zfsvfs->z_os), 0);
	dmu_objset_evict_dbufs(zfsvfs->z_os);
	dd = zfsvfs->z_os->os_dsl_dataset->ds_dir;
	dsl_dir_cancel_waiters(dd);

	return (0);
}

/* Wait for a drain of the unlinked set that is still running. */
static void
zfs_unlinked_drain_wait(zfsvfs_t *zfsvfs)
{

	while (atomic_load_64(&zfsvfs->z_drain_task) != TASKQID_INVALID)
		kpause("zfsdrain", false, 1, NULL);
}

static int
zfs_umount(struct mount *mp, int mntflags)
{
	zfsvfs_t *zfsvfs = mp->mnt_data;
	cred_t *cr = kauth_cred_get();
	objset_t *os;
	int error;

	error = secpolicy_fs_unmount(cr, mp);
	if (error != 0 && dsl_deleg_access(mp->mnt_stat.f_mntfromname,
	    ZFS_DELEG_PERM_MOUNT, cr) != 0)
		return (error);

	/* Snapshots mounted under .zfs go first. */
	if (zfsvfs->z_ctldir != NULL &&
	    (error = zfsctl_umount_snapshots(mp, mntflags, cr)) != 0)
		return (error);

	/*
	 * A forced unmount marks the file system unmounted before the
	 * vnodes are flushed, so that no operation starts on one that is
	 * being cleaned out.
	 */
	if (mntflags & MNT_FORCE) {
		ZFS_TEARDOWN_ENTER_WRITE(zfsvfs, FTAG);
		zfsvfs->z_unmounted = B_TRUE;
		ZFS_TEARDOWN_EXIT(zfsvfs, FTAG);
	}
	/*
	 * The intent log is committed while the files it names still have
	 * their vnodes: committing a record looks its file up, and once
	 * the vnodes are flushed that would make new ones for a file
	 * system on its way out.
	 */
	if (!(mntflags & MNT_FORCE) && zfsvfs->z_log != NULL &&
	    (error = zil_commit(zfsvfs->z_log, 0)) != 0)
		return (error);
	if ((error = vflush(mp, NULL, (mntflags & MNT_FORCE) ?
	    FORCECLOSE : 0)) != 0)
		return (error);
	zfs_unlinked_drain_wait(zfsvfs);

	VERIFY0(zfsvfs_teardown(zfsvfs, B_TRUE));
	os = zfsvfs->z_os;
	/* z_os is NULL if reopening it after a receive failed. */
	if (os != NULL) {
		mutex_enter(&os->os_user_ptr_lock);
		dmu_objset_set_user(os, NULL);
		mutex_exit(&os->os_user_ptr_lock);
		dmu_objset_disown(os, B_TRUE, zfsvfs);
	}
	if (zfsvfs->z_ctldir != NULL)
		zfsctl_destroy(zfsvfs);
	mp->mnt_data = NULL;
	zfsvfs_free(zfsvfs);
	atomic_dec_32(&zfs_active_fs_count);
	return (0);
}

static int
zfs_vget(struct mount *mp, ino_t ino, int lktype, vnode_t **vpp)
{
	zfsvfs_t *zfsvfs = mp->mnt_data;
	znode_t *zp;
	int err;

	/*
	 * The .zfs directories are not objects zfs_zget() can find; NFS
	 * falls back to a lookup.
	 */
	if (ino == ZFSCTL_INO_ROOT || ino == ZFSCTL_INO_SNAPDIR ||
	    (zfsvfs->z_shares_dir != 0 && ino == zfsvfs->z_shares_dir))
		return (SET_ERROR(EOPNOTSUPP));
	if ((err = zfs_enter(zfsvfs, FTAG)) != 0)
		return (err);
	err = zfs_zget(zfsvfs, ino, &zp);
	zfs_exit(zfsvfs, FTAG);
	if (err != 0)
		return (err);
	*vpp = ZTOV(zp);
	if ((err = vn_lock(*vpp, lktype)) != 0) {
		vrele(*vpp);
		*vpp = NULL;
	}
	return (err);
}

/*
 * NFS file handles: the object and its generation, in the layout
 * illumos and FreeBSD give them.  A snapshot's file is reached through
 * its parent, and the long form that names the snapshot is not taken
 * until .zfs is.
 */
static int
zfs_fhtovp(struct mount *mp, struct fid *fhp, int lktype, vnode_t **vpp)
{
	zfsvfs_t *zfsvfs = mp->mnt_data;
	zfid_short_t zfid;
	uint64_t object = 0, fid_gen = 0, zp_gen, gen_mask;
	znode_t *zp;
	size_t i;
	int err;

	*vpp = NULL;
	if (fhp->fid_len < offsetof(struct fid, fid_data) + SHORT_FID_LEN)
		return (SET_ERROR(EINVAL));
	memcpy(&zfid.zf_object, fhp->fid_data, SHORT_FID_LEN);
	for (i = 0; i < sizeof (zfid.zf_object); i++)
		object |= ((uint64_t)zfid.zf_object[i]) << (8 * i);
	for (i = 0; i < sizeof (zfid.zf_gen); i++)
		fid_gen |= ((uint64_t)zfid.zf_gen[i]) << (8 * i);
	gen_mask = -1ULL >> (64 - 8 * sizeof (zfid.zf_gen));

	if ((err = zfs_enter(zfsvfs, FTAG)) != 0)
		return (err);
	if (fid_gen == 0 &&
	    (object == ZFSCTL_INO_ROOT || object == ZFSCTL_INO_SNAPDIR)) {
		err = (zfsvfs->z_ctldir != NULL) ?
		    zfsctl_fhtovp(zfsvfs, object, vpp) : SET_ERROR(ESTALE);
		zfs_exit(zfsvfs, FTAG);
		if (err == 0 && (err = vn_lock(*vpp, lktype)) != 0) {
			vrele(*vpp);
			*vpp = NULL;
		}
		return (err);
	}
	if ((err = zfs_zget(zfsvfs, object, &zp)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (err);
	}
	(void) sa_lookup(zp->z_sa_hdl, SA_ZPL_GEN(zfsvfs), &zp_gen,
	    sizeof (uint64_t));
	zp_gen &= gen_mask;
	if (zp_gen == 0)
		zp_gen = 1;
	if (zp->z_unlinked || zp_gen != fid_gen) {
		vrele(ZTOV(zp));
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(ESTALE));
	}
	zfs_exit(zfsvfs, FTAG);
	*vpp = ZTOV(zp);
	if ((err = vn_lock(*vpp, lktype)) != 0) {
		vrele(*vpp);
		*vpp = NULL;
	}
	return (err);
}

static int
zfs_vptofh(vnode_t *vp, struct fid *fhp, size_t *fh_size)
{
	znode_t *zp;
	zfsvfs_t *zfsvfs;
	zfid_short_t zfid;
	uint64_t object, gen;
	size_t need, i;
	int err;

	if (zfsctl_is_node(vp))
		return (zfsctl_vptofh(vp, fhp, fh_size));
	zp = VTOZ(vp);
	zfsvfs = zp->z_zfsvfs;
	object = zp->z_id;
	need = offsetof(struct fid, fid_data) + SHORT_FID_LEN;
	if (*fh_size < need) {
		*fh_size = need;
		return (SET_ERROR(E2BIG));
	}
	if ((err = zfs_enter(zfsvfs, FTAG)) != 0)
		return (err);
	if ((err = sa_lookup(zp->z_sa_hdl, SA_ZPL_GEN(zfsvfs), &gen,
	    sizeof (gen))) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (err);
	}
	zfs_exit(zfsvfs, FTAG);
	for (i = 0; i < sizeof (zfid.zf_object); i++)
		zfid.zf_object[i] = (uint8_t)(object >> (8 * i));
	/* A generation of 0 means .zfs; 1 stands in for it. */
	if (gen == 0)
		gen = 1;
	for (i = 0; i < sizeof (zfid.zf_gen); i++)
		zfid.zf_gen[i] = (uint8_t)(gen >> (8 * i));
	memset(fhp, 0, need);
	fhp->fid_len = need;
	memcpy(fhp->fid_data, zfid.zf_object, SHORT_FID_LEN);
	*fh_size = need;
	return (0);
}

/*
 * Block out VOPs and close zfsvfs_t::z_os
 *
 * Note, if successful, then we return with the 'z_teardown_lock' and
 * 'z_teardown_inactive_lock' write held.  We leave ownership of the underlying
 * dataset and objset intact so that they can be atomically handed off during
 * a subsequent rollback or recv operation and the resume thereafter.
 */
int
zfs_suspend_fs(zfsvfs_t *zfsvfs)
{
	int error;

	if ((error = zfsvfs_teardown(zfsvfs, B_FALSE)) != 0)
		return (error);

	return (0);
}

/*
 * Rebuild SA and release VOPs.  Note that ownership of the underlying dataset
 * is an invariant across any of the operations that can be performed while the
 * filesystem was suspended.  Whether it succeeded or failed, the preconditions
 * are the same: the relevant objset and associated dataset are owned by
 * zfsvfs, held, and long held on entry.
 */
int
zfs_resume_fs(zfsvfs_t *zfsvfs, dsl_dataset_t *ds)
{
	int err;
	znode_t *zp;

	ASSERT(ZFS_TEARDOWN_WRITE_HELD(zfsvfs));
	ASSERT(ZFS_TEARDOWN_INACTIVE_WRITE_HELD(zfsvfs));

	/*
	 * We already own this, so just update the objset_t, as the one we
	 * had before may have been evicted.
	 */
	objset_t *os;
	VERIFY3P(ds->ds_owner, ==, zfsvfs);
	VERIFY(dsl_dataset_long_held(ds));
	dsl_pool_t *dp = spa_get_dsl(dsl_dataset_get_spa(ds));
	dsl_pool_config_enter(dp, FTAG);
	VERIFY0(dmu_objset_from_ds(ds, &os));
	dsl_pool_config_exit(dp, FTAG);

	err = zfsvfs_init(zfsvfs, os);
	if (err != 0)
		goto bail;

	ds->ds_dir->dd_activity_cancelled = B_FALSE;
	VERIFY0(zfsvfs_setup(zfsvfs, B_FALSE));

	zfs_set_fuid_feature(zfsvfs);

	/*
	 * Attempt to re-establish all the active znodes with
	 * their dbufs.  If a zfs_rezget() fails, then we'll let
	 * any potential callers discover that via zfs_enter_verify_zp
	 * when they try to use their znode.
	 */
	mutex_enter(&zfsvfs->z_znodes_lock);
	for (zp = list_head(&zfsvfs->z_all_znodes); zp;
	    zp = list_next(&zfsvfs->z_all_znodes, zp)) {
		(void) zfs_rezget(zp);
	}
	mutex_exit(&zfsvfs->z_znodes_lock);

bail:
	/* release the VOPs */
	ZFS_TEARDOWN_INACTIVE_EXIT_WRITE(zfsvfs);
	ZFS_TEARDOWN_EXIT(zfsvfs, FTAG);

	if (err) {
		/*
		 * Since we couldn't setup the sa framework, try to force
		 * unmount this file system.
		 */
		vfs_ref(zfsvfs->z_vfs);
		(void) dounmount(zfsvfs->z_vfs, MNT_FORCE, curlwp);
	}
	return (err);
}

void
zfs_init(void)
{

	zfsctl_init();
	zfs_znode_init();
	dmu_objset_register_type(DMU_OST_ZFS, zpl_get_file_info);
}

void
zfs_fini(void)
{

	zfsctl_fini();
	zfs_znode_fini();
}

int
zfs_busy(void)
{

	return (zfs_active_fs_count != 0);
}

/*
 * Release VOPs and unmount a suspended filesystem.
 */
int
zfs_end_fs(zfsvfs_t *zfsvfs, dsl_dataset_t *ds)
{
	ASSERT(ZFS_TEARDOWN_WRITE_HELD(zfsvfs));
	ASSERT(ZFS_TEARDOWN_INACTIVE_WRITE_HELD(zfsvfs));

	/*
	 * We already own this, so just hold and rele it to update the
	 * objset_t, as the one we had before may have been evicted.
	 */
	objset_t *os;
	VERIFY3P(ds->ds_owner, ==, zfsvfs);
	VERIFY(dsl_dataset_long_held(ds));
	dsl_pool_t *dp = spa_get_dsl(dsl_dataset_get_spa(ds));
	dsl_pool_config_enter(dp, FTAG);
	VERIFY0(dmu_objset_from_ds(ds, &os));
	dsl_pool_config_exit(dp, FTAG);
	zfsvfs->z_os = os;

	/* release the VOPs */
	ZFS_TEARDOWN_INACTIVE_EXIT_WRITE(zfsvfs);
	ZFS_TEARDOWN_EXIT(zfsvfs, FTAG);

	/*
	 * Try to force unmount this file system.
	 */
	(void) zfs_umount(zfsvfs->z_vfs, 0);
	zfsvfs->z_unmounted = B_TRUE;
	return (0);
}

int
zfs_set_version(zfsvfs_t *zfsvfs, uint64_t newvers)
{
	int error;
	objset_t *os = zfsvfs->z_os;
	dmu_tx_t *tx;

	if (newvers < ZPL_VERSION_INITIAL || newvers > ZPL_VERSION)
		return (SET_ERROR(EINVAL));

	if (newvers < zfsvfs->z_version)
		return (SET_ERROR(EINVAL));

	if (zfs_spa_version_map(newvers) >
	    spa_version(dmu_objset_spa(zfsvfs->z_os)))
		return (SET_ERROR(ENOTSUP));

	tx = dmu_tx_create(os);
	dmu_tx_hold_zap(tx, MASTER_NODE_OBJ, B_FALSE, ZPL_VERSION_STR);
	if (newvers >= ZPL_VERSION_SA && !zfsvfs->z_use_sa) {
		dmu_tx_hold_zap(tx, MASTER_NODE_OBJ, B_TRUE,
		    ZFS_SA_ATTRS);
		dmu_tx_hold_zap(tx, DMU_NEW_OBJECT, FALSE, NULL);
	}
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error) {
		dmu_tx_abort(tx);
		return (error);
	}

	error = zap_update(os, MASTER_NODE_OBJ, ZPL_VERSION_STR,
	    8, 1, &newvers, tx);

	if (error) {
		dmu_tx_commit(tx);
		return (error);
	}

	if (newvers >= ZPL_VERSION_SA && !zfsvfs->z_use_sa) {
		uint64_t sa_obj;

		ASSERT3U(spa_version(dmu_objset_spa(zfsvfs->z_os)), >=,
		    SPA_VERSION_SA);
		sa_obj = zap_create(os, DMU_OT_SA_MASTER_NODE,
		    DMU_OT_NONE, 0, tx);

		error = zap_add(os, MASTER_NODE_OBJ,
		    ZFS_SA_ATTRS, 8, 1, &sa_obj, tx);
		ASSERT0(error);

		VERIFY0(sa_set_sa_object(os, sa_obj));
		sa_register_update_callback(os, zfs_sa_upgrade);
	}

	spa_history_log_internal_ds(dmu_objset_ds(os), "upgrade", tx,
	    "from %ju to %ju", (uintmax_t)zfsvfs->z_version,
	    (uintmax_t)newvers);
	dmu_tx_commit(tx);

	zfsvfs->z_version = newvers;
	os->os_version = newvers;

	zfs_set_fuid_feature(zfsvfs);

	return (0);
}

int
zfs_set_default_quota(zfsvfs_t *zfsvfs, zfs_prop_t prop, uint64_t quota)
{
	int error;
	objset_t *os = zfsvfs->z_os;
	const char *propstr = zfs_prop_to_name(prop);
	dmu_tx_t *tx;

	tx = dmu_tx_create(os);
	dmu_tx_hold_zap(tx, MASTER_NODE_OBJ, B_FALSE, propstr);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error) {
		dmu_tx_abort(tx);
		return (error);
	}

	if (quota == 0) {
		error = zap_remove(os, MASTER_NODE_OBJ, propstr, tx);
		if (error == ENOENT)
			error = 0;
	} else {
		error = zap_update(os, MASTER_NODE_OBJ, propstr, 8, 1,
		    &quota, tx);
	}

	if (error)
		goto out;

	switch (prop) {
	case ZFS_PROP_DEFAULTUSERQUOTA:
		zfsvfs->z_defaultuserquota = quota;
		break;
	case ZFS_PROP_DEFAULTGROUPQUOTA:
		zfsvfs->z_defaultgroupquota = quota;
		break;
	case ZFS_PROP_DEFAULTPROJECTQUOTA:
		zfsvfs->z_defaultprojectquota = quota;
		break;
	case ZFS_PROP_DEFAULTUSEROBJQUOTA:
		zfsvfs->z_defaultuserobjquota = quota;
		break;
	case ZFS_PROP_DEFAULTGROUPOBJQUOTA:
		zfsvfs->z_defaultgroupobjquota = quota;
		break;
	case ZFS_PROP_DEFAULTPROJECTOBJQUOTA:
		zfsvfs->z_defaultprojectobjquota = quota;
		break;
	default:
		break;
	}

out:
	dmu_tx_commit(tx);
	return (error);
}

/*
 * Return true if the corresponding vfs's unmounted flag is set.
 * Otherwise return false.
 * If this function returns true we know VFS unmount has been initiated.
 */
boolean_t
zfs_get_vfs_flag_unmounted(objset_t *os)
{
	zfsvfs_t *zfvp;
	boolean_t unmounted = B_FALSE;

	ASSERT3U(dmu_objset_type(os), ==, DMU_OST_ZFS);

	mutex_enter(&os->os_user_ptr_lock);
	zfvp = dmu_objset_get_user(os);
	if (zfvp != NULL && zfvp->z_vfs != NULL &&
	    (zfvp->z_vfs->mnt_iflag & IMNT_UNMOUNT))
		unmounted = B_TRUE;
	mutex_exit(&os->os_user_ptr_lock);

	return (unmounted);
}

/* A dataset renamed: the mounts of it and of its children follow. */
void
zfsvfs_update_fromname(const char *oldname, const char *newname)
{
	char tmpbuf[MAXPATHLEN];
	struct mount_iterator *iter;
	struct mount *mp;
	char *fromname;
	size_t oldlen;

	oldlen = strlen(oldname);
	mountlist_iterator_init(&iter);
	while ((mp = mountlist_iterator_next(iter)) != NULL) {
		if (strcmp(mp->mnt_stat.f_fstypename, MNTTYPE_ZFS) != 0)
			continue;
		fromname = mp->mnt_stat.f_mntfromname;
		if (strcmp(fromname, oldname) == 0) {
			(void) strlcpy(fromname, newname,
			    sizeof (mp->mnt_stat.f_mntfromname));
			continue;
		}
		if (strncmp(fromname, oldname, oldlen) == 0 &&
		    (fromname[oldlen] == '/' || fromname[oldlen] == '@')) {
			(void) snprintf(tmpbuf, sizeof (tmpbuf), "%s%s",
			    newname, fromname + oldlen);
			(void) strlcpy(fromname, tmpbuf,
			    sizeof (mp->mnt_stat.f_mntfromname));
		}
	}
	mountlist_iterator_destroy(iter);
}

static const struct vnodeopv_desc * const zfs_vnodeop_descs[] = {
	&zfs_vnodeop_opv_desc,
	&zfs_specop_opv_desc,
	&zfs_fifoop_opv_desc,
	&zfs_sfsop_opv_desc,
	NULL,
};

struct vfsops zfs_vfsops = {
	.vfs_name = MOUNT_ZFS,
	.vfs_min_mount_data = sizeof (struct zfs_args),
	.vfs_opv_descs = zfs_vnodeop_descs,
	.vfs_mount = zfs_mount,
	.vfs_unmount = zfs_umount,
	.vfs_root = zfs_root,
	.vfs_statvfs = zfs_statvfs,
	.vfs_sync = zfs_sync,
	.vfs_vget = zfs_vget,
	.vfs_loadvnode = zfs_loadvnode,
	.vfs_newvnode = zfs_newvnode,
	.vfs_fhtovp = zfs_fhtovp,
	.vfs_vptofh = zfs_vptofh,
	.vfs_init = (void (*)(void))nullop,
	.vfs_reinit = (void (*)(void))nullop,
	.vfs_done = (void (*)(void))nullop,
	.vfs_start = (void *)nullop,
	.vfs_quotactl = (void *)eopnotsupp,
	.vfs_mountroot = (void *)eopnotsupp,
	.vfs_snapshot = (void *)eopnotsupp,
	.vfs_extattrctl = vfs_stdextattrctl,
	.vfs_suspendctl = genfs_suspendctl,
	.vfs_renamelock_enter = genfs_renamelock_enter,
	.vfs_renamelock_exit = genfs_renamelock_exit,
	.vfs_fsync = (void *)eopnotsupp,
};
