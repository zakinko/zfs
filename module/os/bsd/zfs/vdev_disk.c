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
 * A leaf vdev on a NetBSD block device: a disk, a partition or a dk(4)
 * wedge, named by its /dev path.
 *
 * Reads and writes go to the driver as bufs through VOP_STRATEGY(),
 * from a linear copy of the zio's ABD, and complete through b_iodone,
 * which biodone() calls from a soft interrupt or from the thread that
 * finished the I/O; nothing there may sleep or touch an ABD, so the
 * copy back is left to vdev_disk_io_done().  A buf larger than the
 * driver takes in one transfer is split with nestiobuf_setup().
 *
 * A cache flush and a TRIM are an ioctl and a discard, which wait for
 * the device, so they are handed to a taskq rather than done in the
 * zio pipeline's own thread.  The taskq is this file's own, as
 * vdev_file.c has z_vdev_file: on system_taskq a flush waited behind
 * zvol_prefetch_minors_impl() tasks blocked on spa_namespace_lock,
 * whose holder, spa_async_thread in spa_config_update(), was waiting
 * for the txg that the flush belonged to.
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_trim.h>
#include <sys/zio.h>
#include <sys/abd.h>
#include <sys/fs/zfs.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/device.h>
#include <sys/disk.h>
#include <sys/disklabel.h>
#include <sys/dkio.h>
#include <sys/namei.h>
#include <sys/vnode.h>
#include <miscfs/specfs/specdev.h>

static taskq_t *vdev_disk_taskq;

void
vdev_disk_init(void)
{

	vdev_disk_taskq = taskq_create("z_vdev_disk", MAX(boot_ncpus, 16),
	    minclsyspri, boot_ncpus, INT_MAX, TASKQ_DYNAMIC);
	VERIFY(vdev_disk_taskq);
}

void
vdev_disk_fini(void)
{

	taskq_destroy(vdev_disk_taskq);
}

typedef struct vdev_disk {
	struct vnode	*vd_vp;
	int		vd_fflags;	/* FREAD, and FWRITE if writable */
	size_t		vd_maxphys;	/* largest transfer of the driver */
} vdev_disk_t;

/* The largest transfer the disk's driver takes, as physio() asks it. */
static size_t
vdev_disk_maxphys(struct vnode *vp)
{
	struct dkwedge_info dkw;
	struct disk *dk;
	struct buf buf;

	memset(&buf, 0, sizeof (buf));
	buf.b_dev = vp->v_rdev;
	buf.b_bcount = MAXPHYS;
	if (getdiskinfo(vp, &dkw) == 0 &&
	    (dk = disk_find(dkw.dkw_parent)) != NULL &&
	    dk->dk_driver != NULL && dk->dk_driver->d_minphys != NULL)
		(*dk->dk_driver->d_minphys)(&buf);
	return (buf.b_bcount);
}

static boolean_t
vdev_disk_can_discard(struct vnode *vp)
{
	const struct bdevsw *bdev = bdevsw_lookup(vp->v_rdev);

	return (bdev != NULL && bdev->d_discard != NULL &&
	    bdev->d_discard != nodiscard);
}

/*
 * The block device a /dev path names, found without the path: while the
 * root file system is being mounted there is no /dev to look it up in.
 * "/dev/dk3" is a wedge, which has no partitions; "/dev/ld2d" is
 * partition d of disk ld2.
 */
static dev_t
vdev_disk_name2dev(const char *path)
{
	char name[16];
	device_t dv;
	devmajor_t maj;
	size_t len;
	int part = -1;

	if (strncmp(path, "/dev/", 5) != 0 ||
	    strlcpy(name, path + 5, sizeof (name)) >= sizeof (name))
		return (NODEV);
	if ((dv = device_find_by_xname(name)) == NULL) {
		len = strlen(name);
		if (len < 2 || name[len - 1] < 'a' ||
		    name[len - 1] >= 'a' + MAXPARTITIONS)
			return (NODEV);
		part = name[len - 1] - 'a';
		name[len - 1] = '\0';
		if ((dv = device_find_by_xname(name)) == NULL ||
		    device_is_a(dv, "dk"))
			return (NODEV);
	}
	if (device_class(dv) != DV_DISK ||
	    (maj = devsw_name2blk(device_xname(dv), NULL, 0)) == NODEVMAJOR)
		return (NODEV);
	if (device_is_a(dv, "dk"))
		return (makedev(maj, device_unit(dv)));
	return (MAKEDISKDEV(maj, device_unit(dv), part < 0 ? RAW_PART : part));
}

