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

#ifndef _SPL_SYS_UIO_H
#define	_SPL_SYS_UIO_H

/*
 * zfs_uio_t over NetBSD's struct uio.  It either wraps a caller's uio,
 * as the vnode operations hand one in, or carries one of its own, as
 * the encryption code builds one from a zeroed zfs_uio_t: zio_crypt_os_icp.c
 * fills in the iovecs and the segment directly.  The segment is kept
 * here, because NetBSD's uio has no segment flag: whether its iovecs
 * are user or kernel addresses is whether uio_vmspace is the kernel's,
 * and zfs_uio_init() reads it from there.
 */
#include_next <sys/uio.h>
#include <sys/debug.h>

#define	UIO_DIRECT		0x0001
#define	UIO_UNCACHED		0x0002
#define	UIO_DIO_DENY		0x0004
#define	UIO_DIO_CKSUM_RETRIED	0x0008

/*
 * FreeBSD's segment for a page-in that needs no copy.  NetBSD pages in
 * through its own path and never builds such a uio, so it is a value
 * no uio has.
 */
#define	UIO_NOCOPY		((enum uio_seg)2)

typedef struct iovec	iovec_t;
typedef enum uio_seg	zfs_uio_seg_t;
typedef enum uio_rw	zfs_uio_rw_t;

typedef struct {
	struct vm_page	**pages;
	int		npages;
} zfs_uio_dio_t;

typedef struct zfs_uio {
	struct uio	*uio;		/* a caller's, or NULL for uio_store */
	struct uio	uio_store;
	zfs_uio_seg_t	uio_segflg;
	offset_t	uio_soffset;
	uint16_t	uio_extflg;
	zfs_uio_dio_t	uio_dio;
} zfs_uio_t;

#define	GET_UIO_STRUCT(u)						\
	((u)->uio != NULL ? (u)->uio : &(u)->uio_store)
#define	zfs_uio_segflg(u)	(u)->uio_segflg
#define	zfs_uio_offset(u)	GET_UIO_STRUCT(u)->uio_offset
#define	zfs_uio_resid(u)	GET_UIO_STRUCT(u)->uio_resid
#define	zfs_uio_iov(u)		GET_UIO_STRUCT(u)->uio_iov
#define	zfs_uio_iovcnt(u)	GET_UIO_STRUCT(u)->uio_iovcnt
#define	zfs_uio_iovlen(u, idx)	GET_UIO_STRUCT(u)->uio_iov[(idx)].iov_len
#define	zfs_uio_iovbase(u, idx)	GET_UIO_STRUCT(u)->uio_iov[(idx)].iov_base
#define	zfs_uio_rw(u)		GET_UIO_STRUCT(u)->uio_rw
#define	zfs_uio_soffset(u)	(u)->uio_soffset
#define	zfs_uio_fault_disable(u, set)
#define	zfs_uio_prefaultpages(size, u)	(0)

static __inline void
zfs_uio_setoffset(zfs_uio_t *uio, offset_t off)
{

	zfs_uio_offset(uio) = off;
}

static __inline void
zfs_uio_setsoffset(zfs_uio_t *uio, offset_t off)
{

	ASSERT3U(zfs_uio_offset(uio), ==, off);
	zfs_uio_soffset(uio) = off;
}

static __inline void
zfs_uio_advance(zfs_uio_t *uio, ssize_t size)
{

	zfs_uio_resid(uio) -= size;
	zfs_uio_offset(uio) += size;
}

/*
 * NetBSD's <sys/param.h> includes <sys/uio.h>, so this file stays clear
 * of uvm, which zfs_uio_init() needs to read the segment; it is in
 * spl_uio.c.
 */
extern void zfs_uio_init(zfs_uio_t *, struct uio *);

extern int zfs_uio_fault_move(void *, size_t, zfs_uio_rw_t, zfs_uio_t *);

#endif	/* _SPL_SYS_UIO_H */
