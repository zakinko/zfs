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
 * The .zfs directory: .zfs itself, .zfs/snapshot, and a directory there
 * for each snapshot, on which the snapshot is mounted the first time it
 * is looked up.
 *
 * This follows NetBSD's own, in osnet's zfs_ctldir.c under __NetBSD__:
 * the nodes are vnodes of the file system's own mount, told apart from
 * znodes in vcache by the length of their key, and a snapshot is
 * mounted by building its mount here, since mount_domount() takes its
 * path from user space.  The parent is the snapshot's lower mount, as
 * there: fstrans then takes the two as one, so the parent's unmount,
 * which holds the global vfs_suspend_lock through VFS_UNMOUNT(), can
 * call dounmount() on the snapshots in zfsctl_umount_snapshots()
 * without taking that lock again.  Without it that is a locking
 * against myself panic in vfs_suspend().
 *
 * One thing differs from osnet: the snapshot's root keeps VV_ROOT, so
 * ".." from it crosses back to its directory under .zfs/snapshot as
 * from any mount, and umount(8) of its path works.
 */

#include <sys/zfs_context.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/dirent.h>
#include <sys/zfs_ctldir.h>
#include <sys/zfs_ioctl.h>
#include <sys/zfs_vfsops.h>
#include <sys/zfs_znode.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_dataset.h>
#include <sys/dmu_objset.h>
#include <sys/zap.h>
#include <miscfs/genfs/genfs.h>

#define	ZFS_SNAPDIR_NAME	"snapshot"

struct zfsctl_root {
	timestruc_t	zc_cmtime;	/* the root's creation time */
};

/*
 * A node's key: the parent's id and its own.  .zfs is (0, ROOT),
 * .zfs/snapshot (ROOT, SNAPDIR), and a snapshot's directory (SNAPDIR,
 * the snapshot's objset id).  A znode's key is its object number
 * alone, so the length tells the two apart.
 */
struct sfs_node_key {
	uint64_t	parent_id;
	uint64_t	id;
};

struct sfs_node {
	struct sfs_node_key sn_key;
	lwp_t		*sn_mounting;	/* mounting the snapshot here */
};
#define	sn_parent_id	sn_key.parent_id
#define	sn_id		sn_key.id

#define	VTOSFS(vp)	((struct sfs_node *)(vp)->v_data)

static int (**zfs_sfsop_p)(void *);

static int
zfsctl_get(zfsvfs_t *zfsvfs, uint64_t parent_id, uint64_t id, vnode_t **vpp)
{
	struct sfs_node_key key = { .parent_id = parent_id, .id = id };

	return (vcache_get(zfsvfs->z_vfs, &key, sizeof (key), vpp));
}

/*
 * The ".zfs" vnode, referenced, and locked as flags asks (0 for not).
 */
int
zfsctl_root(zfsvfs_t *zfsvfs, int flags, vnode_t **vpp)
{
	int error;

	if ((error = zfsctl_get(zfsvfs, 0, ZFSCTL_INO_ROOT, vpp)) != 0)
		return (error);
	if (flags != 0 && (error = vn_lock(*vpp, flags)) != 0) {
		vrele(*vpp);
		*vpp = NULL;
	}
	return (error);
}

/*
 * Put ".zfs" back in the name cache under the root.  getcwd(3) finds a
 * directory's name in the cache or else in its parent's readdir, where
 * a hidden .zfs is not; and the cache drops entries like any other, so
 * an entry made at the lookup of .zfs alone was gone after some churn,
 * and /bin/pwd under .zfs/snapshot failed 20 times in 20.  A getcwd
 * that reaches .zfs touches it just before it asks the cache: by ".."
 * from .zfs/snapshot, or by VOP_ACCESS when it starts in .zfs.  Both
 * call this.
 */