/*
 * Open a block device by its dev_t, unlocked, as vn_open() leaves a
 * vnode: vn_close() undoes both, so the write count is kept the same.
 */
static int
vdev_disk_open_dev(dev_t dev, int fflags, struct vnode **vpp)
{
	struct vnode *vp;
	int error;

	if ((error = bdevvp(dev, &vp)) != 0)
		return (error);
	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
	if ((error = VOP_OPEN(vp, fflags, kcred)) != 0) {
		vput(vp);
		return (error);
	}
	if (fflags & FWRITE) {
		mutex_enter(vp->v_interlock);
		vp->v_writecount++;
		mutex_exit(vp->v_interlock);
	}
	VOP_UNLOCK(vp);
	*vpp = vp;
	return (0);
}

static int
vdev_disk_open(vdev_t *vd, uint64_t *psize, uint64_t *max_psize,
    uint64_t *logical_ashift, uint64_t *physical_ashift, cred_t *cr)
{
	vdev_disk_t *dvd;
	struct pathbuf *pb;
	struct vnode *vp;
	struct disk_sectoralign dsa;
	uint64_t numsecs;
	unsigned int secsize;
	int fflags, error;

	(void) cr;
	if (vd->vdev_path == NULL || vd->vdev_path[0] != '/') {
		vd->vdev_stat.vs_aux = VDEV_AUX_BAD_LABEL;
		return (SET_ERROR(EINVAL));
	}

	/* A reopen keeps the device open and asks its size again. */
	if (vd->vdev_tsd != NULL) {
		ASSERT(vd->vdev_reopening);
		dvd = vd->vdev_tsd;
		vp = dvd->vd_vp;
		goto skip_open;
	}

	fflags = FREAD;
	if (spa_mode(vd->vdev_spa) & SPA_MODE_WRITE)
		fflags |= FWRITE;
	if (rootvnode == NULL) {
		/* The root pool, before there is a root to find /dev on. */
		dev_t dev = vdev_disk_name2dev(vd->vdev_path);

		error = (dev == NODEV) ? SET_ERROR(ENOENT) :
		    vdev_disk_open_dev(dev, fflags, &vp);
	} else {
		if ((pb = pathbuf_create(vd->vdev_path)) == NULL)
			return (SET_ERROR(ENOMEM));
		error = vn_open(NULL, pb, 0, fflags, 0, &vp, NULL, NULL);
		pathbuf_destroy(pb);
		if (error == 0)
			VOP_UNLOCK(vp);
	}
	if (error != 0) {
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
		return (SET_ERROR(error));
	}
	if (vp->v_type != VBLK) {
		(void) vn_close(vp, fflags, kcred);
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
		return (SET_ERROR(EINVAL));
	}

	dvd = kmem_zalloc(sizeof (*dvd), KM_SLEEP);
	dvd->vd_vp = vp;
	dvd->vd_fflags = fflags;
	dvd->vd_maxphys = vdev_disk_maxphys(vp);
	vd->vdev_tsd = dvd;

skip_open:
	if ((error = getdisksize(vp, &numsecs, &secsize)) != 0) {
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
		return (SET_ERROR(error));
	}
	*psize = numsecs * secsize;
	*max_psize = *psize;
	*logical_ashift = highbit(MAX(secsize, SPA_MINBLOCKSIZE)) - 1;

	/*
	 * A disk that emulates small sectors over larger ones says how
	 * many of its sectors make one of the larger.
	 */
	*physical_ashift = 0;
	if (VOP_IOCTL(vp, DIOCGSECTORALIGN, &dsa, FREAD, kcred) == 0 &&
	    dsa.dsa_alignment > 1)
		*physical_ashift =
		    highbit((uint64_t)dsa.dsa_alignment * secsize) - 1;

	/* Ask for the cache to be flushed again after a reopen. */
	vd->vdev_nowritecache = B_FALSE;
	vd->vdev_has_trim = vdev_disk_can_discard(vp);
	vd->vdev_has_securetrim = B_FALSE;
	return (0);
}

static void
vdev_disk_close(vdev_t *vd)
{
	vdev_disk_t *dvd = vd->vdev_tsd;

	if (vd->vdev_reopening || dvd == NULL)
		return;
	(void) vn_close(dvd->vd_vp, dvd->vd_fflags, kcred);
	kmem_free(dvd, sizeof (*dvd));
	vd->vdev_tsd = NULL;
}

static void
vdev_disk_io_intr(struct buf *bp)
{
	zio_t *zio = bp->b_private;

	zio->io_error = bp->b_error;
	if (zio->io_error == 0 && bp->b_resid != 0)
		zio->io_error = SET_ERROR(EIO);
	zio_delay_interrupt(zio);
}

