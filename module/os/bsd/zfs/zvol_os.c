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
 * Volumes as NetBSD disks.
 *
 * A volume is a minor of the zfs major, which conf/majors gives the
 * same number as a character and a block device; minor 0 is /dev/zfs.
 * Its nodes are made where NetBSD's own ZFS made them,
 * /dev/zvol/rdsk/<name> and /dev/zvol/dsk/<name>, so that what names
 * them -- fstab, swapctl -- goes on working.  The kernel makes them
 * itself, as osnet does, there being no devfs to.
 *
 * I/O is FreeBSD's "dev" volume mode: reads and writes of the character
 * device go through the DMU as a uio, and the block device's bufs are
 * done in the thread that hands them over.  NetBSD calls a device's close
 * once, on its last close, for the character and the block device each,
 * so zv_open_count counts which of the two are open, as a disk driver's
 * openmask does.
 *
 * Each is a disk(9) as well, as osnet's are: disk_ioctl() answers
 * DIOCGDISKINFO, which newfs and the rest of userland ask a disk for its
 * geometry, and the disk's statistics are what iostat shows.
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/dmu_tx.h>
#include <sys/dnode.h>
#include <sys/zap.h>
#include <sys/zil.h>
#include <sys/zil_impl.h>
#include <sys/zfs_rlock.h>
#include <sys/dataset_kstats.h>
#include <sys/dsl_prop.h>
#include <sys/zvol.h>
#include <sys/zvol_impl.h>
#include <sys/fs/zfs.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/dkio.h>
#include <sys/disklabel.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/vfs_syscalls.h>
#include <sys/zfs_netbsd_dev.h>

#define	ZVOL_NETBSD_DSK		"/dev/zvol/dsk/"
#define	ZVOL_NETBSD_RDSK	"/dev/zvol/rdsk/"

/* What is open: the character device, the block device, or both. */
#define	ZSO_OPEN_CHR	0x1
#define	ZSO_OPEN_BLK	0x2

struct zvol_state_os {
	minor_t		zso_minor;
	uint_t		zso_openmask;
	unsigned	zso_secsize;
	kmutex_t	zso_dklock;	/* disk_busy() and disk_unbusy() */
	struct disk	zso_disk;
};

static const struct dkdriver zvol_dkdriver = {
	.d_strategy = zvol_netbsd_strategy,
	.d_minphys = minphys,
};

/*
 * The geometry, in sectors of the pool's largest ashift as osnet's are,
 * so that a file system made on the volume does not write less than a
 * block of the pool at a time.
 */
static void
zvol_set_geom(zvol_state_t *zv)
{
	struct zvol_state_os *zso = zv->zv_zso;
	struct disk_geom *dg = &zso->zso_disk.dk_geom;

	memset(dg, 0, sizeof (*dg));
	dg->dg_secsize = zso->zso_secsize;
	dg->dg_secperunit = zv->zv_volsize / zso->zso_secsize;
	disk_set_info(NULL, &zso->zso_disk, "ZVOL");
}

static void
zvol_disk_busy(zvol_state_t *zv)
{

	mutex_enter(&zv->zv_zso->zso_dklock);
	disk_busy(&zv->zv_zso->zso_disk);
	mutex_exit(&zv->zv_zso->zso_dklock);
}

static void
zvol_disk_unbusy(zvol_state_t *zv, size_t bytes, boolean_t doread)
{

	mutex_enter(&zv->zv_zso->zso_dklock);
	disk_unbusy(&zv->zv_zso->zso_disk, bytes, doread);
	mutex_exit(&zv->zv_zso->zso_dklock);
}

static uint_t zvol_minors;

/*
 * The volumes by minor.  Looking one up holds zvol_minor_lock, which is
 * taken after zv_state_lock is let go; the volume cannot go while its
 * minor is in the table, for zvol_os_remove_minor() takes it out first.
 */
static kmutex_t zvol_minor_lock;
static zvol_state_t **zvol_minor_tbl;
static minor_t zvol_minor_cnt;

static minor_t
zvol_minor_alloc(zvol_state_t *zv)
{
	minor_t m;

	mutex_enter(&zvol_minor_lock);
	for (m = 1; m < zvol_minor_cnt; m++)
		if (zvol_minor_tbl[m] == NULL)
			break;
	if (m >= zvol_minor_cnt) {
		minor_t ncnt = MAX(16, zvol_minor_cnt * 2);
		zvol_state_t **ntbl;

		ntbl = kmem_zalloc(sizeof (*ntbl) * ncnt, KM_SLEEP);
		if (zvol_minor_tbl != NULL) {
			memcpy(ntbl, zvol_minor_tbl,
			    sizeof (*ntbl) * zvol_minor_cnt);
			kmem_free(zvol_minor_tbl,
			    sizeof (*ntbl) * zvol_minor_cnt);
		}
		zvol_minor_tbl = ntbl;
		zvol_minor_cnt = ncnt;
	}
	zvol_minor_tbl[m] = zv;
	mutex_exit(&zvol_minor_lock);
	return (m);
}

