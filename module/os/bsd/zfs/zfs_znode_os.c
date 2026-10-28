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
 * Copyright (c) 2012, 2014 by Delphix. All rights reserved.
 * Copyright (c) 2014 Integros [integros.com]
 */

/* Portions Copyright 2007 Jeremy Teo */
/* Portions Copyright 2011 Martin Matuska <mm@FreeBSD.org> */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/systm.h>
#include <sys/sysmacros.h>
#include <sys/resource.h>
#include <sys/mntent.h>
#include <sys/u8_textprep.h>
#include <sys/dsl_dataset.h>
#include <sys/vfs.h>
#include <sys/vnode.h>
#include <sys/file.h>
#include <sys/kmem.h>
#include <sys/errno.h>
#include <sys/unistd.h>
#include <sys/atomic.h>
#include <sys/zfs_dir.h>
#include <sys/zfs_acl_impl.h>
#include <sys/zfs_ioctl.h>
#include <sys/zfs_rlock.h>
#include <sys/zfs_fuid.h>
#include <sys/dnode.h>
#include <sys/fs/zfs.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/dmu_tx.h>
#include <sys/zfs_refcount.h>
#include <sys/stat.h>
#include <sys/zap.h>
#include <sys/zfs_ctldir.h>
#include <sys/zfs_znode.h>
#include <sys/sa.h>
#include <sys/zfs_sa.h>
#include <sys/zfs_stat.h>
#include <sys/proc.h>
#include <sys/signalvar.h>
#include <miscfs/genfs/genfs_node.h>
#include <miscfs/specfs/specdev.h>
#include <uvm/uvm_extern.h>

#include "zfs_prop.h"
#include "zfs_comutil.h"

/*
 * Define ZNODE_STATS to turn on statistic gathering. By default, it is only
 * turned on when DEBUG is also defined.
 */
#ifdef	ZFS_DEBUG
#define	ZNODE_STATS
#endif	/* DEBUG */

#ifdef	ZNODE_STATS
#define	ZNODE_STAT_ADD(stat)			((stat)++)
#else
#define	ZNODE_STAT_ADD(stat)			/* nothing */
#endif	/* ZNODE_STATS */

static kmem_cache_t *znode_cache = NULL;

extern int (**zfs_vnodeop_p)(void *);
extern int (**zfs_fifoop_p)(void *);
extern int (**zfs_specop_p)(void *);
extern const struct genfs_ops zfs_genfsops;


/*
 * This callback is invoked when acquiring a RL_WRITER or RL_APPEND lock on
 * z_rangelock. It will modify the offset and length of the lock to reflect
 * znode-specific information, and convert RL_APPEND to RL_WRITER.  This is
 * called with the rangelock_t's rl_lock held, which avoids races.
 */
static void
zfs_rangelock_cb(zfs_locked_range_t *new, void *arg)
{
	znode_t *zp = arg;

	/*
	 * If in append mode, convert to writer and lock starting at the
	 * current end of file.
	 */
	if (new->lr_type == RL_APPEND) {
		new->lr_offset = zp->z_size;
		new->lr_type = RL_WRITER;
	}

	/*
	 * If we might grow the block size then lock the whole file range.
	 * NB: this test should match the check in zfs_grow_blocksize
	 */
	uint64_t end_size = MAX(zp->z_size, new->lr_offset + new->lr_length);
	if (zp->z_size <= zp->z_blksz && end_size > zp->z_blksz &&
	    (!ISP2(zp->z_blksz) || zp->z_blksz < ZTOZSB(zp)->z_max_blksz)) {
		new->lr_offset = 0;
		new->lr_length = UINT64_MAX;
	}
}

static int
zfs_znode_cache_constructor(void *buf, void *arg, int kmflags)
{
	znode_t *zp = buf;

	POINTER_INVALIDATE(&zp->z_zfsvfs);

	list_link_init(&zp->z_link_node);

	mutex_init(&zp->z_lock, NULL, MUTEX_DEFAULT, NULL);
	mutex_init(&zp->z_acl_lock, NULL, MUTEX_DEFAULT, NULL);
	rw_init(&zp->z_xattr_lock, NULL, RW_DEFAULT, NULL);

	zfs_rangelock_init(&zp->z_rangelock, zfs_rangelock_cb, zp);

	zp->z_acl_cached = NULL;
	zp->z_xattr_cached = NULL;
	zp->z_xattr_parent = 0;
	zp->z_vnode = NULL;
	zp->z_has_seq = B_FALSE;

	return (0);
}

static void
zfs_znode_cache_destructor(void *buf, void *arg)
{
	(void) arg;
	znode_t *zp = buf;

	ASSERT(!POINTER_IS_VALID(zp->z_zfsvfs));
	ASSERT0P(zp->z_vnode);
	ASSERT(!list_link_active(&zp->z_link_node));
	mutex_destroy(&zp->z_lock);
	mutex_destroy(&zp->z_acl_lock);
	rw_destroy(&zp->z_xattr_lock);
	zfs_rangelock_fini(&zp->z_rangelock);

	ASSERT0P(zp->z_acl_cached);
	ASSERT0P(zp->z_xattr_cached);
}


void
zfs_znode_init(void)
{
	/*
	 * Initialize zcache
	 */
	ASSERT0P(znode_cache);
	znode_cache = kmem_cache_create("zfs_znode_cache",
	    sizeof (znode_t), 0, zfs_znode_cache_constructor,
	    zfs_znode_cache_destructor, NULL, NULL, NULL, KMC_RECLAIMABLE);
}

static znode_t *
zfs_znode_alloc_kmem(int flags)
{
	return (kmem_cache_alloc(znode_cache, flags));
}

static void
zfs_znode_free_kmem(znode_t *zp)
{
	if (zp->z_xattr_cached) {
		nvlist_free(zp->z_xattr_cached);
		zp->z_xattr_cached = NULL;
	}
	kmem_cache_free(znode_cache, zp);
}

void
zfs_znode_fini(void)
{
	/*
	 * Cleanup zcache
	 */
	if (znode_cache) {
		kmem_cache_destroy(znode_cache);
		znode_cache = NULL;
	}
}


static int
zfs_create_share_dir(zfsvfs_t *zfsvfs, dmu_tx_t *tx)
{
	zfs_acl_ids_t acl_ids;
	vattr_t vattr;
	znode_t *sharezp;
	znode_t *zp;
	int error;

	vattr.va_mask = AT_MODE|AT_UID|AT_GID;
	vattr.va_type = VDIR;
	vattr.va_mode = S_IFDIR|0555;
	vattr.va_uid = crgetuid(kcred);
	vattr.va_gid = crgetgid(kcred);

	sharezp = zfs_znode_alloc_kmem(KM_SLEEP);
	ASSERT(!POINTER_IS_VALID(sharezp->z_zfsvfs));
	sharezp->z_unlinked = 0;
	sharezp->z_atime_dirty = 0;
	sharezp->z_xattr_dir_absent = B_FALSE;
	sharezp->z_zfsvfs = zfsvfs;
	sharezp->z_is_sa = zfsvfs->z_use_sa;
	sharezp->z_pflags = 0;

	VERIFY0(zfs_acl_ids_create(sharezp, IS_ROOT_NODE, &vattr,
	    kcred, NULL, &acl_ids));
	zfs_mknode(sharezp, &vattr, tx, kcred, IS_ROOT_NODE, &zp, &acl_ids);
	ASSERT3P(zp, ==, sharezp);
	POINTER_INVALIDATE(&sharezp->z_zfsvfs);
	error = zap_add(zfsvfs->z_os, MASTER_NODE_OBJ,
	    ZFS_SHARES_DIR, 8, 1, &sharezp->z_id, tx);
	zfsvfs->z_shares_dir = sharezp->z_id;

	zfs_acl_ids_free(&acl_ids);
	sa_handle_destroy(sharezp->z_sa_hdl);
	zfs_znode_free_kmem(sharezp);

	return (error);
}

/*
 * define a couple of values we need available
 * for both 64 and 32 bit environments.
 */
#ifndef NBITSMINOR64
#define	NBITSMINOR64	32
#endif
#ifndef MAXMAJ64
#define	MAXMAJ64	0xffffffffUL
#endif
#ifndef	MAXMIN64
#define	MAXMIN64	0xffffffffUL
#endif

/*
 * Create special expldev for ZFS private use.
 * Can't use standard expldev since it doesn't do
 * what we want.  The standard expldev() takes a
 * dev32_t in LP64 and expands it to a long dev_t.
 * We need an interface that takes a dev32_t in ILP32
 * and expands it to a long dev_t.
 */