/* A cache flush or a TRIM, in a taskq thread. */
static void
vdev_disk_io_sync(void *arg)
{
	zio_t *zio = arg;
	vdev_t *vd = zio->io_vd;
	vdev_disk_t *dvd = vd->vdev_tsd;
	struct vnode *vp = dvd->vd_vp;
	int error, force = 1;

	if (zio->io_type == ZIO_TYPE_FLUSH) {
		error = VOP_IOCTL(vp, DIOCCACHESYNC, &force, FWRITE, kcred);
		/*
		 * A device with no cache to flush, or a driver that
		 * cannot, is not asked again until the vdev is reopened.
		 */
		if (error == ENOTTY || error == ENODEV || error == EOPNOTSUPP) {
			vd->vdev_nowritecache = B_TRUE;
			error = SET_ERROR(ENOTSUP);
		}
	} else {
		vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
		error = VOP_FDISCARD(vp, zio->io_offset, zio->io_size);
		VOP_UNLOCK(vp);
		/*
		 * A wedge passes a discard on to its disk, which may not
		 * take one; only then is it known.
		 */
		if (error == EOPNOTSUPP || error == ENODEV) {
			vd->vdev_has_trim = B_FALSE;
			error = SET_ERROR(ENOTSUP);
		}
	}
	zio->io_error = error;
	zio_interrupt(zio);
}

static void
vdev_disk_io_start(zio_t *zio)
{
	vdev_t *vd = zio->io_vd;
	vdev_disk_t *dvd = vd->vdev_tsd;
	struct vnode *vp;
	struct buf *bp, *nbp;
	size_t off, resid, size;

	if (dvd == NULL) {
		zio->io_error = SET_ERROR(ENXIO);
		zio_interrupt(zio);
		return;
	}
	vp = dvd->vd_vp;

	switch (zio->io_type) {
	case ZIO_TYPE_FLUSH:
		if (!vdev_readable(vd)) {
			zio->io_error = SET_ERROR(ENXIO);
			zio_interrupt(zio);
			return;
		}
		if (zfs_nocacheflush) {
			zio_execute(zio);
			return;
		}
		if (vd->vdev_nowritecache) {
			zio->io_error = SET_ERROR(ENOTSUP);
			zio_execute(zio);
			return;
		}
		(void) taskq_dispatch(vdev_disk_taskq, vdev_disk_io_sync, zio,
		    TQ_SLEEP);
		return;

	case ZIO_TYPE_TRIM:
		if (!vd->vdev_has_trim) {
			zio->io_error = SET_ERROR(ENOTSUP);
			zio_execute(zio);
			return;
		}
		(void) taskq_dispatch(vdev_disk_taskq, vdev_disk_io_sync, zio,
		    TQ_SLEEP);
		return;

	case ZIO_TYPE_READ:
	case ZIO_TYPE_WRITE:
		break;

	default:
		zio->io_error = SET_ERROR(ENOTSUP);
		zio_interrupt(zio);
		return;
	}

	zio->io_target_timestamp = zio_handle_io_delay(zio);
	bp = getiobuf(vp, true);
	bp->b_flags = zio->io_type == ZIO_TYPE_READ ? B_READ : B_WRITE;
	bp->b_cflags |= BC_BUSY | BC_NOCACHE;
	if (zio->io_type == ZIO_TYPE_READ)
		bp->b_data = abd_borrow_buf(zio->io_abd, zio->io_size);
	else
		bp->b_data = abd_borrow_buf_copy(zio->io_abd, zio->io_size);
	bp->b_dev = vp->v_rdev;
	bp->b_blkno = btodb(zio->io_offset);
	bp->b_bcount = zio->io_size;
	bp->b_bufsize = zio->io_size;
	bp->b_resid = zio->io_size;
	bp->b_iodone = vdev_disk_io_intr;
	bp->b_private = zio;
	zio->io_bio = bp;

	/* biodone() counts a write down again in vwakeup(). */
	if (zio->io_type == ZIO_TYPE_WRITE) {
		mutex_enter(vp->v_interlock);
		vp->v_numoutput++;
		mutex_exit(vp->v_interlock);
	}

	if (bp->b_bcount <= dvd->vd_maxphys) {
		(void) VOP_STRATEGY(vp, bp);
		return;
	}
	for (off = 0, resid = zio->io_size; resid != 0;
	    off += size, resid -= size) {
		size = MIN(resid, dvd->vd_maxphys);
		nbp = getiobuf(vp, true);
		nestiobuf_setup(bp, nbp, off, size);
		nbp->b_blkno = btodb(zio->io_offset + off);
		(void) VOP_STRATEGY(vp, nbp);
	}
}