static void
zvol_minor_free(minor_t m)
{

	mutex_enter(&zvol_minor_lock);
	zvol_minor_tbl[m] = NULL;
	mutex_exit(&zvol_minor_lock);
}

/* The volume of a minor, with zv_state_lock held. */
static zvol_state_t *
zvol_minor_lookup(dev_t dev)
{
	minor_t m = minor(dev);
	zvol_state_t *zv = NULL;

	mutex_enter(&zvol_minor_lock);
	if (m > 0 && m < zvol_minor_cnt)
		zv = zvol_minor_tbl[m];
	if (zv != NULL)
		mutex_enter(&zv->zv_state_lock);
	mutex_exit(&zvol_minor_lock);
	return (zv);
}

/*
 * The nodes, made from the kernel as osnet's ddi_create_minor_node()
 * does: the directories above them first, then a node of the right kind
 * and number, put back if one of the wrong kind is there.
 */
static void
zvol_mkdirp(const char *path)
{
	char *p, *dir;

	dir = kmem_strdup(path);
	for (p = dir + 1; (p = strchr(p, '/')) != NULL; p++) {
		*p = '\0';
		(void) do_sys_mkdir(dir, 0755, UIO_SYSSPACE);
		*p = '/';
	}
	kmem_strfree(dir);
}

static int
zvol_mknod(const char *prefix, const char *name, mode_t type, dev_t dev)
{
	char *path;
	int error;

	path = kmem_asprintf("%s%s", prefix, name);
	zvol_mkdirp(path);
	(void) do_sys_unlink(path, UIO_SYSSPACE);
	error = do_sys_mknod(curlwp, path, type | S_IRUSR | S_IWUSR |
	    S_IRGRP, dev, UIO_SYSSPACE);
	kmem_strfree(path);
	return (error);
}

static void
zvol_rmnod(const char *prefix, const char *name)
{
	char *path;

	path = kmem_asprintf("%s%s", prefix, name);
	(void) do_sys_unlink(path, UIO_SYSSPACE);
	kmem_strfree(path);
}

static void
zvol_disk_init(zvol_state_t *zv, objset_t *os)
{
	struct zvol_state_os *zso;

	zso = kmem_zalloc(sizeof (*zso), KM_SLEEP);
	zso->zso_secsize = MAX(DEV_BSIZE,
	    1U << dmu_objset_spa(os)->spa_max_ashift);
	mutex_init(&zso->zso_dklock, NULL, MUTEX_DEFAULT, NULL);
	disk_init(&zso->zso_disk, zv->zv_name, &zvol_dkdriver);
	disk_attach(&zso->zso_disk);
	zv->zv_zso = zso;
	zvol_set_geom(zv);
}

static void
zvol_disk_fini(struct zvol_state_os *zso)
{

	disk_detach(&zso->zso_disk);
	disk_destroy(&zso->zso_disk);
	mutex_destroy(&zso->zso_dklock);
	kmem_free(zso, sizeof (*zso));
}

static int
zvol_mknods(const char *name, minor_t m)
{
	int error;

	error = zvol_mknod(ZVOL_NETBSD_RDSK, name, S_IFCHR,
	    makedev(zfsdev_cmajor, m));
	if (error == 0)
		error = zvol_mknod(ZVOL_NETBSD_DSK, name, S_IFBLK,
		    makedev(zfsdev_bmajor, m));
	if (error != 0)
		zvol_rmnod(ZVOL_NETBSD_RDSK, name);
	return (error);
}

static void
zvol_rmnods(const char *name)
{

	zvol_rmnod(ZVOL_NETBSD_RDSK, name);
	zvol_rmnod(ZVOL_NETBSD_DSK, name);
}

