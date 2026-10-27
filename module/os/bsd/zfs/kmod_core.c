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
 * The module: its modcmd, and /dev/zfs.
 *
 * Each open of /dev/zfs is a file of its own, made with fd_clone(), so
 * that the per-open state the common code keeps -- the zevent cursor,
 * the on-exit actions -- lives as long as that descriptor and is torn
 * down by its close, which a plain cdevsw close would not tell apart
 * from the others.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/zfs_context.h>
#include <sys/zfs_ioctl.h>
#include <sys/zfs_vfsops.h>
#include <sys/zfs_ioctl_impl.h>
#include <sys/zfs_ioctl_compat.h>
#include <sys/spa.h>
#include <sys/zio.h>
#include <sys/zvol.h>
#include <sys/zfs_netbsd_dev.h>
#include <zfs_gitrev.h>

MODULE(MODULE_CLASS_DRIVER, zfs, "zlib");

extern int zcommon_init(void);
extern void zcommon_fini(void);
extern int icp_init(void);
extern void icp_fini(void);
extern int zstd_init(void);
extern void zstd_fini(void);
extern void spl_taskq_init(void);
extern void spl_taskq_fini(void);
extern void spl_sysctl_fini(void);
extern struct vfsops zfs_vfsops;

static int zfsdev_fop_ioctl(file_t *, u_long, void *);
static int zfsdev_fop_close(file_t *);

const struct fileops zfsdev_fileops = {
	.fo_name = "zfs",
	.fo_read = fbadop_read,
	.fo_write = fbadop_write,
	.fo_ioctl = zfsdev_fop_ioctl,
	.fo_fcntl = fnullop_fcntl,
	.fo_poll = fnullop_poll,
	.fo_stat = fbadop_stat,
	.fo_close = zfsdev_fop_close,
	.fo_kqfilter = fnullop_kqfilter,
	.fo_restart = fnullop_restart,
};

static dev_type_open(zfsdev_open);
static dev_type_close(zfsdev_close);
static dev_type_read(zfsdev_read);
static dev_type_write(zfsdev_write);
static dev_type_ioctl(zfsdev_ioctl);
static dev_type_discard(zfsdev_discard);
static dev_type_open(zfsdev_bopen);
static dev_type_close(zfsdev_bclose);

devmajor_t zfsdev_bmajor = NODEVMAJOR, zfsdev_cmajor = NODEVMAJOR;

/*
 * conf/majors gives zfs a block major as well as a character one, the
 * same number, for the volumes, and devsw_attach() takes the name only
 * with both.  Until volumes are disks, opening one fails.
 */
static const struct bdevsw zfs_bdevsw = {
	.d_open = zfsdev_bopen,
	.d_close = zfsdev_bclose,
	.d_strategy = zvol_netbsd_strategy,
	.d_ioctl = zvol_netbsd_ioctl,
	.d_dump = nodump,
	.d_psize = zvol_netbsd_psize,
	.d_discard = zvol_netbsd_discard,
	.d_flag = D_DISK | D_MPSAFE,
};

static const struct cdevsw zfs_cdevsw = {
	.d_open = zfsdev_open,
	.d_close = zfsdev_close,
	.d_read = zfsdev_read,
	.d_write = zfsdev_write,
	.d_ioctl = zfsdev_ioctl,
	.d_stop = nostop,
	.d_tty = notty,
	.d_poll = nopoll,
	.d_mmap = nommap,
	.d_kqfilter = nokqfilter,
	.d_discard = zfsdev_discard,
	.d_flag = D_OTHER | D_MPSAFE,
};

/* zfsdev_state_init() hands the state here; it becomes the file's. */
void
zfsdev_private_set_state(void *priv, zfsdev_state_t *zs)
{

	*(zfsdev_state_t **)priv = zs;
}

zfsdev_state_t *
zfsdev_private_get_state(void *priv)
{

	return (priv);
}

static int
zfsdev_open(dev_t dev, int flag, int mode, struct lwp *l)
{
	zfsdev_state_t *zs = NULL;
	file_t *fp;
	int error, fd;

	if (minor(dev) != 0)
		return (zvol_netbsd_open(dev, flag, mode, l));
	if ((error = fd_allocfile(&fp, &fd)) != 0)
		return (error);
	mutex_enter(&zfsdev_state_lock);
	error = zfsdev_state_init(&zs);
	mutex_exit(&zfsdev_state_lock);
	if (error != 0) {
		fd_abort(curproc, fp, fd);
		return (error);
	}
	return (fd_clone(fp, fd, flag, &zfsdev_fileops, zs));
}

/*
 * Minor 0 never gets here past its open: fd_clone() gives that open a
 * file of its own.  The rest are volumes.
 */
static int
zfsdev_close(dev_t dev, int flag, int mode, struct lwp *l)
{

	return (zvol_netbsd_close(dev, flag, mode, l));
}