static uint64_t
zfs_expldev(dev_t dev)
{
	return (((uint64_t)major(dev) << NBITSMINOR64) | minor(dev));
}
/*
 * Special cmpldev for ZFS private use.
 * Can't use standard cmpldev since it takes
 * a long dev_t and compresses it to dev32_t in
 * LP64.  We need to do a compaction of a long dev_t
 * to a dev32_t in ILP32.
 */
dev_t
zfs_cmpldev(uint64_t dev)
{
	return (makedev((dev >> NBITSMINOR64), (dev & MAXMIN64)));
}

static void
zfs_znode_sa_init(zfsvfs_t *zfsvfs, znode_t *zp,
    dmu_buf_t *db, dmu_object_type_t obj_type, sa_handle_t *sa_hdl)
{
	ASSERT(!POINTER_IS_VALID(zp->z_zfsvfs) || (zfsvfs == zp->z_zfsvfs));
	ASSERT(MUTEX_HELD(ZFS_OBJ_MUTEX(zfsvfs, zp->z_id)));

	ASSERT0P(zp->z_sa_hdl);
	ASSERT0P(zp->z_acl_cached);
	if (sa_hdl == NULL) {
		VERIFY0(sa_handle_get_from_db(zfsvfs->z_os, db, zp,
		    SA_HDL_SHARED, &zp->z_sa_hdl));
	} else {
		zp->z_sa_hdl = sa_hdl;
		sa_set_userp(sa_hdl, zp);
	}

	zp->z_is_sa = (obj_type == DMU_OT_SA) ? B_TRUE : B_FALSE;

	/*
	 * Slap on VROOT if we are the root znode unless we are the root
	 * node of a snapshot mounted under .zfs.
	 */
	if (zp->z_id == zfsvfs->z_root && zfsvfs->z_parent == zfsvfs &&
	    ZTOV(zp) != NULL)
		ZTOV(zp)->v_vflag |= VV_ROOT;
}

void
zfs_znode_dmu_fini(znode_t *zp)
{
	ASSERT(MUTEX_HELD(ZFS_OBJ_MUTEX(zp->z_zfsvfs, zp->z_id)) ||
	    ZFS_TEARDOWN_INACTIVE_WRITE_HELD(zp->z_zfsvfs));

	sa_handle_destroy(zp->z_sa_hdl);
	zp->z_sa_hdl = NULL;
}

/*
 * Construct a new znode/vnode and initialize.
 *
 * This does not do a call to dmu_set_user() that is
 * up to the caller to do, in case you don't want to
 * return the znode
 */
static znode_t *
zfs_znode_alloc(zfsvfs_t *zfsvfs, dmu_buf_t *db, int blksz,
    dmu_object_type_t obj_type, sa_handle_t *hdl, vnode_t *vp)
{
	znode_t	*zp;
	uint64_t mode, rdev;
	uint64_t parent;
#ifdef notyet
	uint64_t mtime[2], ctime[2];
#endif
	uint64_t projid = ZFS_DEFAULT_PROJID;
	sa_bulk_attr_t bulk[9];
	int count = 0;
	int error;

	zp = zfs_znode_alloc_kmem(KM_SLEEP);

	/*
	 * The vnode comes from vcache_get() or vcache_new(), which hold it
	 * from every other thread until this returns; a failure here has
	 * vcache throw it away.
	 */
	zp->z_vnode = vp;
	vp->v_data = &zp->z_gnode;

	ASSERT(!POINTER_IS_VALID(zp->z_zfsvfs));

	zp->z_sa_hdl = NULL;
	zp->z_unlinked = 0;
	zp->z_atime_dirty = 0;
	zp->z_xattr_dir_absent = B_FALSE;
	zp->z_mapcnt = 0;
	zp->z_id = db->db_object;
	zp->z_blksz = blksz;
	zp->z_seq = 0x7A4653;
	zp->z_sync_cnt = 0;
	zp->z_cached_symlink = NULL;
	zp->z_lockf = NULL;

	zfs_znode_sa_init(zfsvfs, zp, db, obj_type, hdl);

	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MODE(zfsvfs), NULL, &mode, 8);
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_GEN(zfsvfs), NULL, &zp->z_gen, 8);
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_SIZE(zfsvfs), NULL,
	    &zp->z_size, 8);
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_LINKS(zfsvfs), NULL,
	    &zp->z_links, 8);
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_FLAGS(zfsvfs), NULL,
	    &zp->z_pflags, 8);
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_PARENT(zfsvfs), NULL, &parent, 8);
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_ATIME(zfsvfs), NULL,
	    &zp->z_atime, 16);
#ifdef notyet
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MTIME(zfsvfs), NULL,
	    &mtime, 16);
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_CTIME(zfsvfs), NULL,
	    &ctime, 16);
#endif
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_UID(zfsvfs), NULL,
	    &zp->z_uid, 8);
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_GID(zfsvfs), NULL,
	    &zp->z_gid, 8);

	if (sa_bulk_lookup(zp->z_sa_hdl, bulk, count) != 0 || zp->z_gen == 0 ||
	    (dmu_objset_projectquota_enabled(zfsvfs->z_os) &&
	    (zp->z_pflags & ZFS_PROJID) &&
	    sa_lookup(zp->z_sa_hdl, SA_ZPL_PROJID(zfsvfs), &projid, 8) != 0)) {
		if (hdl == NULL)
			sa_handle_destroy(zp->z_sa_hdl);
		vp->v_data = NULL;
		zp->z_vnode = NULL;
		zfs_znode_free_kmem(zp);
		return (NULL);
	}

	/*
	 * Restore z_seq from SA_ZPL_SEQ when present, marking the file migrated
	 * via the in-core z_has_seq (never persisted). Absence keeps the
	 * default z_seq; FreeBSD's va_filerev never folded ctime in, so no
	 * seed is needed across the upgrade.
	 */
	if (zp->z_is_sa && sa_lookup(zp->z_sa_hdl, SA_ZPL_SEQ(zfsvfs),
	    &zp->z_seq, sizeof (zp->z_seq)) == 0)
		zp->z_has_seq = B_TRUE;
	else
		zp->z_has_seq = B_FALSE;

	zp->z_projid = projid;
	zp->z_mode = mode;

	/* Cache the xattr parent id */
	if (zp->z_pflags & ZFS_XATTR)
		zp->z_xattr_parent = parent;

	vp->v_type = IFTOVT((mode_t)mode);
	vp->v_op = zfs_vnodeop_p;
	vp->v_tag = VT_ZFS;

	switch (vp->v_type) {
	case VDIR:
		zp->z_zn_prefetch = B_TRUE; /* z_prefetch default is enabled */
		break;
	case VBLK:
	case VCHR:
		VERIFY0(sa_lookup(zp->z_sa_hdl, SA_ZPL_RDEV(zfsvfs), &rdev,
		    sizeof (rdev)));
		vp->v_op = zfs_specop_p;
		spec_node_init(vp, zfs_cmpldev(rdev));
		break;
	case VFIFO:
		vp->v_op = zfs_fifoop_p;
		break;
	default:
		break;
	}
	genfs_node_init(vp, &zfs_genfsops);
	uvm_vnp_setsize(vp, zp->z_size);

	mutex_enter(&zfsvfs->z_znodes_lock);
	list_insert_tail(&zfsvfs->z_all_znodes, zp);
	zp->z_zfsvfs = zfsvfs;
	mutex_exit(&zfsvfs->z_znodes_lock);

	return (zp);
}

static uint64_t empty_xattr;
static uint64_t pad[4];
static zfs_acl_phys_t acl_phys;
/*
 * Create a new DMU object to hold a zfs znode.
 *
 *	IN:	dzp	- parent directory for new znode
 *		vap	- file attributes for new znode
 *		tx	- dmu transaction id for zap operations
 *		cr	- credentials of caller
 *		flag	- flags:
 *			  IS_ROOT_NODE	- new object will be root
 *			  IS_XATTR	- new object is an attribute
 *		bonuslen - length of bonus buffer
 *		setaclp  - File/Dir initial ACL
 *		fuidp	 - Tracks fuid allocation.
 *
 *	OUT:	zpp	- allocated znode
 *
 * On NetBSD the object is made by VFS_NEWVNODE, called from
 * vcache_new(), so that the vnode and the znode come into the vnode
 * cache together under the object's number.  The root of a new file
 * system is made with no mount and no vnode, by zfs_create_fs().  The
 * znode is returned with its vnode referenced and locked exclusively,
 * as the code that creates files expects.
 */
struct zfs_newvnode_args {
	dmu_tx_t	*zna_tx;
	uint_t		zna_flag;
	zfs_acl_ids_t	*zna_acl_ids;
};

static void zfs_mknode_impl(znode_t *, vattr_t *, dmu_tx_t *, cred_t *,
    uint_t, znode_t **, zfs_acl_ids_t *, vnode_t *);