static void
zfsctl_cache_ctldir(zfsvfs_t *zfsvfs, vnode_t *ctlvp)
{
	znode_t *rootzp;

	if (!zfsvfs->z_use_namecache)
		return;
	if (zfs_zget(zfsvfs, zfsvfs->z_root, &rootzp) != 0)
		return;
	cache_enter(ZTOV(rootzp), ctlvp, ZFS_CTLDIR_NAME,
	    strlen(ZFS_CTLDIR_NAME), MAKEENTRY);
	vrele(ZTOV(rootzp));
}

static int
zfsctl_snapdir(zfsvfs_t *zfsvfs, vnode_t **vpp)
{

	return (zfsctl_get(zfsvfs, ZFSCTL_INO_ROOT, ZFSCTL_INO_SNAPDIR, vpp));
}

int
zfsctl_loadvnode(struct mount *mp, vnode_t *vp, const void *key,
    size_t key_len, const void **new_key)
{
	struct sfs_node *node;

	if (key_len != sizeof (struct sfs_node_key))
		return (SET_ERROR(EINVAL));
	if (mp->mnt_iflag & IMNT_UNMOUNT)
		return (SET_ERROR(ENOENT));

	node = kmem_alloc(sizeof (*node), KM_SLEEP);
	memcpy(&node->sn_key, key, key_len);
	node->sn_mounting = NULL;

	vp->v_data = node;
	vp->v_op = zfs_sfsop_p;
	vp->v_tag = VT_ZFS;
	vp->v_type = VDIR;
	uvm_vnp_setsize(vp, 0);
	*new_key = &node->sn_key;
	return (0);
}

boolean_t
zfsctl_is_node(vnode_t *vp)
{

	return (vp->v_op == zfs_sfsop_p);
}

/*
 * A .zfs node's NFS handle: its id with a generation of 0, which no
 * znode has (zfs_vptofh() gives 1 for it).
 */
int
zfsctl_vptofh(vnode_t *vp, struct fid *fhp, size_t *fh_size)
{
	struct sfs_node *node = VTOSFS(vp);
	zfid_short_t zfid;
	size_t need, i;

	need = offsetof(struct fid, fid_data) + SHORT_FID_LEN;
	if (*fh_size < need) {
		*fh_size = need;
		return (SET_ERROR(E2BIG));
	}
	for (i = 0; i < sizeof (zfid.zf_object); i++)
		zfid.zf_object[i] = (uint8_t)(node->sn_id >> (8 * i));
	memset(zfid.zf_gen, 0, sizeof (zfid.zf_gen));
	memset(fhp, 0, need);
	fhp->fid_len = need;
	memcpy(fhp->fid_data, zfid.zf_object, SHORT_FID_LEN);
	*fh_size = need;
	return (0);
}

/* The .zfs directory, or .zfs/snapshot, for an NFS handle. */
int
zfsctl_fhtovp(zfsvfs_t *zfsvfs, uint64_t object, vnode_t **vpp)
{

	if (object == ZFSCTL_INO_ROOT)
		return (zfsctl_root(zfsvfs, 0, vpp));
	if (object == ZFSCTL_INO_SNAPDIR)
		return (zfsctl_snapdir(zfsvfs, vpp));
	return (SET_ERROR(EINVAL));
}

/*
 * Mount snapshot snapname of the file system dvp is in, read-only, on
 * vp, its directory under .zfs/snapshot.
 */
