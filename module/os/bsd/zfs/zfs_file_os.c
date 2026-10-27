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
 * Files the module reads and writes: a file vdev or the pool cache,
 * which it opens by path, and a descriptor a process hands it, for
 * zfs send and receive and for the event and on-exit descriptors of
 * /dev/zfs.
 *
 * A file opened here has no file_t: NetBSD makes one only in a
 * process's descriptor table, and the process asking for a file vdev
 * to be opened is not the one that should find it there.  So a
 * zfs_file_t is either a vnode with an offset of its own, or a
 * descriptor's file_t, held from fd_getfile() to fd_putfile().
 */

#include <sys/zfs_context.h>
#include <sys/zfs_file.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/namei.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <sys/vfs_syscalls.h>

struct zfs_file {
	struct vnode	*zf_vp;		/* opened by path */
	int		zf_flag;	/* its FREAD and FWRITE */
	off_t		zf_offset;	/* its offset */
	kauth_cred_t	zf_cred;	/* who opened it */
	file_t		*zf_fp;		/* a descriptor's file */
	int		zf_fd;		/* that descriptor */
};

/* Declared by zfs_ioctl_os.c: the file a process opens /dev/zfs as. */
extern const struct fileops zfsdev_fileops;

int
zfs_file_open(const char *path, int flags, int mode, cred_t *cr,
    zfs_file_t **fpp)
{
	struct pathbuf *pb;
	struct vnode *vp;
	zfs_file_t *zf;
	int fflags, error;

	fflags = FFLAGS(flags);
	if ((pb = pathbuf_create(path)) == NULL)
		return (SET_ERROR(ENOMEM));
	error = vn_open(NULL, pb, 0, fflags, mode, &vp, NULL, NULL);
	pathbuf_destroy(pb);
	if (error != 0)
		return (SET_ERROR(error));
	if (vp->v_type != VREG) {
		VOP_UNLOCK(vp);
		(void) vn_close(vp, fflags, cr);
		return (SET_ERROR(EACCES));
	}
	VOP_UNLOCK(vp);

	zf = kmem_zalloc(sizeof (*zf), KM_SLEEP);
	zf->zf_vp = vp;
	zf->zf_flag = fflags & (FREAD | FWRITE);
	zf->zf_fd = -1;
	kauth_cred_hold(cr);
	zf->zf_cred = cr;
	*fpp = zf;
	return (0);
}

void
zfs_file_close(zfs_file_t *zf)
{

	ASSERT3P(zf->zf_vp, !=, NULL);
	(void) vn_close(zf->zf_vp, zf->zf_flag, zf->zf_cred);
	kauth_cred_free(zf->zf_cred);
	kmem_free(zf, sizeof (*zf));
}

static int
zfs_file_rdwr(zfs_file_t *zf, enum uio_rw rw, void *buf, size_t count,
    off_t *offp, int fofflags, ssize_t *resid)
{
	struct iovec aiov;
	struct uio auio;
	int error;

	if (((zf->zf_fp != NULL ? zf->zf_fp->f_flag : zf->zf_flag) &
	    (rw == UIO_READ ? FREAD : FWRITE)) == 0)
		return (SET_ERROR(EBADF));

	aiov.iov_base = buf;
	aiov.iov_len = count;
	auio.uio_iov = &aiov;
	auio.uio_iovcnt = 1;
	auio.uio_resid = count;
	auio.uio_rw = rw;
	auio.uio_offset = *offp;
	UIO_SETUP_SYSSPACE(&auio);

	if (zf->zf_fp != NULL) {
		file_t *fp = zf->zf_fp;

		if (rw == UIO_READ)
			error = (*fp->f_ops->fo_read)(fp, offp, &auio,
			    fp->f_cred, fofflags);
		else
			error = (*fp->f_ops->fo_write)(fp, offp, &auio,
			    fp->f_cred, fofflags);
	} else {
		struct vnode *vp = zf->zf_vp;

		vn_lock(vp, (rw == UIO_READ ? LK_SHARED : LK_EXCLUSIVE) |
		    LK_RETRY);
		if (rw == UIO_READ)
			error = VOP_READ(vp, &auio, 0, zf->zf_cred);
		else
			error = VOP_WRITE(vp, &auio, 0, zf->zf_cred);
		VOP_UNLOCK(vp);
		if (fofflags & FOF_UPDATE_OFFSET)
			*offp = auio.uio_offset;
	}
	if (error != 0)
		return (SET_ERROR(error));
	if (resid != NULL)
		*resid = auio.uio_resid;
	else if (rw == UIO_WRITE && auio.uio_resid != 0)
		return (SET_ERROR(EIO));
	return (0);
}

static off_t *
zfs_file_offp(zfs_file_t *zf)
{

	return (zf->zf_fp != NULL ? &zf->zf_fp->f_offset : &zf->zf_offset);
}