static void
zvol_ensure_zilog(zvol_state_t *zv)
{

	ASSERT(RW_READ_HELD(&zv->zv_suspend_lock));
	/*
	 * The intent log is opened at the first write, under the suspend
	 * lock, which is upgraded for it; see FreeBSD's.
	 */
	if (zv->zv_zilog == NULL) {
		if (!rw_tryupgrade(&zv->zv_suspend_lock)) {
			rw_exit(&zv->zv_suspend_lock);
			rw_enter(&zv->zv_suspend_lock, RW_WRITER);
		}
		if (zv->zv_zilog == NULL) {
			zv->zv_zilog = zil_open(zv->zv_objset,
			    zvol_get_data, &zv->zv_kstat.dk_zil_sums);
			zv->zv_flags |= ZVOL_WRITTEN_TO;
			VERIFY0(zv->zv_zilog->zl_header->zh_flags &
			    ZIL_REPLAY_NEEDED);
		}
		rw_downgrade(&zv->zv_suspend_lock);
	}
}

int
zvol_netbsd_open(dev_t dev, int flags, int fmt, struct lwp *l)
{
	zvol_state_t *zv;
	uint_t bit = (fmt == S_IFBLK) ? ZSO_OPEN_BLK : ZSO_OPEN_CHR;
	boolean_t drop_suspend = B_FALSE;
	int err = 0;

	(void) l;
retry:
	if ((zv = zvol_minor_lookup(dev)) == NULL)
		return (SET_ERROR(ENXIO));
	if (zv->zv_flags & ZVOL_REMOVING) {
		err = SET_ERROR(ENXIO);
		goto out_locked;
	}

	/*
	 * The first open holds off a suspension and takes the namespace
	 * lock for zvol_first_open(), in FreeBSD's order: the suspend
	 * lock before zv_state_lock, the namespace lock tried.
	 */
	if (zv->zv_open_count == 0) {
		drop_suspend = B_TRUE;
		if (!rw_tryenter(&zv->zv_suspend_lock, RW_READER)) {
			mutex_exit(&zv->zv_state_lock);
			rw_enter(&zv->zv_suspend_lock, RW_READER);
			mutex_enter(&zv->zv_state_lock);
			if (zv->zv_flags & ZVOL_REMOVING) {
				err = SET_ERROR(ENXIO);
				goto out_locked;
			}
			if (zv->zv_open_count != 0) {
				rw_exit(&zv->zv_suspend_lock);
				drop_suspend = B_FALSE;
			}
		}
	}
	if (zv->zv_open_count == 0) {
		boolean_t drop_namespace = B_FALSE;

		if (!spa_namespace_held()) {
			if (!spa_namespace_tryenter(FTAG)) {
				mutex_exit(&zv->zv_state_lock);
				rw_exit(&zv->zv_suspend_lock);
				drop_suspend = B_FALSE;
				kpause("zvolopen", false, 1, NULL);
				goto retry;
			}
			drop_namespace = B_TRUE;
		}
		err = zvol_first_open(zv, !(flags & FWRITE));
		if (drop_namespace)
			spa_namespace_exit(FTAG);
		if (err != 0)
			goto out_locked;
	}

	if ((flags & FWRITE) && (zv->zv_flags & ZVOL_RDONLY)) {
		err = SET_ERROR(EROFS);
		goto out_opened;
	}
	if (zv->zv_flags & ZVOL_EXCL) {
		err = SET_ERROR(EBUSY);
		goto out_opened;
	}
	if (flags & O_EXCL) {
		if (zv->zv_open_count != 0) {
			err = SET_ERROR(EBUSY);
			goto out_opened;
		}
		zv->zv_flags |= ZVOL_EXCL;
	}
	if ((zv->zv_zso->zso_openmask & bit) == 0) {
		zv->zv_zso->zso_openmask |= bit;
		zv->zv_open_count++;
	}
out_opened:
	if (zv->zv_open_count == 0)
		zvol_last_close(zv);
out_locked:
	mutex_exit(&zv->zv_state_lock);
	if (drop_suspend)
		rw_exit(&zv->zv_suspend_lock);
	return (err);
}

int
zvol_netbsd_close(dev_t dev, int flags, int fmt, struct lwp *l)
{
	zvol_state_t *zv;
	uint_t bit = (fmt == S_IFBLK) ? ZSO_OPEN_BLK : ZSO_OPEN_CHR;
	boolean_t drop_suspend = B_FALSE;

	(void) flags, (void) l;
	if ((zv = zvol_minor_lookup(dev)) == NULL)
		return (SET_ERROR(ENXIO));
	if ((zv->zv_zso->zso_openmask & bit) == 0) {
		mutex_exit(&zv->zv_state_lock);
		return (0);
	}
	if (zv->zv_open_count == 1) {
		drop_suspend = B_TRUE;
		if (!rw_tryenter(&zv->zv_suspend_lock, RW_READER)) {
			mutex_exit(&zv->zv_state_lock);
			rw_enter(&zv->zv_suspend_lock, RW_READER);
			mutex_enter(&zv->zv_state_lock);
			if (zv->zv_open_count != 1) {
				rw_exit(&zv->zv_suspend_lock);
				drop_suspend = B_FALSE;
			}
		}
	}
	zv->zv_zso->zso_openmask &= ~bit;
	zv->zv_flags &= ~ZVOL_EXCL;
	zv->zv_open_count--;
	if (zv->zv_open_count == 0) {
		ASSERT(RW_READ_HELD(&zv->zv_suspend_lock));
		zvol_last_close(zv);
		cv_broadcast(&zv->zv_removing_cv);
	}
	mutex_exit(&zv->zv_state_lock);
	if (drop_suspend)
		rw_exit(&zv->zv_suspend_lock);
	return (0);
}