static int
zfsctl_snapshot_mount(vnode_t *vp, const char *snapname)
{
	zfsvfs_t *zfsvfs = vp->v_mount->mnt_data;
	struct vfsops *ops;
	struct mount *mp;
	char *path, *osname;
	int error;

	if ((ops = vfs_getopsbyname(vp->v_mount->mnt_op->vfs_name)) == NULL)
		return (SET_ERROR(ENODEV));
	path = PNBUF_GET();
	osname = PNBUF_GET();
	dmu_objset_name(zfsvfs->z_os, path);
	(void) snprintf(osname, MAXPATHLEN, "%s@%s", path, snapname);
	(void) snprintf(path, MAXPATHLEN, "%s/" ZFS_CTLDIR_NAME "/"
	    ZFS_SNAPDIR_NAME "/%s", vp->v_mount->mnt_stat.f_mntonname,
	    snapname);

	mp = vfs_mountalloc(ops, vp);
	mp->mnt_stat.f_owner = 0;
	mp->mnt_flag = MNT_RDONLY | MNT_NOSUID | MNT_NODEV | MNT_IGNORE;
	mutex_enter(mp->mnt_updating);
	if ((error = zfs_domount(mp, osname, "")) != 0) {
		mutex_exit(mp->mnt_updating);
		vfs_rele(mp);
		goto out;
	}
	(void) set_statvfs_info(path, UIO_SYSSPACE, osname, UIO_SYSSPACE,
	    mp->mnt_op->vfs_name, mp, curlwp);
	if ((error = vfs_set_lowermount(mp, vp->v_mount)) != 0) {
		if (VFS_UNMOUNT(mp, MNT_FORCE) != 0)
			panic("zfsctl_snapshot_mount: unmounting %s", osname);
		mutex_exit(mp->mnt_updating);
		vfs_rele(mp);
		goto out;
	}
	mountlist_append(mp);
	vref(vp);	/* the covered vnode's, which dounmount() drops */
	vp->v_mountedhere = mp;
	mutex_exit(mp->mnt_updating);
	(void) VFS_STATVFS(mp, &mp->mnt_stat);
out:
	PNBUF_PUT(osname);
	PNBUF_PUT(path);
	return (error);
}

/*
 * Look up a snapshot in .zfs/snapshot, mounting it if it is not, and
 * return the root of the snapshot.
 */
static int
zfsctl_lookup_snapshot(vnode_t *dvp, struct componentname *cnp,
    vnode_t **vpp)
{
	zfsvfs_t *zfsvfs = dvp->v_mount->mnt_data;
	char snapname[ZFS_MAX_DATASET_NAME_LEN];
	struct sfs_node *node;
	uint64_t id;
	vnode_t *vp;
	int error;

	if (cnp->cn_namelen >= sizeof (snapname))
		return (SET_ERROR(ENOENT));
	(void) strlcpy(snapname, cnp->cn_nameptr, cnp->cn_namelen + 1);
	if ((error = dsl_dataset_snap_lookup(dmu_objset_ds(zfsvfs->z_os),
	    snapname, &id)) != 0)
		return (error);
	if ((error = zfsctl_get(zfsvfs, ZFSCTL_INO_SNAPDIR, id, &vp)) != 0)
		return (error);

	/* Another thread mounting it: let namei try again. */
	mutex_enter(vp->v_interlock);
	node = VTOSFS(vp);
	if (node->sn_mounting != NULL) {
		error = (node->sn_mounting == curlwp) ? 0 : ERESTART;
		mutex_exit(vp->v_interlock);
		if (error != 0) {
			vrele(vp);
			yield();
			return (SET_ERROR(error));
		}
	} else if (vp->v_mountedhere == NULL) {
		node->sn_mounting = curlwp;
		mutex_exit(vp->v_interlock);

		VOP_UNLOCK(dvp);
		error = zfsctl_snapshot_mount(vp, snapname);
		vn_lock(dvp, LK_EXCLUSIVE | LK_RETRY);

		mutex_enter(vp->v_interlock);
		node->sn_mounting = NULL;
		mutex_exit(vp->v_interlock);
		if (error != 0) {
			vrele(vp);
			return (error);
		}
	} else {
		mutex_exit(vp->v_interlock);
	}

	/*
	 * The directory itself, which namei crosses into the mount, as
	 * for any mount point.
	 */
	*vpp = vp;
	return (0);
}