void
zfs_mknode(znode_t *dzp, vattr_t *vap, dmu_tx_t *tx, cred_t *cr,
    uint_t flag, znode_t **zpp, zfs_acl_ids_t *acl_ids)
{
	struct zfs_newvnode_args args = { tx, flag, acl_ids };
	vnode_t *vp;

	if (flag & IS_ROOT_NODE) {
		zfs_mknode_impl(dzp, vap, tx, cr, flag, zpp, acl_ids, NULL);
		return;
	}
	VERIFY0(vcache_new(dzp->z_zfsvfs->z_vfs, ZTOV(dzp), vap, cr, &args,
	    &vp));
	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
	*zpp = VTOZ(vp);
}

int
zfs_newvnode(struct mount *mp, vnode_t *dvp, vnode_t *vp, vattr_t *vap,
    cred_t *cr, void *extra, size_t *key_len, const void **new_key)
{
	struct zfs_newvnode_args *args = extra;
	znode_t *zp;

	(void) mp;
	zfs_mknode_impl(VTOZ(dvp), vap, args->zna_tx, cr, args->zna_flag,
	    &zp, args->zna_acl_ids, vp);
	*key_len = sizeof (zp->z_id);
	*new_key = &zp->z_id;
	return (0);
}

static void
zfs_mknode_impl(znode_t *dzp, vattr_t *vap, dmu_tx_t *tx, cred_t *cr,
    uint_t flag, znode_t **zpp, zfs_acl_ids_t *acl_ids, vnode_t *vp)
{
	uint64_t	crtime[2], atime[2], mtime[2], ctime[2];
	uint64_t	mode, size, links, parent, pflags;
	uint64_t	dzp_pflags = 0;
	uint64_t	projid = ZFS_DEFAULT_PROJID;
	uint64_t	rdev = 0;
	zfsvfs_t	*zfsvfs = dzp->z_zfsvfs;
	dmu_buf_t	*db;
	timestruc_t	now;
	uint64_t	gen, obj;
	int		bonuslen;
	int		dnodesize;
	sa_handle_t	*sa_hdl;
	dmu_object_type_t obj_type;
	sa_bulk_attr_t	*sa_attrs;
	int		cnt = 0;
	zfs_acl_locator_cb_t locate = { 0 };

	ASSERT3P(vap, !=, NULL);
	ASSERT3U((vap->va_mask & AT_MODE), ==, AT_MODE);

	if (zfsvfs->z_replay) {
		obj = vap->va_nodeid;
		now = vap->va_ctime;		/* see zfs_replay_create() */
		gen = vap->va_nblocks;		/* ditto */
		dnodesize = vap->va_fsid;	/* ditto */
	} else {
		obj = 0;
		vfs_timestamp(&now);
		gen = dmu_tx_get_txg(tx);
		dnodesize = dmu_objset_dnodesize(zfsvfs->z_os);
	}

	if (dnodesize == 0)
		dnodesize = DNODE_MIN_SIZE;

	obj_type = zfsvfs->z_use_sa ? DMU_OT_SA : DMU_OT_ZNODE;
	bonuslen = (obj_type == DMU_OT_SA) ?
	    DN_BONUS_SIZE(dnodesize) : ZFS_OLD_ZNODE_PHYS_SIZE;

	/*
	 * Create a new DMU object.
	 */
	/*
	 * There's currently no mechanism for pre-reading the blocks that will
	 * be needed to allocate a new object, so we accept the small chance
	 * that there will be an i/o error and we will fail one of the
	 * assertions below.
	 */
	if (vap->va_type == VDIR) {
		if (zfsvfs->z_replay) {
			VERIFY0(zap_create_claim_norm_dnsize(zfsvfs->z_os, obj,
			    zfsvfs->z_norm, DMU_OT_DIRECTORY_CONTENTS,
			    obj_type, bonuslen, dnodesize, tx));
		} else {
			obj = zap_create_norm_dnsize(zfsvfs->z_os,
			    zfsvfs->z_norm, DMU_OT_DIRECTORY_CONTENTS,
			    obj_type, bonuslen, dnodesize, tx);
		}
	} else {
		if (zfsvfs->z_replay) {
			VERIFY0(dmu_object_claim_dnsize(zfsvfs->z_os, obj,
			    DMU_OT_PLAIN_FILE_CONTENTS, 0,
			    obj_type, bonuslen, dnodesize, tx));
		} else {
			obj = dmu_object_alloc_dnsize(zfsvfs->z_os,
			    DMU_OT_PLAIN_FILE_CONTENTS, 0,
			    obj_type, bonuslen, dnodesize, tx);
		}
	}

	ZFS_OBJ_HOLD_ENTER(zfsvfs, obj);
	VERIFY0(sa_buf_hold(zfsvfs->z_os, obj, NULL, &db));

	/*
	 * If this is the root, fix up the half-initialized parent pointer
	 * to reference the just-allocated physical data area.
	 */
	if (flag & IS_ROOT_NODE) {
		dzp->z_id = obj;
	} else {
		dzp_pflags = dzp->z_pflags;
	}

	/*
	 * If parent is an xattr, so am I.
	 */
	if (dzp_pflags & ZFS_XATTR) {
		flag |= IS_XATTR;
	}

	if (zfsvfs->z_use_fuids)
		pflags = ZFS_ARCHIVE | ZFS_AV_MODIFIED;
	else
		pflags = 0;

	if (vap->va_type == VDIR) {
		size = 2;		/* contents ("." and "..") */
		links = (flag & (IS_ROOT_NODE | IS_XATTR)) ? 2 : 1;
	} else {
		size = links = 0;
	}

	if (vap->va_type == VBLK || vap->va_type == VCHR) {
		rdev = zfs_expldev(vap->va_rdev);
	}

	parent = dzp->z_id;
	mode = acl_ids->z_mode;
	if (flag & IS_XATTR)
		pflags |= ZFS_XATTR;

	/*
	 * With ZFS_PROJID flag, we can easily know whether there is
	 * project ID stored on disk or not. See zpl_get_file_info().
	 */
	if (obj_type != DMU_OT_ZNODE &&
	    dmu_objset_projectquota_enabled(zfsvfs->z_os))
		pflags |= ZFS_PROJID;

	/*
	 * Inherit project ID from parent if required.  Every object type
	 * takes part, as ext4 and XFS do: an object that carried no project
	 * ID of its own would be treated as belonging to a different project
	 * than the directory holding it, so zfs_rename() and zfs_link()
	 * would refuse it with EXDEV even within its own project.
	 *
	 * The ZFS_PROJINHERIT flag itself keeps passing to regular files and
	 * directories only, as before, so that lsattr(1) output is unchanged.
	 */
	projid = zfs_inherit_projid(dzp);
	if ((vap->va_type == VREG || vap->va_type == VDIR) &&
	    (dzp_pflags & ZFS_PROJINHERIT))
		pflags |= ZFS_PROJINHERIT;

	/*
	 * No execs denied will be determined when zfs_mode_compute() is called.
	 */
	pflags |= acl_ids->z_aclp->z_hints &
	    (ZFS_ACL_TRIVIAL|ZFS_INHERIT_ACE|ZFS_ACL_AUTO_INHERIT|
	    ZFS_ACL_DEFAULTED|ZFS_ACL_PROTECTED);

	ZFS_TIME_ENCODE(&now, crtime);
	ZFS_TIME_ENCODE(&now, ctime);

	if (vap->va_mask & AT_ATIME) {
		ZFS_TIME_ENCODE(&vap->va_atime, atime);
	} else {
		ZFS_TIME_ENCODE(&now, atime);
	}

	if (vap->va_mask & AT_MTIME) {
		ZFS_TIME_ENCODE(&vap->va_mtime, mtime);
	} else {
		ZFS_TIME_ENCODE(&now, mtime);
	}

	/* Now add in all of the "SA" attributes */
	VERIFY0(sa_handle_get_from_db(zfsvfs->z_os, db, NULL, SA_HDL_SHARED,
	    &sa_hdl));

	/*
	 * Setup the array of attributes to be replaced/set on the new file
	 *
	 * order for  DMU_OT_ZNODE is critical since it needs to be constructed
	 * in the old znode_phys_t format.  Don't change this ordering
	 */
	sa_attrs = kmem_alloc(sizeof (sa_bulk_attr_t) * ZPL_END, KM_SLEEP);

	if (obj_type == DMU_OT_ZNODE) {
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_ATIME(zfsvfs),
		    NULL, &atime, 16);
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_MTIME(zfsvfs),
		    NULL, &mtime, 16);
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_CTIME(zfsvfs),
		    NULL, &ctime, 16);
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_CRTIME(zfsvfs),
		    NULL, &crtime, 16);
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_GEN(zfsvfs),
		    NULL, &gen, 8);
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_MODE(zfsvfs),
		    NULL, &mode, 8);
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_SIZE(zfsvfs),
		    NULL, &size, 8);
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_PARENT(zfsvfs),
		    NULL, &parent, 8);
	} else {
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_MODE(zfsvfs),
		    NULL, &mode, 8);
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_SIZE(zfsvfs),
		    NULL, &size, 8);
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_GEN(zfsvfs),
		    NULL, &gen, 8);
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_UID(zfsvfs),
		    NULL, &acl_ids->z_fuid, 8);
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_GID(zfsvfs),
		    NULL, &acl_ids->z_fgid, 8);
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_PARENT(zfsvfs),
		    NULL, &parent, 8);
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_FLAGS(zfsvfs),
		    NULL, &pflags, 8);
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_ATIME(zfsvfs),
		    NULL, &atime, 16);
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_MTIME(zfsvfs),
		    NULL, &mtime, 16);
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_CTIME(zfsvfs),
		    NULL, &ctime, 16);
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_CRTIME(zfsvfs),
		    NULL, &crtime, 16);
	}

	SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_LINKS(zfsvfs), NULL, &links, 8);

	if (obj_type == DMU_OT_ZNODE) {
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_XATTR(zfsvfs), NULL,
		    &empty_xattr, 8);
	} else if (dmu_objset_projectquota_enabled(zfsvfs->z_os) &&
	    pflags & ZFS_PROJID) {
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_PROJID(zfsvfs),
		    NULL, &projid, 8);
	}
	if (obj_type == DMU_OT_ZNODE ||
	    (vap->va_type == VBLK || vap->va_type == VCHR)) {
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_RDEV(zfsvfs),
		    NULL, &rdev, 8);

	}
	if (obj_type == DMU_OT_ZNODE) {
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_FLAGS(zfsvfs),
		    NULL, &pflags, 8);
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_UID(zfsvfs), NULL,
		    &acl_ids->z_fuid, 8);
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_GID(zfsvfs), NULL,
		    &acl_ids->z_fgid, 8);
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_PAD(zfsvfs), NULL, pad,
		    sizeof (uint64_t) * 4);
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_ZNODE_ACL(zfsvfs), NULL,
		    &acl_phys, sizeof (zfs_acl_phys_t));
	} else if (acl_ids->z_aclp->z_version >= ZFS_ACL_VERSION_FUID) {
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_DACL_COUNT(zfsvfs), NULL,
		    &acl_ids->z_aclp->z_acl_count, 8);
		locate.cb_aclp = acl_ids->z_aclp;
		SA_ADD_BULK_ATTR(sa_attrs, cnt, SA_ZPL_DACL_ACES(zfsvfs),
		    zfs_acl_data_locator, &locate,
		    acl_ids->z_aclp->z_acl_bytes);
		mode = zfs_mode_compute(mode, acl_ids->z_aclp, &pflags,
		    acl_ids->z_fuid, acl_ids->z_fgid);
	}

	VERIFY0(sa_replace_all_by_template(sa_hdl, sa_attrs, cnt, tx));

	if (!(flag & IS_ROOT_NODE)) {
		*zpp = zfs_znode_alloc(zfsvfs, db, 0, obj_type, sa_hdl, vp);
		VERIFY3P(*zpp, !=, NULL);
	} else {
		/*
		 * If we are creating the root node, the "parent" we
		 * passed in is the znode for the root.
		 */
		*zpp = dzp;

		(*zpp)->z_sa_hdl = sa_hdl;
	}

	(*zpp)->z_pflags = pflags;
	(*zpp)->z_mode = mode;
	(*zpp)->z_dnodesize = dnodesize;
	(*zpp)->z_projid = projid;

	if (vap->va_mask & AT_XVATTR)
		zfs_xvattr_set(*zpp, (xvattr_t *)vap, tx);

	if (obj_type == DMU_OT_ZNODE ||
	    acl_ids->z_aclp->z_version < ZFS_ACL_VERSION_FUID) {
		VERIFY0(zfs_aclset_common(*zpp, acl_ids->z_aclp, cr, tx));
	}
	kmem_free(sa_attrs, sizeof (sa_bulk_attr_t) * ZPL_END);
	ZFS_OBJ_HOLD_EXIT(zfsvfs, obj);
}