/*
 * The open volume of a device, with only the suspend lock held for
 * reading: an open volume is not removed under its I/O.
 */
static zvol_state_t *
zvol_io_enter(dev_t dev)
{
	zvol_state_t *zv;

	if ((zv = zvol_minor_lookup(dev)) == NULL)
		return (NULL);
	if (zv->zv_open_count == 0) {
		mutex_exit(&zv->zv_state_lock);
		return (NULL);
	}
	mutex_exit(&zv->zv_state_lock);
	rw_enter(&zv->zv_suspend_lock, RW_READER);
	return (zv);
}

int
zvol_netbsd_read(dev_t dev, struct uio *uio_s, int ioflag)
{
	zvol_state_t *zv;
	zfs_locked_range_t *lr;
	uint64_t volsize;
	ssize_t start_resid;
	zfs_uio_t uio;
	int error = 0;

	(void) ioflag;
	if ((zv = zvol_io_enter(dev)) == NULL)
		return (SET_ERROR(ENXIO));
	zfs_uio_init(&uio, uio_s);
	volsize = zv->zv_volsize;
	/* Reading at the end is EOF, not an error. */
	if (zfs_uio_resid(&uio) > 0 &&
	    (zfs_uio_offset(&uio) < 0 || zfs_uio_offset(&uio) > volsize)) {
		rw_exit(&zv->zv_suspend_lock);
		return (SET_ERROR(EIO));
	}
	start_resid = zfs_uio_resid(&uio);
	zvol_disk_busy(zv);
	lr = zfs_rangelock_enter(&zv->zv_rangelock, zfs_uio_offset(&uio),
	    zfs_uio_resid(&uio), RL_READER);
	while (zfs_uio_resid(&uio) > 0 && zfs_uio_offset(&uio) < volsize) {
		uint64_t bytes = MIN(zfs_uio_resid(&uio), DMU_MAX_ACCESS >> 1);

		if (bytes > volsize - zfs_uio_offset(&uio))
			bytes = volsize - zfs_uio_offset(&uio);
		error = dmu_read_uio_dnode(zv->zv_dn, &uio, bytes,
		    DMU_READ_PREFETCH);
		if (error != 0) {
			if (error == ECKSUM)
				error = SET_ERROR(EIO);
			break;
		}
	}
	zfs_rangelock_exit(lr);
	zvol_disk_unbusy(zv, start_resid - zfs_uio_resid(&uio), B_TRUE);
	dataset_kstats_update_read_kstats(&zv->zv_kstat,
	    start_resid - zfs_uio_resid(&uio));
	rw_exit(&zv->zv_suspend_lock);
	return (error);
}