static void
vdev_disk_io_done(zio_t *zio)
{
	struct buf *bp = zio->io_bio;

	if (zio->io_type != ZIO_TYPE_READ && zio->io_type != ZIO_TYPE_WRITE)
		return;
	if (bp == NULL) {
		/* The vdev was closed, and the zio never reached a buf. */
		return;
	}
	if (zio->io_type == ZIO_TYPE_READ)
		abd_return_buf_copy(zio->io_abd, bp->b_data, zio->io_size);
	else
		abd_return_buf(zio->io_abd, bp->b_data, zio->io_size);
	putiobuf(bp);
	zio->io_bio = NULL;
}

static void
vdev_disk_hold(vdev_t *vd)
{

	(void) vd;
}

static void
vdev_disk_rele(vdev_t *vd)
{

	(void) vd;
}

vdev_ops_t vdev_disk_ops = {
	.vdev_op_init = NULL,
	.vdev_op_fini = NULL,
	.vdev_op_open = vdev_disk_open,
	.vdev_op_close = vdev_disk_close,
	.vdev_op_asize_to_psize = vdev_default_psize,
	.vdev_op_psize_to_asize = vdev_default_asize,
	.vdev_op_min_asize = vdev_default_min_asize,
	.vdev_op_min_alloc = NULL,
	.vdev_op_io_start = vdev_disk_io_start,
	.vdev_op_io_done = vdev_disk_io_done,
	.vdev_op_state_change = NULL,
	.vdev_op_need_resilver = NULL,
	.vdev_op_hold = vdev_disk_hold,
	.vdev_op_rele = vdev_disk_rele,
	.vdev_op_remap = NULL,
	.vdev_op_xlate = vdev_default_xlate,
	.vdev_op_rebuild_asize = NULL,
	.vdev_op_metaslab_init = NULL,
	.vdev_op_config_generate = NULL,
	.vdev_op_nparity = NULL,
	.vdev_op_ndisks = NULL,
	.vdev_op_type = VDEV_TYPE_DISK,
	.vdev_op_leaf = B_TRUE,
	.vdev_op_kobj_evt_post = NULL
};

/*
 * Read the configuration in the best of a device's four labels, as
 * zpool import does: a label of a pool that is not active, or not yet
 * written (txg 0), is no configuration.
 */
static int
vdev_disk_read_config(struct vnode *vp, nvlist_t **configp)
{
	vdev_phys_t *vp_phys;
	nvlist_t *config;
	uint64_t numsecs, psize, state, txg;
	unsigned int secsize;
	size_t resid;
	int l, nlabels = 0;

	*configp = NULL;
	if (getdisksize(vp, &numsecs, &secsize) != 0)
		return (0);
	psize = P2ALIGN_TYPED(numsecs * secsize, sizeof (vdev_label_t),
	    uint64_t);
	if (psize < SPA_MINDEVSIZE)
		return (0);
	vp_phys = kmem_alloc(sizeof (*vp_phys), KM_SLEEP);
	for (l = 0; l < VDEV_LABELS; l++) {
		if (vn_rdwr(UIO_READ, vp, vp_phys, sizeof (*vp_phys),
		    vdev_label_offset(psize, l,
		    offsetof(vdev_label_t, vl_vdev_phys)), UIO_SYSSPACE,
		    IO_NODELOCKED, kcred, &resid, curlwp) != 0 || resid != 0)
			continue;
		if (nvlist_unpack(vp_phys->vp_nvlist,
		    sizeof (vp_phys->vp_nvlist), &config, 0) != 0)
			continue;
		if (nvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_STATE,
		    &state) != 0 || state > POOL_STATE_L2CACHE ||
		    (state != POOL_STATE_SPARE && state != POOL_STATE_L2CACHE &&
		    (nvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_TXG,
		    &txg) != 0 || txg == 0))) {
			nvlist_free(config);
			continue;
		}
		if (*configp != NULL)
			nvlist_free(*configp);
		*configp = config;
		nlabels++;
	}
	kmem_free(vp_phys, sizeof (*vp_phys));
	return (nlabels);
}

/*
 * Keep cfg, a label's configuration, as configs[] has it by top-level
 * vdev id, if it is of the pool name names and newer than the one there.
 */