/*
 * Update in-core attributes.  It is assumed the caller will be doing an
 * sa_bulk_update to push the changes out.
 */
void
zfs_xvattr_set(znode_t *zp, xvattr_t *xvap, dmu_tx_t *tx)
{
	xoptattr_t *xoap;

	xoap = xva_getxoptattr(xvap);
	ASSERT3P(xoap, !=, NULL);

	if (zp->z_zfsvfs->z_replay == B_FALSE) {
		ASSERT_VOP_IN_SEQC(ZTOV(zp));
	}

	if (XVA_ISSET_REQ(xvap, XAT_CREATETIME)) {
		uint64_t times[2];
		ZFS_TIME_ENCODE(&xoap->xoa_createtime, times);
		(void) sa_update(zp->z_sa_hdl, SA_ZPL_CRTIME(zp->z_zfsvfs),
		    &times, sizeof (times), tx);
		XVA_SET_RTN(xvap, XAT_CREATETIME);
	}
	if (XVA_ISSET_REQ(xvap, XAT_READONLY)) {
		ZFS_ATTR_SET(zp, ZFS_READONLY, xoap->xoa_readonly,
		    zp->z_pflags, tx);
		XVA_SET_RTN(xvap, XAT_READONLY);
	}
	if (XVA_ISSET_REQ(xvap, XAT_HIDDEN)) {
		ZFS_ATTR_SET(zp, ZFS_HIDDEN, xoap->xoa_hidden,
		    zp->z_pflags, tx);
		XVA_SET_RTN(xvap, XAT_HIDDEN);
	}
	if (XVA_ISSET_REQ(xvap, XAT_SYSTEM)) {
		ZFS_ATTR_SET(zp, ZFS_SYSTEM, xoap->xoa_system,
		    zp->z_pflags, tx);
		XVA_SET_RTN(xvap, XAT_SYSTEM);
	}
	if (XVA_ISSET_REQ(xvap, XAT_ARCHIVE)) {
		ZFS_ATTR_SET(zp, ZFS_ARCHIVE, xoap->xoa_archive,
		    zp->z_pflags, tx);
		XVA_SET_RTN(xvap, XAT_ARCHIVE);
	}
	if (XVA_ISSET_REQ(xvap, XAT_IMMUTABLE)) {
		ZFS_ATTR_SET(zp, ZFS_IMMUTABLE, xoap->xoa_immutable,
		    zp->z_pflags, tx);
		XVA_SET_RTN(xvap, XAT_IMMUTABLE);
	}
	if (XVA_ISSET_REQ(xvap, XAT_NOUNLINK)) {
		ZFS_ATTR_SET(zp, ZFS_NOUNLINK, xoap->xoa_nounlink,
		    zp->z_pflags, tx);
		XVA_SET_RTN(xvap, XAT_NOUNLINK);
	}
	if (XVA_ISSET_REQ(xvap, XAT_APPENDONLY)) {
		ZFS_ATTR_SET(zp, ZFS_APPENDONLY, xoap->xoa_appendonly,
		    zp->z_pflags, tx);
		XVA_SET_RTN(xvap, XAT_APPENDONLY);
	}
	if (XVA_ISSET_REQ(xvap, XAT_NODUMP)) {
		ZFS_ATTR_SET(zp, ZFS_NODUMP, xoap->xoa_nodump,
		    zp->z_pflags, tx);
		XVA_SET_RTN(xvap, XAT_NODUMP);
	}
	if (XVA_ISSET_REQ(xvap, XAT_OPAQUE)) {
		ZFS_ATTR_SET(zp, ZFS_OPAQUE, xoap->xoa_opaque,
		    zp->z_pflags, tx);
		XVA_SET_RTN(xvap, XAT_OPAQUE);
	}
	if (XVA_ISSET_REQ(xvap, XAT_AV_QUARANTINED)) {
		ZFS_ATTR_SET(zp, ZFS_AV_QUARANTINED,
		    xoap->xoa_av_quarantined, zp->z_pflags, tx);
		XVA_SET_RTN(xvap, XAT_AV_QUARANTINED);
	}
	if (XVA_ISSET_REQ(xvap, XAT_AV_MODIFIED)) {
		ZFS_ATTR_SET(zp, ZFS_AV_MODIFIED, xoap->xoa_av_modified,
		    zp->z_pflags, tx);
		XVA_SET_RTN(xvap, XAT_AV_MODIFIED);
	}
	if (XVA_ISSET_REQ(xvap, XAT_AV_SCANSTAMP)) {
		zfs_sa_set_scanstamp(zp, xvap, tx);
		XVA_SET_RTN(xvap, XAT_AV_SCANSTAMP);
	}
	if (XVA_ISSET_REQ(xvap, XAT_REPARSE)) {
		ZFS_ATTR_SET(zp, ZFS_REPARSE, xoap->xoa_reparse,
		    zp->z_pflags, tx);
		XVA_SET_RTN(xvap, XAT_REPARSE);
	}
	if (XVA_ISSET_REQ(xvap, XAT_OFFLINE)) {
		ZFS_ATTR_SET(zp, ZFS_OFFLINE, xoap->xoa_offline,
		    zp->z_pflags, tx);
		XVA_SET_RTN(xvap, XAT_OFFLINE);
	}
	if (XVA_ISSET_REQ(xvap, XAT_SPARSE)) {
		ZFS_ATTR_SET(zp, ZFS_SPARSE, xoap->xoa_sparse,
		    zp->z_pflags, tx);
		XVA_SET_RTN(xvap, XAT_SPARSE);
	}
	if (XVA_ISSET_REQ(xvap, XAT_PROJINHERIT)) {
		ZFS_ATTR_SET(zp, ZFS_PROJINHERIT, xoap->xoa_projinherit,
		    zp->z_pflags, tx);
		XVA_SET_RTN(xvap, XAT_PROJINHERIT);
	}
}