static int
zfsdev_read(dev_t dev, struct uio *uio, int ioflag)
{

	return (zvol_netbsd_read(dev, uio, ioflag));
}

static int
zfsdev_write(dev_t dev, struct uio *uio, int ioflag)
{

	return (zvol_netbsd_write(dev, uio, ioflag));
}

static int
zfsdev_ioctl(dev_t dev, u_long cmd, void *data, int flag, struct lwp *l)
{

	return (zvol_netbsd_ioctl(dev, cmd, data, flag, l));
}

static int
zfsdev_discard(dev_t dev, off_t pos, off_t len)
{

	return (zvol_netbsd_discard(dev, pos, len));
}

static int
zfsdev_bopen(dev_t dev, int flag, int mode, struct lwp *l)
{

	if (minor(dev) == 0)
		return (SET_ERROR(ENXIO));
	return (zvol_netbsd_open(dev, flag, mode, l));
}

static int
zfsdev_bclose(dev_t dev, int flag, int mode, struct lwp *l)
{

	return (zvol_netbsd_close(dev, flag, mode, l));
}

static int
zfsdev_fop_close(file_t *fp)
{

	zfsdev_state_destroy(fp->f_data);
	return (0);
}

/*
 * sys_ioctl() has copied the zfs_iocparm_t in; the zfs_cmd_t it names
 * is copied here, and back out whether or not the command failed, for
 * a command reports some failures in it.
 */
static int
zfsdev_fop_ioctl(file_t *fp, u_long cmd, void *data)
{
	zfs_iocparm_t *zp = data;
	zfs_cmd_t *zc;
	void *uaddr;
	int error, rc;

	(void) fp;
	if (IOCGROUP(cmd) != 'Z' || IOCPARM_LEN(cmd) != sizeof (*zp))
		return (SET_ERROR(ENOTTY));
	if (zp->zfs_ioctl_version != ZFS_IOCVER_OZFS ||
	    zp->zfs_cmd_size != sizeof (zfs_cmd_t))
		return (SET_ERROR(ENOTSUP));

	uaddr = (void *)(uintptr_t)zp->zfs_cmd;
	zc = vmem_zalloc(sizeof (zfs_cmd_t), KM_SLEEP);
	if (copyin(uaddr, zc, sizeof (zfs_cmd_t)) != 0) {
		error = SET_ERROR(EFAULT);
		goto out;
	}
	error = zfsdev_ioctl_common(ZFS_IOCREQ(cmd), zc, 0);
	rc = copyout(zc, uaddr, sizeof (zfs_cmd_t));
	if (error == 0 && rc != 0)
		error = SET_ERROR(EFAULT);
out:
	vmem_free(zc, sizeof (zfs_cmd_t));
	return (error);
}

int
zfsdev_attach(void)
{

	return (devsw_attach(ZFS_DRIVER, &zfs_bdevsw, &zfsdev_bmajor,
	    &zfs_cdevsw, &zfsdev_cmajor));
}

void
zfsdev_detach(void)
{

	devsw_detach(&zfs_bdevsw, &zfs_cdevsw);
}

static int
zfs_modinit(void)
{
	int error;

	spl_taskq_init();
	if ((error = zcommon_init()) != 0)
		goto zcommon_failed;
	if ((error = icp_init()) != 0)
		goto icp_failed;
	if ((error = zstd_init()) != 0)
		goto zstd_failed;
	if ((error = zfs_kmod_init()) != 0)
		goto kmod_failed;
	if ((error = vfs_attach(&zfs_vfsops)) != 0) {
		zfs_kmod_fini();
		goto kmod_failed;
	}
	printf("ZFS filesystem version: " ZPL_VERSION_STRING "\n");
	printf("ZFS storage pool version: features support ("
	    SPA_VERSION_STRING "), " ZFS_META_GITREV "\n");
	return (0);

kmod_failed:
	zstd_fini();
zstd_failed:
	icp_fini();
icp_failed:
	zcommon_fini();
zcommon_failed:
	spl_taskq_fini();
	return (error);
}

static int
zfs_modfini(void)
{

	if (zfs_busy() || zvol_busy() || zio_injection_enabled)
		return (SET_ERROR(EBUSY));
	if (vfs_detach(&zfs_vfsops) != 0)
		return (SET_ERROR(EBUSY));
	zfs_kmod_fini();
	zstd_fini();
	icp_fini();
	zcommon_fini();
	spl_taskq_fini();
	spl_sysctl_fini();
	return (0);
}

static int
zfs_modcmd(modcmd_t cmd, void *arg)
{

	(void) arg;
	switch (cmd) {
	case MODULE_CMD_INIT:
		return (zfs_modinit());
	case MODULE_CMD_FINI:
		return (zfs_modfini());
	default:
		return (ENOTTY);
	}
}