static void
vdev_disk_keep_config(nvlist_t ***configs, uint64_t *count, nvlist_t *cfg,
    const char *name, uint64_t *pool_guid)
{
	nvlist_t *tree, **n;
	const char *pname;
	uint64_t guid, top_guid, id, txg, children = 1;

	/*
	 * The id sizes configs[], so it is held to the count of top-level
	 * vdevs the label gives, which an old label leaves out for one, as
	 * spa_generate_rootconf() takes it.
	 */
	(void) nvlist_lookup_uint64(cfg, ZPOOL_CONFIG_VDEV_CHILDREN, &children);
	if (nvlist_lookup_string(cfg, ZPOOL_CONFIG_POOL_NAME, &pname) != 0 ||
	    strcmp(pname, name) != 0 ||
	    nvlist_lookup_uint64(cfg, ZPOOL_CONFIG_POOL_GUID, &guid) != 0 ||
	    nvlist_lookup_uint64(cfg, ZPOOL_CONFIG_TOP_GUID, &top_guid) != 0 ||
	    nvlist_lookup_nvlist(cfg, ZPOOL_CONFIG_VDEV_TREE, &tree) != 0 ||
	    nvlist_lookup_uint64(tree, ZPOOL_CONFIG_ID, &id) != 0 ||
	    nvlist_lookup_uint64(cfg, ZPOOL_CONFIG_POOL_TXG, &txg) != 0 ||
	    id >= children ||
	    (*pool_guid != 0 && guid != *pool_guid)) {
		nvlist_free(cfg);
		return;
	}
	*pool_guid = guid;
	if (id >= *count) {
		n = kmem_zalloc((id + 1) * sizeof (nvlist_t *), KM_SLEEP);
		if (*count != 0) {
			memcpy(n, *configs, *count * sizeof (nvlist_t *));
			kmem_free(*configs, *count * sizeof (nvlist_t *));
		}
		*configs = n;
		*count = id + 1;
	}
	if ((*configs)[id] != NULL) {
		if (txg <= fnvlist_lookup_uint64((*configs)[id],
		    ZPOOL_CONFIG_POOL_TXG)) {
			nvlist_free(cfg);
			return;
		}
		nvlist_free((*configs)[id]);
	}
	(*configs)[id] = cfg;
}

/*
 * Find the labels of pool name on every disk and wedge, for the root
 * pool, which is imported before there is a /dev or a zpool.cache.
 * configs[] gets the newest configuration of each top-level vdev, and
 * paths maps each vdev guid found to the /dev path of the device it is
 * on now, since the paths in the labels are where the devices were.
 * Floppies and optical drives are passed over: reading one with no
 * medium takes seconds and prints errors.
 */
int
vdev_disk_read_pool_label(const char *name, nvlist_t ***configs,
    uint64_t *count, nvlist_t **paths)
{
	char path[MAXPATHLEN];
	struct vnode *vp;
	nvlist_t *cfg;
	deviter_t di;
	device_t dv;
	devmajor_t maj;
	dev_t dev;
	uint64_t pool_guid = 0, guid;

	*configs = NULL;
	*count = 0;
	*paths = fnvlist_alloc();
	for (dv = deviter_first(&di, DEVITER_F_ROOT_FIRST); dv != NULL;
	    dv = deviter_next(&di)) {
		if (device_class(dv) != DV_DISK ||
		    device_is_a(dv, "fd") || device_is_a(dv, "cd") ||
		    (maj = devsw_name2blk(device_xname(dv), NULL, 0)) ==
		    NODEVMAJOR)
			continue;
		if (device_is_a(dv, "dk")) {
			dev = makedev(maj, device_unit(dv));
			(void) snprintf(path, sizeof (path), "/dev/%s",
			    device_xname(dv));
		} else {
			dev = MAKEDISKDEV(maj, device_unit(dv), RAW_PART);
			(void) snprintf(path, sizeof (path), "/dev/%s%c",
			    device_xname(dv), 'a' + RAW_PART);
		}
		if (vdev_disk_open_dev(dev, FREAD, &vp) != 0)
			continue;
		vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
		(void) vdev_disk_read_config(vp, &cfg);
		VOP_UNLOCK(vp);
		(void) vn_close(vp, FREAD, kcred);
		if (cfg == NULL)
			continue;
		if (nvlist_lookup_uint64(cfg, ZPOOL_CONFIG_GUID, &guid) == 0) {
			char key[24];

			(void) snprintf(key, sizeof (key), "%llu",
			    (u_longlong_t)guid);
			fnvlist_add_string(*paths, key, path);
		}
		vdev_disk_keep_config(configs, count, cfg, name, &pool_guid);
	}
	deviter_release(&di);
	return (*count > 0 ? 0 : SET_ERROR(ENOENT));
}