/*
 * The znode of an object, with its vnode referenced and not locked.
 * vcache_get() finds the vnode in the cache, or makes one and has
 * zfs_loadvnode() read the object into it; either way it is the only
 * vnode for that object on this mount.
 */
int
zfs_zget(zfsvfs_t *zfsvfs, uint64_t obj_num, znode_t **zpp)
{
	vnode_t *vp;
	int err;

	*zpp = NULL;
	err = vcache_get(zfsvfs->z_vfs, &obj_num, sizeof (obj_num), &vp);
	if (err != 0)
		return (err);
	*zpp = VTOZ(vp);
	return (0);
}

int
zfs_loadvnode(struct mount *mp, vnode_t *vp, const void *key,
    size_t key_len, const void **new_key)
{
	zfsvfs_t *zfsvfs = mp->mnt_data;
	dmu_object_info_t doi;
	dmu_buf_t *db;
	znode_t *zp;
	uint64_t obj_num;
	int err;

	/* A .zfs node's key is longer than an object number. */
	if (key_len != sizeof (obj_num))
		return (zfsctl_loadvnode(mp, vp, key, key_len, new_key));
	memcpy(&obj_num, key, key_len);

	ZFS_OBJ_HOLD_ENTER(zfsvfs, obj_num);
	err = sa_buf_hold(zfsvfs->z_os, obj_num, NULL, &db);
	if (err != 0) {
		ZFS_OBJ_HOLD_EXIT(zfsvfs, obj_num);
		return (err);
	}
	dmu_object_info_from_db(db, &doi);
	if (doi.doi_bonus_type != DMU_OT_SA &&
	    (doi.doi_bonus_type != DMU_OT_ZNODE ||
	    (doi.doi_bonus_type == DMU_OT_ZNODE &&
	    doi.doi_bonus_size < sizeof (znode_phys_t)))) {
		sa_buf_rele(db, NULL);
		ZFS_OBJ_HOLD_EXIT(zfsvfs, obj_num);
		return (SET_ERROR(EINVAL));
	}
	/*
	 * A znode still on the buffer belongs to a vnode vcache has not
	 * finished reclaiming, which it does before asking for this one;
	 * so it is a znode with no vnode, as zfs_rezget() can leave.
	 */
	if (dmu_buf_get_user(db) != NULL) {
		sa_buf_rele(db, NULL);
		ZFS_OBJ_HOLD_EXIT(zfsvfs, obj_num);
		return (SET_ERROR(ENOENT));
	}
	zp = zfs_znode_alloc(zfsvfs, db, doi.doi_data_block_size,
	    doi.doi_bonus_type, NULL, vp);
	ZFS_OBJ_HOLD_EXIT(zfsvfs, obj_num);
	if (zp == NULL)
		return (SET_ERROR(ENOENT));
	*new_key = &zp->z_id;
	return (0);
}

int
zfs_rezget(znode_t *zp)
{
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	dmu_object_info_t doi;
	dmu_buf_t *db;
	vnode_t *vp;
	uint64_t obj_num = zp->z_id;
	uint64_t mode, size;
	sa_bulk_attr_t bulk[8];
	int err;
	int count = 0;
	uint64_t gen;
	uint64_t projid = ZFS_DEFAULT_PROJID;

	/*
	 * Remove cached pages before reloading the znode, so that they are not
	 * lingering after we run into any error.  Ideally, we should vgone()
	 * the vnode in case of error, but currently we cannot do that
	 * because of the LOR between the vnode lock and z_teardown_lock.
	 * So, instead, we have to "doom" the znode in the illumos style.
	 *
	 * Ignore invalid pages during the scan.  This is to avoid deadlocks
	 * between page busying and the teardown lock, as pages are busied prior
	 * to a VOP_GETPAGES operation, which acquires the teardown read lock.
	 * Such pages will be invalid and can safely be skipped here.
	 */
	vp = ZTOV(zp);
	rw_enter(vp->v_uobj.vmobjlock, RW_WRITER);
	(void) VOP_PUTPAGES(vp, 0, 0, PGO_ALLPAGES | PGO_FREE);

	ZFS_OBJ_HOLD_ENTER(zfsvfs, obj_num);

	mutex_enter(&zp->z_acl_lock);
	if (zp->z_acl_cached) {
		zfs_acl_free(zp->z_acl_cached);
		zp->z_acl_cached = NULL;
	}
	mutex_exit(&zp->z_acl_lock);

	rw_enter(&zp->z_xattr_lock, RW_WRITER);
	if (zp->z_xattr_cached) {
		nvlist_free(zp->z_xattr_cached);
		zp->z_xattr_cached = NULL;
	}
	zp->z_xattr_dir_absent = B_FALSE;
	rw_exit(&zp->z_xattr_lock);

	ASSERT0P(zp->z_sa_hdl);
	err = sa_buf_hold(zfsvfs->z_os, obj_num, NULL, &db);
	if (err) {
		ZFS_OBJ_HOLD_EXIT(zfsvfs, obj_num);
		return (err);
	}

	dmu_object_info_from_db(db, &doi);
	if (doi.doi_bonus_type != DMU_OT_SA &&
	    (doi.doi_bonus_type != DMU_OT_ZNODE ||
	    (doi.doi_bonus_type == DMU_OT_ZNODE &&
	    doi.doi_bonus_size < sizeof (znode_phys_t)))) {
		sa_buf_rele(db, NULL);
		ZFS_OBJ_HOLD_EXIT(zfsvfs, obj_num);
		return (SET_ERROR(EINVAL));
	}

	zfs_znode_sa_init(zfsvfs, zp, db, doi.doi_bonus_type, NULL);
	size = zp->z_size;

	/* reload cached values */
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_GEN(zfsvfs), NULL,
	    &gen, sizeof (gen));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_SIZE(zfsvfs), NULL,
	    &zp->z_size, sizeof (zp->z_size));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_LINKS(zfsvfs), NULL,
	    &zp->z_links, sizeof (zp->z_links));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_FLAGS(zfsvfs), NULL,
	    &zp->z_pflags, sizeof (zp->z_pflags));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_ATIME(zfsvfs), NULL,
	    &zp->z_atime, sizeof (zp->z_atime));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_UID(zfsvfs), NULL,
	    &zp->z_uid, sizeof (zp->z_uid));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_GID(zfsvfs), NULL,
	    &zp->z_gid, sizeof (zp->z_gid));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MODE(zfsvfs), NULL,
	    &mode, sizeof (mode));

	if (sa_bulk_lookup(zp->z_sa_hdl, bulk, count)) {
		zfs_znode_dmu_fini(zp);
		ZFS_OBJ_HOLD_EXIT(zfsvfs, obj_num);
		return (SET_ERROR(EIO));
	}

	if (dmu_objset_projectquota_enabled(zfsvfs->z_os)) {
		err = sa_lookup(zp->z_sa_hdl, SA_ZPL_PROJID(zfsvfs),
		    &projid, 8);
		if (err != 0 && err != ENOENT) {
			zfs_znode_dmu_fini(zp);
			ZFS_OBJ_HOLD_EXIT(zfsvfs, obj_num);
			return (err);
		}
	}

	zp->z_projid = projid;

	/*
	 * Reload z_has_seq and z_seq from disk so stale in-core state from
	 * before rollback/recv does not survive. A stale TRUE marker would
	 * make ZFS_SEQ_MAY_GROW() skip the grow reservation while SA_ZPL_SEQ
	 * is gone on disk.
	 */
	zp->z_has_seq = (zp->z_is_sa &&
	    sa_lookup(zp->z_sa_hdl, SA_ZPL_SEQ(zfsvfs),
	    &zp->z_seq, sizeof (zp->z_seq)) == 0);

	zp->z_mode = mode;

	if (gen != zp->z_gen) {
		zfs_znode_dmu_fini(zp);
		ZFS_OBJ_HOLD_EXIT(zfsvfs, obj_num);
		return (SET_ERROR(EIO));
	}

	/*
	 * It is highly improbable but still quite possible that two
	 * objects in different datasets are created with the same
	 * object numbers and in transaction groups with the same
	 * numbers.  znodes corresponding to those objects would
	 * have the same z_id and z_gen, but their other attributes
	 * may be different.
	 * zfs recv -F may replace one of such objects with the other.
	 * As a result file properties recorded in the replaced
	 * object's vnode may no longer match the received object's
	 * properties.  At present the only cached property is the
	 * files type recorded in v_type.
	 * So, handle this case by leaving the old vnode and znode
	 * disassociated from the actual object.  A new vnode and a
	 * znode will be created if the object is accessed
	 * (e.g. via a look-up).  The old vnode and znode will be
	 * recycled when the last vnode reference is dropped.
	 */
	if (vp->v_type != IFTOVT((mode_t)zp->z_mode)) {
		zfs_znode_dmu_fini(zp);
		ZFS_OBJ_HOLD_EXIT(zfsvfs, obj_num);
		return (SET_ERROR(EIO));
	}

	/*
	 * If the file has zero links, then it has been unlinked on the send
	 * side and it must be in the received unlinked set.
	 * We call zfs_znode_dmu_fini() now to prevent any accesses to the
	 * stale data and to prevent automatically removal of the file in
	 * zfs_zinactive().  The file will be removed either when it is removed
	 * on the send side and the next incremental stream is received or
	 * when the unlinked set gets processed.
	 */
	zp->z_unlinked = (zp->z_links == 0);
	if (zp->z_unlinked) {
		zfs_znode_dmu_fini(zp);
		ZFS_OBJ_HOLD_EXIT(zfsvfs, obj_num);
		return (0);
	}

	zp->z_blksz = doi.doi_data_block_size;
	if (zp->z_size != size)
		uvm_vnp_setsize(vp, zp->z_size);

	ZFS_OBJ_HOLD_EXIT(zfsvfs, obj_num);

	return (0);
}