int
zfs_file_write(zfs_file_t *zf, const void *buf, size_t count, ssize_t *resid)
{

	return (zfs_file_rdwr(zf, UIO_WRITE, __UNCONST(buf), count,
	    zfs_file_offp(zf), FOF_UPDATE_OFFSET, resid));
}

int
zfs_file_pwrite(zfs_file_t *zf, const void *buf, size_t count, loff_t off,
    uint8_t ashift, ssize_t *resid)
{
	off_t o = off;

	(void) ashift;
	return (zfs_file_rdwr(zf, UIO_WRITE, __UNCONST(buf), count, &o, 0,
	    resid));
}

int
zfs_file_read(zfs_file_t *zf, void *buf, size_t count, ssize_t *resid)
{

	return (zfs_file_rdwr(zf, UIO_READ, buf, count, zfs_file_offp(zf),
	    FOF_UPDATE_OFFSET, resid));
}

int
zfs_file_pread(zfs_file_t *zf, void *buf, size_t count, loff_t off,
    ssize_t *resid)
{
	off_t o = off;

	return (zfs_file_rdwr(zf, UIO_READ, buf, count, &o, 0, resid));
}

static struct vnode *
zfs_file_vnode(zfs_file_t *zf)
{

	if (zf->zf_vp != NULL)
		return (zf->zf_vp);
	if (zf->zf_fp->f_type == DTYPE_VNODE)
		return (zf->zf_fp->f_vnode);
	return (NULL);
}

int
zfs_file_getattr(zfs_file_t *zf, zfs_file_attr_t *zfattr)
{
	struct vnode *vp;
	struct vattr va;
	struct stat sb;
	int error;

	if (zf->zf_fp != NULL) {
		error = (*zf->zf_fp->f_ops->fo_stat)(zf->zf_fp, &sb);
		if (error != 0)
			return (SET_ERROR(error));
		zfattr->zfa_size = sb.st_size;
		zfattr->zfa_mode = sb.st_mode;
		return (0);
	}
	vp = zf->zf_vp;
	vn_lock(vp, LK_SHARED | LK_RETRY);
	error = VOP_GETATTR(vp, &va, zf->zf_cred);
	VOP_UNLOCK(vp);
	if (error != 0)
		return (SET_ERROR(error));
	zfattr->zfa_size = va.va_size;
	zfattr->zfa_mode = VTTOIF(va.va_type) | va.va_mode;
	return (0);
}

int
zfs_file_fsync(zfs_file_t *zf, int flags)
{
	struct vnode *vp;
	int error;

	(void) flags;
	if ((vp = zfs_file_vnode(zf)) == NULL)
		return (SET_ERROR(EINVAL));
	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
	error = VOP_FSYNC(vp, zf->zf_vp != NULL ? zf->zf_cred :
	    zf->zf_fp->f_cred, FSYNC_WAIT, 0, 0);
	VOP_UNLOCK(vp);
	return (SET_ERROR(error));
}

/*
 * Free the storage under a range of the file, as fallocate(2)'s
 * FALLOC_FL_PUNCH_HOLE does; NetBSD's is VOP_FDISCARD, which most of
 * its file systems answer with EOPNOTSUPP.
 */
int
zfs_file_deallocate(zfs_file_t *zf, loff_t offset, loff_t len)
{
	struct vnode *vp;
	int error;

	if ((vp = zfs_file_vnode(zf)) == NULL)
		return (SET_ERROR(EOPNOTSUPP));
	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
	error = VOP_FDISCARD(vp, offset, len);
	VOP_UNLOCK(vp);
	return (SET_ERROR(error));
}

/*
 * A descriptor of the calling process.  fd_putfile() has to be called
 * by the same thread as fd_getfile(), which the callers do: each takes
 * and lets go of its descriptor within one ioctl.
 */
zfs_file_t *
zfs_file_get(int fd)
{
	zfs_file_t *zf;
	file_t *fp;

	if ((fp = fd_getfile(fd)) == NULL)
		return (NULL);
	zf = kmem_zalloc(sizeof (*zf), KM_SLEEP);
	zf->zf_fp = fp;
	zf->zf_fd = fd;
	return (zf);
}

void
zfs_file_put(zfs_file_t *zf)
{

	ASSERT3P(zf->zf_fp, !=, NULL);
	fd_putfile(zf->zf_fd);
	kmem_free(zf, sizeof (*zf));
}

loff_t
zfs_file_off(zfs_file_t *zf)
{

	return (*zfs_file_offp(zf));
}

/* The state of the /dev/zfs open the descriptor is, if it is one. */
void *
zfs_file_private(zfs_file_t *zf)
{

	if (zf->zf_fp == NULL || zf->zf_fp->f_ops != &zfsdev_fileops)
		return (NULL);
	return (zf->zf_fp->f_data);
}

int
zfs_file_unlink(const char *path)
{

	return (SET_ERROR(do_sys_unlink(path, UIO_SYSSPACE)));
}