int
zvol_netbsd_write(dev_t dev, struct uio *uio_s, int ioflag)
{
	zvol_state_t *zv;
	zfs_locked_range_t *lr;
	uint64_t volsize;
	ssize_t start_resid;
	boolean_t commit;
	zfs_uio_t uio;
	int error = 0;

	if ((zv = zvol_io_enter(dev)) == NULL)
		return (SET_ERROR(ENXIO));
	zfs_uio_init(&uio, uio_s);
	volsize = zv->zv_volsize;
	if (zfs_uio_resid(&uio) > 0 &&
	    (zfs_uio_offset(&uio) < 0 || zfs_uio_offset(&uio) > volsize)) {
		rw_exit(&zv->zv_suspend_lock);
		return (SET_ERROR(EIO));
	}
	if (zv->zv_flags & ZVOL_RDONLY) {
		rw_exit(&zv->zv_suspend_lock);
		return (SET_ERROR(EROFS));
	}
	start_resid = zfs_uio_resid(&uio);
	commit = (ioflag & IO_SYNC) ||
	    zv->zv_objset->os_sync == ZFS_SYNC_ALWAYS;
	zvol_ensure_zilog(zv);
	zvol_disk_busy(zv);
	lr = zfs_rangelock_enter(&zv->zv_rangelock, zfs_uio_offset(&uio),
	    zfs_uio_resid(&uio), RL_WRITER);
	while (zfs_uio_resid(&uio) > 0 && zfs_uio_offset(&uio) < volsize) {
		uint64_t bytes = MIN(zfs_uio_resid(&uio), DMU_MAX_ACCESS >> 1);
		uint64_t off = zfs_uio_offset(&uio);
		dmu_tx_t *tx = dmu_tx_create(zv->zv_objset);

		if (bytes > volsize - off)
			bytes = volsize - off;
		dmu_tx_hold_write_by_dnode(tx, zv->zv_dn, off, bytes);
		if ((error = dmu_tx_assign(tx, DMU_TX_WAIT)) != 0) {
			dmu_tx_abort(tx);
			break;
		}
		error = dmu_write_uio_dnode(zv->zv_dn, &uio, bytes, tx,
		    DMU_READ_PREFETCH);
		if (error == 0)
			zvol_log_write(zv, tx, off, bytes, commit);
		dmu_tx_commit(tx);
		if (error != 0)
			break;
	}
	zfs_rangelock_exit(lr);
	zvol_disk_unbusy(zv, start_resid - zfs_uio_resid(&uio), B_FALSE);
	dataset_kstats_update_write_kstats(&zv->zv_kstat,
	    start_resid - zfs_uio_resid(&uio));
	if (error == 0 && commit)
		error = zil_commit(zv->zv_zilog, ZVOL_OBJ);
	rw_exit(&zv->zv_suspend_lock);
	return (error);
}

/* Free a range of the volume, as a discard or a TRIM asks. */
static int
zvol_free_range(zvol_state_t *zv, uint64_t off, uint64_t len)
{
	zfs_locked_range_t *lr;
	dmu_tx_t *tx;
	int error;

	if (zv->zv_flags & ZVOL_RDONLY)
		return (SET_ERROR(EROFS));
	if (off >= zv->zv_volsize)
		return (0);
	len = MIN(len, zv->zv_volsize - off);
	zvol_ensure_zilog(zv);
	lr = zfs_rangelock_enter(&zv->zv_rangelock, off, len, RL_WRITER);
	tx = dmu_tx_create(zv->zv_objset);
	dmu_tx_mark_netfree(tx);
	if ((error = dmu_tx_assign(tx, DMU_TX_WAIT)) != 0) {
		dmu_tx_abort(tx);
	} else {
		zvol_log_truncate(zv, tx, off, len);
		dmu_tx_commit(tx);
		error = dmu_free_long_range(zv->zv_objset, ZVOL_OBJ, off, len);
	}
	zfs_rangelock_exit(lr);
	if (error == 0 && zv->zv_objset->os_sync == ZFS_SYNC_ALWAYS)
		error = zil_commit(zv->zv_zilog, ZVOL_OBJ);
	return (error);
}

/* A block device's buf, done here: reads and writes of the DMU. */
void
zvol_netbsd_strategy(struct buf *bp)
{
	zvol_state_t *zv;
	zfs_locked_range_t *lr;
	boolean_t doread = (bp->b_flags & B_READ) != 0;
	boolean_t commit;
	uint64_t off, volsize;
	size_t resid;
	char *addr;
	int error = 0;

	if ((zv = zvol_io_enter(bp->b_dev)) == NULL) {
		bp->b_error = ENXIO;
		bp->b_resid = bp->b_bcount;
		biodone(bp);
		return;
	}
	off = (uint64_t)bp->b_blkno << DEV_BSHIFT;
	volsize = zv->zv_volsize;
	addr = bp->b_data;
	resid = bp->b_bcount;
	if (!doread && (zv->zv_flags & ZVOL_RDONLY)) {
		error = SET_ERROR(EROFS);
		goto out;
	}
	if (resid > 0 && off >= volsize) {
		error = SET_ERROR(EIO);
		goto out;
	}
	if (!doread)
		zvol_ensure_zilog(zv);
	commit = !doread && zv->zv_objset->os_sync == ZFS_SYNC_ALWAYS;
	zvol_disk_busy(zv);
	lr = zfs_rangelock_enter(&zv->zv_rangelock, off, resid,
	    doread ? RL_READER : RL_WRITER);
	while (resid != 0 && off < volsize) {
		size_t size = MIN(resid, MIN(volsize - off,
		    DMU_MAX_ACCESS >> 1));

		if (doread) {
			error = dmu_read_by_dnode(zv->zv_dn, off, size, addr,
			    DMU_READ_PREFETCH);
		} else {
			dmu_tx_t *tx = dmu_tx_create(zv->zv_objset);

			dmu_tx_hold_write_by_dnode(tx, zv->zv_dn, off, size);
			if ((error = dmu_tx_assign(tx, DMU_TX_WAIT)) != 0) {
				dmu_tx_abort(tx);
			} else {
				dmu_write_by_dnode(zv->zv_dn, off, size, addr,
				    tx, DMU_READ_PREFETCH);
				zvol_log_write(zv, tx, off, size, commit);
				dmu_tx_commit(tx);
			}
		}
		if (error != 0) {
			if (error == ECKSUM)
				error = SET_ERROR(EIO);
			break;
		}
		off += size;
		addr += size;
		resid -= size;
	}
	zfs_rangelock_exit(lr);
	zvol_disk_unbusy(zv, bp->b_bcount - resid, doread);
	if (doread)
		dataset_kstats_update_read_kstats(&zv->zv_kstat,
		    bp->b_bcount - resid);
	else
		dataset_kstats_update_write_kstats(&zv->zv_kstat,
		    bp->b_bcount - resid);
	if (error == 0 && commit)
		error = zil_commit(zv->zv_zilog, ZVOL_OBJ);
out:
	rw_exit(&zv->zv_suspend_lock);
	bp->b_error = error;
	bp->b_resid = (error != 0) ? bp->b_bcount : resid;
	biodone(bp);
}