void
zfs_znode_delete(znode_t *zp, dmu_tx_t *tx)
{
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	objset_t *os = zfsvfs->z_os;
	uint64_t obj = zp->z_id;
	uint64_t acl_obj = zfs_external_acl(zp);

	ZFS_OBJ_HOLD_ENTER(zfsvfs, obj);
	if (acl_obj) {
		VERIFY(!zp->z_is_sa);
		VERIFY0(dmu_object_free(os, acl_obj, tx));
	}
	VERIFY0(dmu_object_free(os, obj, tx));
	zfs_znode_dmu_fini(zp);
	ZFS_OBJ_HOLD_EXIT(zfsvfs, obj);
}

void
zfs_zinactive(znode_t *zp)
{
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	uint64_t z_id = zp->z_id;

	ASSERT3P(zp->z_sa_hdl, !=, NULL);

	/*
	 * Don't allow a zfs_zget() while were trying to release this znode
	 */
	ZFS_OBJ_HOLD_ENTER(zfsvfs, z_id);

	/*
	 * If this was the last reference to a file with no links, remove
	 * the file from the file system unless the file system is mounted
	 * read-only.  That can happen, for example, if the file system was
	 * originally read-write, the file was opened, then unlinked and
	 * the file system was made read-only before the file was finally
	 * closed.  The file will remain in the unlinked set.
	 */
	if (zp->z_unlinked) {
		ASSERT(!zfsvfs->z_issnap);
		if ((zfsvfs->z_vfs->vfs_flag & VFS_RDONLY) == 0) {
			ZFS_OBJ_HOLD_EXIT(zfsvfs, z_id);
			zfs_rmnode(zp);
			return;
		}
	}

	zfs_znode_dmu_fini(zp);
	ZFS_OBJ_HOLD_EXIT(zfsvfs, z_id);
	zfs_znode_free(zp);
}

void
zfs_znode_free(znode_t *zp)
{
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	char *symlink;

	ASSERT0P(zp->z_sa_hdl);
	zp->z_vnode = NULL;
	mutex_enter(&zfsvfs->z_znodes_lock);
	POINTER_INVALIDATE(&zp->z_zfsvfs);
	list_remove(&zfsvfs->z_all_znodes, zp);
	mutex_exit(&zfsvfs->z_znodes_lock);

	symlink = zp->z_cached_symlink;
	if (symlink != NULL) {
		zp->z_cached_symlink = NULL;
		kmem_strfree(symlink);
	}

	if (zp->z_acl_cached) {
		zfs_acl_free(zp->z_acl_cached);
		zp->z_acl_cached = NULL;
	}

	zfs_znode_free_kmem(zp);
}

/*
 * Determine whether the znode's atime must be updated.  The logic mostly
 * duplicates the Linux kernel's relatime_need_update() functionality.
 * This function is only called if the underlying filesystem actually has
 * atime updates enabled.
 */
boolean_t
zfs_relatime_need_update(const znode_t *zp)
{
	uint64_t mtime[2], ctime[2];
	sa_bulk_attr_t bulk[2];
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	struct timespec now, tmp_atime, tmp_ts;
	int count = 0;

	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MTIME(zfsvfs), NULL, mtime, 16);
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_CTIME(zfsvfs), NULL, ctime, 16);
	if (sa_bulk_lookup(zp->z_sa_hdl, bulk, count) != 0)
		return (B_TRUE);

	ZFS_TIME_DECODE(&tmp_atime, zp->z_atime);
	/*
	 * In relatime mode, only update the atime if the previous atime
	 * is earlier than either the ctime or mtime or if at least a day
	 * has passed since the last update of atime.
	 */
	ZFS_TIME_DECODE(&tmp_ts, mtime);
	/* CSTYLED */
	if (timespeccmp(&tmp_ts, &tmp_atime, >=))
		return (B_TRUE);

	ZFS_TIME_DECODE(&tmp_ts, ctime);
	/* CSTYLED */
	if (timespeccmp(&tmp_ts, &tmp_atime, >=))
		return (B_TRUE);

	vfs_timestamp(&now);
	if ((hrtime_t)now.tv_sec - (hrtime_t)tmp_atime.tv_sec >= 24*60*60)
		return (B_TRUE);

	return (B_FALSE);
}

void
zfs_tstamp_update_setup_ext(znode_t *zp, uint_t flag, uint64_t mtime[2],
    uint64_t ctime[2], boolean_t have_tx)
{
	timestruc_t	now;

	vfs_timestamp(&now);

	if (have_tx) {	/* will sa_bulk_update happen really soon? */
		zp->z_atime_dirty = 0;
		atomic_inc_64(&zp->z_seq);
	} else {
		zp->z_atime_dirty = 1;
	}

	if (flag & AT_ATIME) {
		ZFS_TIME_ENCODE(&now, zp->z_atime);
	}

	if (flag & AT_MTIME) {
		ZFS_TIME_ENCODE(&now, mtime);
		if (zp->z_zfsvfs->z_use_fuids) {
			zp->z_pflags |= (ZFS_ARCHIVE |
			    ZFS_AV_MODIFIED);
		}
	}

	if (flag & AT_CTIME) {
		ZFS_TIME_ENCODE(&now, ctime);
		if (zp->z_zfsvfs->z_use_fuids)
			zp->z_pflags |= ZFS_ARCHIVE;
	}
}


void
zfs_tstamp_update_setup(znode_t *zp, uint_t flag, uint64_t mtime[2],
    uint64_t ctime[2])
{
	zfs_tstamp_update_setup_ext(zp, flag, mtime, ctime, B_TRUE);
}
/*
 * Grow the block size for a file.
 *
 *	IN:	zp	- znode of file to free data in.
 *		size	- requested block size
 *		tx	- open transaction.
 *
 * NOTE: this function assumes that the znode is write locked.
 */
void
zfs_grow_blocksize(znode_t *zp, uint64_t size, dmu_tx_t *tx)
{
	int		error;
	u_longlong_t	dummy;

	if (size <= zp->z_blksz)
		return;
	/*
	 * If the file size is already greater than the current blocksize,
	 * we will not grow.  If there is more than one block in a file,
	 * the blocksize cannot change.
	 */
	if (zp->z_blksz && zp->z_size > zp->z_blksz)
		return;

	error = dmu_object_set_blocksize(zp->z_zfsvfs->z_os, zp->z_id,
	    size, 0, tx);

	if (error == ENOTSUP)
		return;
	ASSERT0(error);

	/* What blocksize did we actually get? */
	dmu_object_size_from_db(sa_get_db(zp->z_sa_hdl), &zp->z_blksz, &dummy);
}