static int
zfsctl_lookup(void *v)
{
	struct vop_lookup_v2_args *ap = v;
	vnode_t *dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
	zfsvfs_t *zfsvfs = dvp->v_mount->mnt_data;
	struct sfs_node *dnode = VTOSFS(dvp);
	vnode_t **vpp = ap->a_vpp;
	int error;

	*vpp = NULL;
	if ((cnp->cn_flags & ISLASTCN) && cnp->cn_nameiop != LOOKUP)
		return (SET_ERROR(EROFS));
	if ((error = zfs_enter(zfsvfs, FTAG)) != 0)
		return (error);

	if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
		vref(dvp);
		*vpp = dvp;
	} else if (cnp->cn_flags & ISDOTDOT) {
		if (dnode->sn_parent_id == 0) {
			znode_t *rootzp;

			error = zfs_zget(zfsvfs, zfsvfs->z_root, &rootzp);
			if (error == 0)
				*vpp = ZTOV(rootzp);
		} else if (dnode->sn_parent_id == ZFSCTL_INO_ROOT) {
			error = zfsctl_root(zfsvfs, 0, vpp);
			if (error == 0)
				zfsctl_cache_ctldir(zfsvfs, *vpp);
		} else {
			error = zfsctl_snapdir(zfsvfs, vpp);
		}
	} else if (dnode->sn_id == ZFSCTL_INO_ROOT) {
		if (cnp->cn_namelen == strlen(ZFS_SNAPDIR_NAME) &&
		    strncmp(cnp->cn_nameptr, ZFS_SNAPDIR_NAME,
		    cnp->cn_namelen) == 0)
			error = zfsctl_snapdir(zfsvfs, vpp);
		else
			error = SET_ERROR(ENOENT);
	} else if (dnode->sn_id == ZFSCTL_INO_SNAPDIR) {
		error = zfsctl_lookup_snapshot(dvp, cnp, vpp);
	} else {
		/* A snapshot's directory, with nothing mounted on it. */
		error = SET_ERROR(ENOENT);
	}
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

static int
zfsctl_open(void *v)
{
	struct vop_open_args *ap = v;

	return ((ap->a_mode & FWRITE) ? SET_ERROR(EACCES) : 0);
}

static int
zfsctl_close(void *v)
{

	(void) v;
	return (0);
}

static int
zfsctl_access(void *v)
{
	struct vop_access_args *ap = v;
	vnode_t *vp = ap->a_vp;
	zfsvfs_t *zfsvfs = vp->v_mount->mnt_data;

	if (ap->a_accmode & VWRITE)
		return (SET_ERROR(EACCES));
	if (VTOSFS(vp)->sn_id == ZFSCTL_INO_ROOT &&
	    zfs_enter(zfsvfs, FTAG) == 0) {
		zfsctl_cache_ctldir(zfsvfs, vp);
		zfs_exit(zfsvfs, FTAG);
	}
	return (0);
}

