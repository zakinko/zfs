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
	if ((pb = pathbuf_create(vd->vdev_path)) == NULL)
		return (SET_ERROR(ENOMEM));
	error = vn_open(NULL, pb, 0, fflags, 0, &vp, NULL, NULL);
	pathbuf_destroy(pb);
	if (error != 0) {
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
		return (SET_ERROR(error));
	}
	VOP_UNLOCK(vp);
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