/*
 * Increase the file length
 *
 *	IN:	zp	- znode of file to free data in.
 *		end	- new end-of-file
 *
 *	RETURN:	0 on success, error code on failure
 */
static int
zfs_extend(znode_t *zp, uint64_t end)
{
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	dmu_tx_t *tx;
	zfs_locked_range_t *lr;
	uint64_t newblksz;
	int error;

	/*
	 * We will change zp_size, lock the whole file.
	 */
	lr = zfs_rangelock_enter(&zp->z_rangelock, 0, UINT64_MAX, RL_WRITER);

	/*
	 * Nothing to do if file already at desired length.
	 */
	if (end <= zp->z_size) {
		zfs_rangelock_exit(lr);
		return (0);
	}
	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
	zfs_sa_upgrade_txholds(tx, zp);
	if (end > zp->z_blksz &&
	    (!ISP2(zp->z_blksz) || zp->z_blksz < zfsvfs->z_max_blksz)) {
		/*
		 * We are growing the file past the current block size.
		 */
		if (zp->z_blksz > zp->z_zfsvfs->z_max_blksz) {
			/*
			 * File's blocksize is already larger than the
			 * "recordsize" property.  Only let it grow to
			 * the next power of 2.
			 */
			ASSERT(!ISP2(zp->z_blksz));
			newblksz = MIN(end, 1 << highbit64(zp->z_blksz));
		} else {
			newblksz = MIN(end, zp->z_zfsvfs->z_max_blksz);
		}
		dmu_tx_hold_write(tx, zp->z_id, 0, newblksz);
	} else {
		newblksz = 0;
	}

	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error) {
		dmu_tx_abort(tx);
		zfs_rangelock_exit(lr);
		return (error);
	}

	if (newblksz)
		zfs_grow_blocksize(zp, newblksz, tx);

	zp->z_size = end;

	VERIFY0(sa_update(zp->z_sa_hdl, SA_ZPL_SIZE(zp->z_zfsvfs),
	    &zp->z_size, sizeof (zp->z_size), tx));

	uvm_vnp_setsize(ZTOV(zp), end);

	zfs_rangelock_exit(lr);

	dmu_tx_commit(tx);

	return (0);
}

/*
 * Free space in a file.
 *
 *	IN:	zp	- znode of file to free data in.
 *		off	- start of section to free.
 *		len	- length of section to free.
 *
 *	RETURN:	0 on success, error code on failure
 */
static int
zfs_free_range(znode_t *zp, uint64_t off, uint64_t len)
{
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	vnode_t *vp = ZTOV(zp);
	zfs_locked_range_t *lr;
	voff_t lo, hi;
	int error;

	/*
	 * The pages over the range are written back before it is freed,
	 * and dropped after: the pages at its edges hold data outside it
	 * as well, which must reach the disk first, and the data inside
	 * it must not be read from them again.  Both happen outside the
	 * range lock, which zfs_putpages() and zfs_getpages() take.
	 */
	lo = trunc_page(off);
	hi = round_page(MIN(off + len, zp->z_size));
	if (vn_has_cached_data(vp) && lo < hi) {
		rw_enter(vp->v_uobj.vmobjlock, RW_WRITER);
		(void) VOP_PUTPAGES(vp, lo, hi, PGO_CLEANIT | PGO_SYNCIO);
	}

	lr = zfs_rangelock_enter(&zp->z_rangelock, off, len, RL_WRITER);
	if (off >= zp->z_size) {
		zfs_rangelock_exit(lr);
		return (0);
	}
	if (off + len > zp->z_size)
		len = zp->z_size - off;
	error = dmu_free_long_range(zfsvfs->z_os, zp->z_id, off, len);
	zfs_rangelock_exit(lr);

	if (error == 0 && vn_has_cached_data(vp) && lo < hi) {
		rw_enter(vp->v_uobj.vmobjlock, RW_WRITER);
		(void) VOP_PUTPAGES(vp, lo, hi, PGO_FREE);
	}
	return (error);
}

/*
 * Truncate a file
 *
 *	IN:	zp	- znode of file to free data in.
 *		end	- new end-of-file.
 *
 *	RETURN:	0 on success, error code on failure
 */
static int
zfs_trunc(znode_t *zp, uint64_t end)
{
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	vnode_t *vp = ZTOV(zp);
	dmu_tx_t *tx;
	zfs_locked_range_t *lr;
	int error;
	sa_bulk_attr_t bulk[2];
	int count = 0;

	/*
	 * We will change zp_size, lock the whole file.
	 */
	lr = zfs_rangelock_enter(&zp->z_rangelock, 0, UINT64_MAX, RL_WRITER);

	/*
	 * Nothing to do if file already at desired length.
	 */
	if (end >= zp->z_size) {
		zfs_rangelock_exit(lr);
		return (0);
	}

	error = dmu_free_long_range(zfsvfs->z_os, zp->z_id, end,
	    DMU_OBJECT_END);
	if (error) {
		zfs_rangelock_exit(lr);
		return (error);
	}
	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
	zfs_sa_upgrade_txholds(tx, zp);
	dmu_tx_mark_netfree(tx);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error) {
		dmu_tx_abort(tx);
		zfs_rangelock_exit(lr);
		return (error);
	}

	zp->z_size = end;
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_SIZE(zfsvfs),
	    NULL, &zp->z_size, sizeof (zp->z_size));

	if (end == 0) {
		zp->z_pflags &= ~ZFS_SPARSE;
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_FLAGS(zfsvfs),
		    NULL, &zp->z_pflags, 8);
	}
	VERIFY0(sa_bulk_update(zp->z_sa_hdl, bulk, count, tx));

	dmu_tx_commit(tx);

	zfs_rangelock_exit(lr);

	/*
	 * Drop the pages past the new end, and zero what the last page
	 * holds past it, which a later extension would otherwise show.
	 * This is done outside the range lock, which zfs_getpages() and
	 * zfs_putpages() take.
	 */
	uvm_vnp_setsize(vp, end);
	if ((end & PAGE_MASK) != 0 && vn_has_cached_data(vp))
		ubc_zerorange(&vp->v_uobj, end, PAGE_SIZE - (end & PAGE_MASK),
		    UBC_VNODE_FLAGS(vp));

	return (0);
}

/*
 * Free space in a file
 *
 *	IN:	zp	- znode of file to free data in.
 *		off	- start of range
 *		len	- end of range (0 => EOF)
 *		flag	- current file open mode flags.
 *		log	- TRUE if this action should be logged
 *
 *	RETURN:	0 on success, error code on failure
 */
int
zfs_freesp(znode_t *zp, uint64_t off, uint64_t len, int flag, boolean_t log)
{
	dmu_tx_t *tx;
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	zilog_t *zilog = zfsvfs->z_log;
	uint64_t mode;
	uint64_t mtime[2], ctime[2];
	sa_bulk_attr_t bulk[4];
	int count = 0;
	int error;

	if ((error = sa_lookup(zp->z_sa_hdl, SA_ZPL_MODE(zfsvfs), &mode,
	    sizeof (mode))) != 0)
		return (error);

	if (off > zp->z_size) {
		error =  zfs_extend(zp, off+len);
		if (error == 0 && log)
			goto log;
		else
			return (error);
	}

	if (len == 0) {
		error = zfs_trunc(zp, off);
	} else {
		if ((error = zfs_free_range(zp, off, len)) == 0 &&
		    off + len > zp->z_size)
			error = zfs_extend(zp, off+len);
	}
	if (error || !log)
		return (error);
log:
	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_sa(tx, zp->z_sa_hdl, ZFS_SEQ_MAY_GROW(zp));
	zfs_sa_upgrade_txholds(tx, zp);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error) {
		dmu_tx_abort(tx);
		return (error);
	}

	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MTIME(zfsvfs), NULL, mtime, 16);
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_CTIME(zfsvfs), NULL, ctime, 16);
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_FLAGS(zfsvfs),
	    NULL, &zp->z_pflags, 8);
	zfs_tstamp_update_setup(zp, CONTENT_MODIFIED, mtime, ctime);
	ZFS_PERSIST_SEQ(zp, bulk, count);
	ASSERT3S(count, <=, ARRAY_SIZE(bulk));
	error = sa_bulk_update(zp->z_sa_hdl, bulk, count, tx);
	ASSERT0(error);

	zfs_log_truncate(zilog, tx, TX_TRUNCATE, zp, off, len);

	dmu_tx_commit(tx);
	return (0);
}