int
zvol_netbsd_discard(dev_t dev, off_t pos, off_t len)
{
	zvol_state_t *zv;
	int error;

	if ((zv = zvol_io_enter(dev)) == NULL)
		return (SET_ERROR(ENXIO));
	error = (pos < 0 || len < 0) ? SET_ERROR(EINVAL) :
	    zvol_free_range(zv, pos, len);
	rw_exit(&zv->zv_suspend_lock);
	return (error);
}

/* The size in DEV_BSIZE blocks, as swapping on the volume asks. */
int
zvol_netbsd_psize(dev_t dev)
{
	zvol_state_t *zv;
	uint64_t size;

	if ((zv = zvol_minor_lookup(dev)) == NULL)
		return (-1);
	size = zv->zv_volsize >> DEV_BSHIFT;
	mutex_exit(&zv->zv_state_lock);
	return (size > INT_MAX ? INT_MAX : (int)size);
}

int
zvol_netbsd_ioctl(dev_t dev, u_long cmd, void *data, int flag,
    struct lwp *l)
{
	zvol_state_t *zv;
	int error = 0;

	if ((zv = zvol_io_enter(dev)) == NULL)
		return (SET_ERROR(ENXIO));
	error = disk_ioctl(&zv->zv_zso->zso_disk, NODEV, cmd, data, flag, l);
	if (error != EPASSTHROUGH) {
		rw_exit(&zv->zv_suspend_lock);
		return (error);
	}
	error = 0;
	switch (cmd) {
	case DIOCGWEDGEINFO: {
		struct dkwedge_info *dkw = data;
		struct disk_geom *dg = &zv->zv_zso->zso_disk.dk_geom;

		/*
		 * A whole volume, as a wedge of a parent that is no disk:
		 * osnet's "ZFS", which no device is called, so that the
		 * kernel asks DIOCGPARTINFO below and userland
		 * DIOCGDISKINFO.
		 */
		memset(dkw, 0, sizeof (*dkw));
		strlcpy(dkw->dkw_devname, zv->zv_name,
		    sizeof (dkw->dkw_devname));
		strlcpy(dkw->dkw_parent, "ZFS", sizeof (dkw->dkw_parent));
		dkw->dkw_offset = 0;
		dkw->dkw_size = dg->dg_secperunit;
		strlcpy(dkw->dkw_ptype, DKW_PTYPE_FFS,
		    sizeof (dkw->dkw_ptype));
		break;
	}
	case DIOCGPARTINFO: {
		struct partinfo *pi = data;
		struct disk_geom *dg = &zv->zv_zso->zso_disk.dk_geom;

		memset(pi, 0, sizeof (*pi));
		pi->pi_offset = 0;
		pi->pi_secsize = dg->dg_secsize;
		pi->pi_size = dg->dg_secperunit;
		pi->pi_fstype = FS_OTHER;
		pi->pi_bsize = MAX(BLKDEV_IOSIZE, pi->pi_secsize);
		break;
	}
	case DIOCCACHESYNC:
		if (zv->zv_zilog != NULL)
			error = zil_commit(zv->zv_zilog, ZVOL_OBJ);
		break;
	default:
		error = SET_ERROR(ENOTTY);
		break;
	}
	rw_exit(&zv->zv_suspend_lock);
	return (error);
}