static int
zfsctl_getattr(void *v)
{
	struct vop_getattr_args *ap = v;
	vnode_t *vp = ap->a_vp;
	struct sfs_node *node = VTOSFS(vp);
	vattr_t *vap = ap->a_vap;
	zfsvfs_t *zfsvfs = vp->v_mount->mnt_data;
	dsl_dataset_t *ds;
	uint64_t snap_count;
	timestruc_t now;
	int error;

	if ((error = zfs_enter(zfsvfs, FTAG)) != 0)
		return (error);
	vattr_null(vap);
	vap->va_type = VDIR;
	vap->va_mode = S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH |
	    S_IXOTH;
	vap->va_nlink = 2;
	vap->va_uid = 0;
	vap->va_gid = 0;
	vap->va_fsid = vp->v_mount->mnt_stat.f_fsid;
	vap->va_fileid = node->sn_id;
	vap->va_size = 0;
	vap->va_blocksize = 0;
	gethrestime(&now);
	vap->va_atime = now;
	vap->va_ctime = zfsvfs->z_ctldir->zc_cmtime;
	vap->va_mtime = vap->va_ctime;
	vap->va_birthtime = vap->va_ctime;
	vap->va_gen = 0;
	vap->va_flags = 0;
	vap->va_rdev = 0;
	vap->va_bytes = 0;
	vap->va_filerev = 0;

	switch (node->sn_id) {
	case ZFSCTL_INO_ROOT:
		vap->va_nlink += 1;	/* snapshot */
		vap->va_size = vap->va_nlink;
		break;
	case ZFSCTL_INO_SNAPDIR:
		ds = dmu_objset_ds(zfsvfs->z_os);
		if (dsl_dataset_phys(ds)->ds_snapnames_zapobj != 0) {
			error = zap_count(
			    dmu_objset_pool(ds->ds_objset)->dp_meta_objset,
			    dsl_dataset_phys(ds)->ds_snapnames_zapobj,
			    &snap_count);
			if (error != 0)
				break;
			vap->va_nlink += snap_count;
		}
		vap->va_size = vap->va_nlink;
		break;
	}
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

static int
zfsctl_readdir_one(struct vop_readdir_args *ap, struct dirent *dp,
    const char *name, ino_t ino, off_t *offp)
{
	int error;

	dp->d_fileno = ino;
	dp->d_type = DT_DIR;
	(void) strlcpy(dp->d_name, name, sizeof (dp->d_name));
	dp->d_namlen = strlen(dp->d_name);
	dp->d_reclen = _DIRENT_SIZE(dp);

	if (ap->a_uio->uio_offset > *offp) {
		*offp += dp->d_reclen;
		return (0);
	}
	if (ap->a_uio->uio_resid < dp->d_reclen)
		return (SET_ERROR(ENAMETOOLONG));
	if ((error = uiomove(dp, dp->d_reclen, ap->a_uio)) != 0)
		return (error);
	*offp += dp->d_reclen;
	if (ap->a_ncookies != NULL)
		(*ap->a_cookies)[(*ap->a_ncookies)++] = *offp;
	return (0);
}

static int
zfsctl_readdir(void *v)
{
	struct vop_readdir_args *ap = v;
	vnode_t *vp = ap->a_vp;
	struct sfs_node *node = VTOSFS(vp);
	zfsvfs_t *zfsvfs = vp->v_mount->mnt_data;
	struct dirent *dp;
	uint64_t parent;
	off_t offset;
	int error, ncookies = 0;

	if ((error = zfs_enter(zfsvfs, FTAG)) != 0)
		return (error);
	parent = (node->sn_parent_id == 0) ? zfsvfs->z_root :
	    node->sn_parent_id;
	dp = kmem_zalloc(sizeof (*dp), KM_SLEEP);
	if (ap->a_ncookies != NULL) {
		ncookies = ap->a_uio->uio_resid / _DIRENT_MINSIZE(dp) + 1;
		*ap->a_ncookies = 0;
		*ap->a_cookies = malloc(ncookies * sizeof (off_t), M_TEMP,
		    M_WAITOK);
	}

	offset = 0;
	error = zfsctl_readdir_one(ap, dp, ".", node->sn_id, &offset);
	if (error == 0)
		error = zfsctl_readdir_one(ap, dp, "..", parent, &offset);
	if (error == 0 && node->sn_id == ZFSCTL_INO_ROOT) {
		error = zfsctl_readdir_one(ap, dp, ZFS_SNAPDIR_NAME,
		    ZFSCTL_INO_SNAPDIR, &offset);
	} else if (error == 0 && node->sn_id == ZFSCTL_INO_SNAPDIR) {
		char snapname[ZFS_MAX_DATASET_NAME_LEN];
		uint64_t cookie = 0, id;

		for (;;) {
			dsl_pool_config_enter(dmu_objset_pool(zfsvfs->z_os),
			    FTAG);
			error = dmu_snapshot_list_next(zfsvfs->z_os,
			    sizeof (snapname), snapname, &id, &cookie, NULL);
			dsl_pool_config_exit(dmu_objset_pool(zfsvfs->z_os),
			    FTAG);
			if (error != 0) {
				if (error == ENOENT)
					error = 0;
				break;
			}
			error = zfsctl_readdir_one(ap, dp, snapname, id,
			    &offset);
			if (error != 0)
				break;
			if (ap->a_ncookies != NULL &&
			    *ap->a_ncookies >= ncookies)
				break;
		}
	}

	/* Out of room is the end of this read, not an error. */
	if (error == ENAMETOOLONG)
		error = 0;
	else if (error == 0 && ap->a_eofflag != NULL)
		*ap->a_eofflag = 1;
	if (ap->a_ncookies != NULL && error != 0) {
		free(*ap->a_cookies, M_TEMP);
		*ap->a_ncookies = 0;
		*ap->a_cookies = NULL;
	}
	kmem_free(dp, sizeof (*dp));
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

static int
zfsctl_inactive(void *v)
{
	struct vop_inactive_v2_args *ap = v;

	/* A snapshot's directory is cheap to make again. */
	*ap->a_recycle = (VTOSFS(ap->a_vp)->sn_parent_id ==
	    ZFSCTL_INO_SNAPDIR && ap->a_vp->v_mountedhere == NULL);
	return (0);
}

static int
zfsctl_reclaim(void *v)
{
	struct vop_reclaim_v2_args *ap = v;
	vnode_t *vp = ap->a_vp;
	struct sfs_node *node = VTOSFS(vp);

	VOP_UNLOCK(vp);
	vp->v_data = NULL;
	kmem_free(node, sizeof (*node));
	return (0);
}

static int
zfsctl_print(void *v)
{
	struct vop_print_args *ap = v;
	struct sfs_node *node = VTOSFS(ap->a_vp);

	printf("\tzfs .zfs node id %llu, parent %llu\n",
	    (u_longlong_t)node->sn_id, (u_longlong_t)node->sn_parent_id);
	return (0);
}

static const struct vnodeopv_entry_desc zfs_sfsop_entries[] = {
	{ &vop_default_desc,		vn_default_error },
	{ &vop_parsepath_desc,		genfs_parsepath },
	{ &vop_lookup_desc,		zfsctl_lookup },
	{ &vop_open_desc,		zfsctl_open },
	{ &vop_close_desc,		zfsctl_close },
	{ &vop_access_desc,		zfsctl_access },
	{ &vop_accessx_desc,		genfs_accessx },
	{ &vop_getattr_desc,		zfsctl_getattr },
	{ &vop_lock_desc,		genfs_lock },
	{ &vop_unlock_desc,		genfs_unlock },
	{ &vop_islocked_desc,		genfs_islocked },
	{ &vop_readdir_desc,		zfsctl_readdir },
	{ &vop_inactive_desc,		zfsctl_inactive },
	{ &vop_reclaim_desc,		zfsctl_reclaim },
	{ &vop_seek_desc,		genfs_seek },
	{ &vop_putpages_desc,		genfs_null_putpages },
	{ &vop_print_desc,		zfsctl_print },
	{ &vop_pathconf_desc,		genfs_pathconf },
	{ NULL, NULL }
};

const struct vnodeopv_desc zfs_sfsop_opv_desc =
	{ &zfs_sfsop_p, zfs_sfsop_entries };

void
zfsctl_init(void)
{
}

void
zfsctl_fini(void)
{
}

/*
 * Give a mounted file system its .zfs, timed as its root was made.
 */
void
zfsctl_create(zfsvfs_t *zfsvfs)
{
	struct zfsctl_root *zc;
	uint64_t crtime[2];
	znode_t *rootzp;

	ASSERT0P(zfsvfs->z_ctldir);
	zc = kmem_zalloc(sizeof (*zc), KM_SLEEP);
	VERIFY0(zfs_zget(zfsvfs, zfsvfs->z_root, &rootzp));
	VERIFY0(sa_lookup(rootzp->z_sa_hdl, SA_ZPL_CRTIME(zfsvfs), &crtime,
	    sizeof (crtime)));
	vrele(ZTOV(rootzp));
	ZFS_TIME_DECODE(&zc->zc_cmtime, crtime);
	zfsvfs->z_ctldir = zc;
}

void
zfsctl_destroy(zfsvfs_t *zfsvfs)
{

	if (zfsvfs->z_ctldir != NULL)
		kmem_free(zfsvfs->z_ctldir, sizeof (struct zfsctl_root));
	zfsvfs->z_ctldir = NULL;
}

/*
 * The file system of a snapshot mounted under .zfs/snapshot, by its
 * objset id.
 */
int
zfsctl_lookup_objset(vfs_t *vfsp, uint64_t objsetid, zfsvfs_t **zfsvfsp)
{
	zfsvfs_t *zfsvfs = vfsp->mnt_data;
	vnode_t *vp;

	*zfsvfsp = NULL;
	if (zfsctl_get(zfsvfs, ZFSCTL_INO_SNAPDIR, objsetid, &vp) == 0) {
		if (vp->v_mountedhere != NULL)
			*zfsvfsp = vp->v_mountedhere->mnt_data;
		vrele(vp);
	}
	return (*zfsvfsp == NULL ? SET_ERROR(EINVAL) : 0);
}

/*
 * Unmount every snapshot mounted under this file system's .zfs, before
 * it is unmounted itself.
 */
int
zfsctl_umount_snapshots(vfs_t *vfsp, int fflags, cred_t *cr)
{
	char snapname[ZFS_MAX_DATASET_NAME_LEN];
	zfsvfs_t *zfsvfs = vfsp->mnt_data;
	struct mount *mp;
	uint64_t cookie = 0, id;
	vnode_t *vp;
	int error;

	(void) cr;
	if (zfsvfs->z_ctldir == NULL)
		return (0);
	for (;;) {
		dsl_pool_config_enter(dmu_objset_pool(zfsvfs->z_os), FTAG);
		error = dmu_snapshot_list_next(zfsvfs->z_os, sizeof (snapname),
		    snapname, &id, &cookie, NULL);
		dsl_pool_config_exit(dmu_objset_pool(zfsvfs->z_os), FTAG);
		if (error != 0) {
			if (error == ENOENT)
				error = 0;
			break;
		}
		/* Only one already in vcache can have anything on it. */
		struct sfs_node_key key = {
			.parent_id = ZFSCTL_INO_SNAPDIR, .id = id
		};
		if (vcache_get(vfsp, &key, sizeof (key), &vp) != 0)
			continue;
		if ((mp = vp->v_mountedhere) == NULL) {
			vrele(vp);
			continue;
		}
		/* dounmount() leaves the caller's reference, as sys_unmount. */
		vfs_ref(mp);
		error = dounmount(mp, fflags, curlwp);
		vfs_rele(mp);
		vrele(vp);
		if (error != 0)
			break;
	}
	return (error);
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
	int error;

	if (strchr(snapname, '@') == NULL)
		return (0);
	if (getzfsvfs(snapname, &zfsvfs) != 0)
		return (0);
	vfsp = zfsvfs->z_vfs;
	ASSERT(!dsl_pool_config_held(dmu_objset_pool(zfsvfs->z_os)));
	vfs_ref(vfsp);
	vfs_unbusy(vfsp);
	/*
	 * dounmount() does not take the reference: sys_unmount() drops
	 * its own after it.  Leaving it kept the mount and its vfsops
	 * referenced, and modunload of zfs then failed with EBUSY.
	 */
	error = dounmount(vfsp, MNT_FORCE, curlwp);
	vfs_rele(vfsp);
	return (error);
}