void
zfs_create_fs(objset_t *os, cred_t *cr, nvlist_t *zplprops, dmu_tx_t *tx)
{
	uint64_t	moid, obj, sa_obj, version;
	uint64_t	sense = ZFS_CASE_SENSITIVE;
	uint64_t	norm = 0;
	nvpair_t	*elem;
	int		error;
	int		i;
	znode_t		*rootzp = NULL;
	zfsvfs_t	*zfsvfs;
	vattr_t		vattr;
	znode_t		*zp;
	zfs_acl_ids_t	acl_ids;

	/*
	 * First attempt to create master node.
	 */
	/*
	 * In an empty objset, there are no blocks to read and thus
	 * there can be no i/o errors (which we assert below).
	 */
	moid = MASTER_NODE_OBJ;
	error = zap_create_claim(os, moid, DMU_OT_MASTER_NODE,
	    DMU_OT_NONE, 0, tx);
	ASSERT0(error);

	/*
	 * Set starting attributes.
	 */
	version = zfs_zpl_version_map(spa_version(dmu_objset_spa(os)));
	elem = NULL;
	while ((elem = nvlist_next_nvpair(zplprops, elem)) != NULL) {
		/* For the moment we expect all zpl props to be uint64_ts */
		uint64_t val;
		const char *name;

		ASSERT3S(nvpair_type(elem), ==, DATA_TYPE_UINT64);
		val = fnvpair_value_uint64(elem);
		name = nvpair_name(elem);
		if (strcmp(name, zfs_prop_to_name(ZFS_PROP_VERSION)) == 0) {
			if (val < version)
				version = val;
		} else {
			error = zap_update(os, moid, name, 8, 1, &val, tx);
		}
		ASSERT0(error);
		if (strcmp(name, zfs_prop_to_name(ZFS_PROP_NORMALIZE)) == 0)
			norm = val;
		else if (strcmp(name, zfs_prop_to_name(ZFS_PROP_CASE)) == 0)
			sense = val;
	}
	ASSERT3U(version, !=, 0);
	error = zap_update(os, moid, ZPL_VERSION_STR, 8, 1, &version, tx);
	ASSERT0(error);

	/*
	 * Create zap object used for SA attribute registration
	 */

	if (version >= ZPL_VERSION_SA) {
		sa_obj = zap_create(os, DMU_OT_SA_MASTER_NODE,
		    DMU_OT_NONE, 0, tx);
		error = zap_add(os, moid, ZFS_SA_ATTRS, 8, 1, &sa_obj, tx);
		ASSERT0(error);
	} else {
		sa_obj = 0;
	}
	/*
	 * Create a delete queue.
	 */
	obj = zap_create(os, DMU_OT_UNLINKED_SET, DMU_OT_NONE, 0, tx);

	error = zap_add(os, moid, ZFS_UNLINKED_SET, 8, 1, &obj, tx);
	ASSERT0(error);

	/*
	 * Create root znode.  Create minimal znode/vnode/zfsvfs
	 * to allow zfs_mknode to work.
	 */
	vattr_null(&vattr);
	vattr.va_mask = AT_MODE|AT_UID|AT_GID;
	vattr.va_type = VDIR;
	vattr.va_mode = S_IFDIR|0755;
	vattr.va_uid = crgetuid(cr);
	vattr.va_gid = crgetgid(cr);

	zfsvfs = kmem_zalloc(sizeof (zfsvfs_t), KM_SLEEP);

	rootzp = zfs_znode_alloc_kmem(KM_SLEEP);
	ASSERT(!POINTER_IS_VALID(rootzp->z_zfsvfs));
	rootzp->z_unlinked = 0;
	rootzp->z_atime_dirty = 0;
	rootzp->z_xattr_dir_absent = B_FALSE;
	rootzp->z_is_sa = USE_SA(version, os);
	rootzp->z_pflags = 0;

	zfsvfs->z_os = os;
	zfsvfs->z_parent = zfsvfs;
	zfsvfs->z_version = version;
	zfsvfs->z_use_fuids = USE_FUIDS(version, os);
	zfsvfs->z_use_sa = USE_SA(version, os);
	zfsvfs->z_norm = norm;

	error = sa_setup(os, sa_obj, zfs_attr_table, ZPL_END,
	    &zfsvfs->z_attr_table);

	ASSERT0(error);

	/*
	 * Fold case on file systems that are always or sometimes case
	 * insensitive.
	 */
	if (sense == ZFS_CASE_INSENSITIVE || sense == ZFS_CASE_MIXED)
		zfsvfs->z_norm |= U8_TEXTPREP_TOUPPER;

	mutex_init(&zfsvfs->z_znodes_lock, NULL, MUTEX_DEFAULT, NULL);
	list_create(&zfsvfs->z_all_znodes, sizeof (znode_t),
	    offsetof(znode_t, z_link_node));

	for (i = 0; i != ZFS_OBJ_MTX_SZ; i++)
		mutex_init(&zfsvfs->z_hold_mtx[i], NULL, MUTEX_DEFAULT, NULL);

	rootzp->z_zfsvfs = zfsvfs;
	VERIFY0(zfs_acl_ids_create(rootzp, IS_ROOT_NODE, &vattr,
	    cr, NULL, &acl_ids));
	zfs_mknode(rootzp, &vattr, tx, cr, IS_ROOT_NODE, &zp, &acl_ids);
	ASSERT3P(zp, ==, rootzp);
	error = zap_add(os, moid, ZFS_ROOT_OBJ, 8, 1, &rootzp->z_id, tx);
	ASSERT0(error);
	zfs_acl_ids_free(&acl_ids);
	POINTER_INVALIDATE(&rootzp->z_zfsvfs);

	sa_handle_destroy(rootzp->z_sa_hdl);
	zfs_znode_free_kmem(rootzp);

	/*
	 * Create shares directory
	 */

	error = zfs_create_share_dir(zfsvfs, tx);

	ASSERT0(error);

	for (i = 0; i != ZFS_OBJ_MTX_SZ; i++)
		mutex_destroy(&zfsvfs->z_hold_mtx[i]);
	kmem_free(zfsvfs, sizeof (zfsvfs_t));
}

void
zfs_znode_update_vfs(znode_t *zp)
{
	vnode_t *vp = ZTOV(zp);

	if (vp != NULL && vp->v_size != zp->z_size)
		uvm_vnp_setsize(vp, zp->z_size);
}

int
zfs_znode_parent_and_name(znode_t *zp, znode_t **dzpp, char *buf,
    uint64_t buflen)
{
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	uint64_t parent;
	int is_xattrdir;
	int err;

	/* Extended attributes should not be visible as regular files. */
	if ((zp->z_pflags & ZFS_XATTR) != 0)
		return (SET_ERROR(EINVAL));

	err = zfs_obj_to_pobj(zfsvfs->z_os, zp->z_sa_hdl, zfsvfs->z_attr_table,
	    &parent, &is_xattrdir);
	if (err != 0)
		return (err);
	ASSERT0(is_xattrdir);

	/* No name as this is a root object. */
	if (parent == zp->z_id)
		return (SET_ERROR(EINVAL));

	err = zap_value_search(zfsvfs->z_os, parent, zp->z_id,
	    ZFS_DIRENT_OBJ(-1ULL), buf, buflen);
	if (err != 0)
		return (err);
	err = zfs_zget(zfsvfs, parent, dzpp);
	return (err);
}

/*
 * A write that would take the file past the process's RLIMIT_FSIZE
 * fails with EFBIG and sends it SIGXFSZ, as enforce_rlimit_fsize() does
 * for NetBSD's own file systems.  A kernel thread has no limit.
 */
int
zfs_rlimit_fsize(off_t fsize)
{
	struct proc *p = curproc;

	if (p == NULL || p == &proc0 || (curlwp->l_flag & LW_SYSTEM) != 0)
		return (0);
	if (__predict_true((rlim_t)fsize <=
	    p->p_rlimit[RLIMIT_FSIZE].rlim_cur))
		return (0);
	mutex_enter(&proc_lock);
	psignal(p, SIGXFSZ);
	mutex_exit(&proc_lock);
	return (SET_ERROR(EFBIG));
}

int
zfs_rlimit_fsize_uio(znode_t *zp, zfs_uio_t *uio)
{

	(void) zp;
	return (zfs_rlimit_fsize(zfs_uio_offset(uio) + zfs_uio_resid(uio)));
}

/*
 * Write the file's dirty pages to the DMU, and wait for them if asked,
 * before something that reads it from the DMU instead: Direct I/O, or
 * a block clone.
 */
void
zfs_flush_cached_data(vnode_t *vp, boolean_t sync)
{

	if (!vn_has_cached_data(vp))
		return;
	rw_enter(vp->v_uobj.vmobjlock, RW_WRITER);
	(void) VOP_PUTPAGES(vp, 0, 0, PGO_ALLPAGES | PGO_CLEANIT |
	    (sync ? PGO_SYNCIO : 0));
}