boolean_t
zvol_os_is_zvol(const char *device)
{

	return (device != NULL &&
	    strncmp(device, "/dev/zvol/", 10) == 0);
}

int
zvol_os_rename_minor(zvol_state_t *zv, const char *newname)
{
	int error;

	ASSERT(RW_WRITE_HELD(&zvol_state_lock));
	ASSERT(MUTEX_HELD(&zv->zv_state_lock));

	zvol_rmnods(zv->zv_name);
	zv->zv_hash = zvol_name_hash(newname);
	hlist_del(&zv->zv_hlink);
	hlist_add_head(&zv->zv_hlink, ZVOL_HT_HEAD(zv->zv_hash));
	strlcpy(zv->zv_name, newname, sizeof (zv->zv_name));
	dataset_kstats_rename(&zv->zv_kstat, newname);
	mutex_enter(&zv->zv_zso->zso_dklock);
	disk_rename(&zv->zv_zso->zso_disk, zv->zv_name);
	mutex_exit(&zv->zv_zso->zso_dklock);
	error = zvol_mknods(newname, zv->zv_zso->zso_minor);
	return (error);
}

void
zvol_os_remove_minor(zvol_state_t *zv)
{
	struct zvol_state_os *zso = zv->zv_zso;

	ASSERT(MUTEX_HELD(&zv->zv_state_lock));
	ASSERT0(zv->zv_open_count);
	ASSERT(zv->zv_flags & ZVOL_REMOVING);

	zv->zv_zso = NULL;
	mutex_exit(&zv->zv_state_lock);
	zvol_minor_free(zso->zso_minor);
	zvol_rmnods(zv->zv_name);
	zvol_disk_fini(zso);
	mutex_enter(&zv->zv_state_lock);
}

void
zvol_os_free(zvol_state_t *zv)
{

	ASSERT(!RW_LOCK_HELD(&zv->zv_suspend_lock));
	ASSERT(!MUTEX_HELD(&zv->zv_state_lock));
	ASSERT0(zv->zv_open_count);
	ASSERT0P(zv->zv_zso);
	ASSERT0P(zv->zv_objset);
	ASSERT0P(zv->zv_zilog);
	ASSERT0P(zv->zv_dn);

	rw_destroy(&zv->zv_suspend_lock);
	zfs_rangelock_fini(&zv->zv_rangelock);
	mutex_destroy(&zv->zv_state_lock);
	cv_destroy(&zv->zv_removing_cv);
	dataset_kstats_destroy(&zv->zv_kstat);
	kmem_free(zv, sizeof (zvol_state_t));
	atomic_dec_uint(&zvol_minors);
}

/*
 * Make the device of a volume: what FreeBSD's does, with a minor and
 * two nodes for its cdev.  volmode=none has none; "geom", FreeBSD's
 * default, is taken as "dev", the one kind there is here.
 */
int
zvol_os_create_minor(const char *name)
{
	zvol_state_t *zv;
	objset_t *os;
	dmu_object_info_t *doi;
	uint64_t volsize, volmode, hash, len;
	boolean_t replayed_zil = B_FALSE;
	int error;

	if (zvol_inhibit_dev)
		return (0);
	hash = zvol_name_hash(name);
	if ((zv = zvol_find_by_name_hash(name, hash, RW_NONE)) != NULL) {
		ASSERT(MUTEX_HELD(&zv->zv_state_lock));
		mutex_exit(&zv->zv_state_lock);
		return (SET_ERROR(EEXIST));
	}
	if ((error = dsl_prop_get_integer(name,
	    zfs_prop_to_name(ZFS_PROP_VOLMODE), &volmode, NULL)) != 0)
		return (error);
	if (volmode == ZFS_VOLMODE_DEFAULT)
		volmode = zvol_volmode;
	if (volmode == ZFS_VOLMODE_NONE)
		return (0);

	doi = kmem_alloc(sizeof (dmu_object_info_t), KM_SLEEP);
	/* Owned read-only to look; the first open owns it as it means. */
	if ((error = dmu_objset_own(name, DMU_OST_ZVOL, B_TRUE, B_TRUE, FTAG,
	    &os)) != 0)
		goto out_doi;
	if ((error = dmu_object_info(os, ZVOL_OBJ, doi)) != 0 ||
	    (error = zap_lookup(os, ZVOL_ZAP_OBJ, "size", 8, 1,
	    &volsize)) != 0)
		goto out_disown;

	zv = kmem_zalloc(sizeof (*zv), KM_SLEEP);
	mutex_init(&zv->zv_state_lock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&zv->zv_removing_cv, NULL, CV_DEFAULT, NULL);
	rw_init(&zv->zv_suspend_lock, NULL, RW_DEFAULT, NULL);
	zfs_rangelock_init(&zv->zv_rangelock, NULL, NULL);
	zv->zv_volmode = ZFS_VOLMODE_DEV;
	zv->zv_volsize = volsize;
	zv->zv_volblocksize = doi->doi_data_block_size;
	zv->zv_hash = hash;
	(void) strlcpy(zv->zv_name, name, sizeof (zv->zv_name));
	zvol_disk_init(zv, os);
	if (dmu_objset_is_snapshot(os) || !spa_writeable(dmu_objset_spa(os)))
		zv->zv_flags |= ZVOL_RDONLY;

	zv->zv_objset = os;
	VERIFY0(dataset_kstats_create(&zv->zv_kstat, zv->zv_objset));
	zv->zv_zilog = zil_open(os, zvol_get_data, &zv->zv_kstat.dk_zil_sums);
	if (spa_writeable(dmu_objset_spa(os))) {
		if (zil_replay_disable)
			replayed_zil = zil_destroy(zv->zv_zilog, B_FALSE);
		else
			replayed_zil = zil_replay(os, zv, zvol_replay_vector);
	}
	if (replayed_zil)
		zil_close(zv->zv_zilog);
	zv->zv_zilog = NULL;
	len = MIN(zvol_prefetch_bytes, SPA_MAXBLOCKSIZE);
	if (len > 0) {
		dmu_prefetch(os, ZVOL_OBJ, 0, 0, len, ZIO_PRIORITY_ASYNC_READ);
		dmu_prefetch(os, ZVOL_OBJ, 0, volsize - len, len,
		    ZIO_PRIORITY_ASYNC_READ);
	}
	zv->zv_objset = NULL;

	zv->zv_zso->zso_minor = zvol_minor_alloc(zv);
	if ((error = zvol_mknods(name, zv->zv_zso->zso_minor)) != 0) {
		zvol_minor_free(zv->zv_zso->zso_minor);
		zvol_disk_fini(zv->zv_zso);
		zv->zv_zso = NULL;
		dmu_objset_disown(os, B_TRUE, FTAG);
		zvol_os_free(zv);
		atomic_inc_uint(&zvol_minors);	/* zvol_os_free() took one */
		goto out_doi;
	}
out_disown:
	dmu_objset_disown(os, B_TRUE, FTAG);
	if (error == 0) {
		rw_enter(&zvol_state_lock, RW_WRITER);
		zvol_insert(zv);
		atomic_inc_uint(&zvol_minors);
		rw_exit(&zvol_state_lock);
	}
out_doi:
	kmem_free(doi, sizeof (dmu_object_info_t));
	return (error);
}

int
zvol_os_update_volsize(zvol_state_t *zv, uint64_t volsize)
{

	zv->zv_volsize = volsize;
	if (zv->zv_zso != NULL)
		zvol_set_geom(zv);
	return (0);
}

void
zvol_os_set_disk_ro(zvol_state_t *zv, int flags)
{

	(void) zv, (void) flags;
}

void
zvol_os_set_capacity(zvol_state_t *zv, uint64_t capacity)
{

	(void) zv, (void) capacity;
}

/* Wait, a while, for the last close of a volume being removed. */
void
zvol_wait_close(zvol_state_t *zv)
{

	mutex_enter(&zv->zv_state_lock);
	if (zv->zv_open_count != 0)
		(void) cv_timedwait(&zv->zv_removing_cv, &zv->zv_state_lock,
		    ddi_get_lbolt() + SEC_TO_TICK(10));
	mutex_exit(&zv->zv_state_lock);
}

int
zvol_busy(void)
{

	return (zvol_minors != 0);
}

int
zvol_init(void)
{

	mutex_init(&zvol_minor_lock, NULL, MUTEX_DEFAULT, NULL);
	return (zvol_init_impl());
}

void
zvol_fini(void)
{

	zvol_fini_impl();
	if (zvol_minor_tbl != NULL)
		kmem_free(zvol_minor_tbl, sizeof (*zvol_minor_tbl) *
		    zvol_minor_cnt);
	zvol_minor_tbl = NULL;
	zvol_minor_cnt = 0;
	mutex_destroy(&zvol_minor_lock);
}
