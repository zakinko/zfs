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

#ifndef _SPL_SYS_VNODE_H
#define	_SPL_SYS_VNODE_H

/*
 * illumos's vnode names over NetBSD's vnodes.  NetBSD's own ZFS made
 * the same choices, and they are taken from it: va_mask lives in the
 * vattr's spare word, and whether a vnode has pages cached is
 * VI_PAGES.
 */
#include <sys/types.h>
#include_next <sys/vnode.h>
#include <sys/namei.h>
#include <sys/fcntl.h>
#include <sys/cred.h>
#include <sys/proc.h>

typedef struct vattr	vattr_t;
typedef enum vtype	vtype_t;
typedef struct flock	flock64_t;

enum symfollow { NO_FOLLOW = NOFOLLOW };

#define	F_FREESP	11

#define	vn_has_cached_data(vp)	(((vp)->v_iflag & VI_PAGES) != 0)
#define	vn_vfswlock(vp)		(0)
#define	vn_vfsunlock(vp)	do { } while (0)
#define	vn_matchops(vp, vops)	((vp)->v_op == (vops))

#define	VN_HOLD(v)		vref(v)
#define	VN_RELE(v)		vrele(v)
#define	VN_URELE(v)		vput(v)
#define	VN_RELE_ASYNC(v, tq)	vrele_async(v)

#define	vnevent_create(vp, ct)			do { } while (0)
#define	vnevent_link(vp, ct)			do { } while (0)
#define	vnevent_remove(vp, dvp, name, ct)	do { } while (0)
#define	vnevent_rmdir(vp, dvp, name, ct)	do { } while (0)
#define	vnevent_rename_src(vp, dvp, name, ct)	do { } while (0)
#define	vnevent_rename_dest(vp, dvp, name, ct)	do { } while (0)
#define	vnevent_rename_dest_dir(vp, ct)		do { } while (0)

#define	va_mask		va_spare
#define	va_nodeid	va_fileid
#define	va_nblocks	va_bytes
#define	va_blksize	va_blocksize

#define	MAXOFFSET_T	__type_max(off_t)
#define	FIGNORECASE	0x00

/* illumos's vattr mask bits. */
#define	AT_TYPE		0x00001
#define	AT_MODE		0x00002
#define	AT_UID		0x00004
#define	AT_GID		0x00008
#define	AT_FSID		0x00010
#define	AT_NODEID	0x00020
#define	AT_NLINK	0x00040
#define	AT_SIZE		0x00080
#define	AT_ATIME	0x00100
#define	AT_MTIME	0x00200
#define	AT_CTIME	0x00400
#define	AT_RDEV		0x00800
#define	AT_BLKSIZE	0x01000
#define	AT_NBLOCKS	0x02000
#define	AT_SEQ		0x08000
#define	AT_XVATTR	0x10000

#define	AT_ALL		(AT_MODE|AT_UID|AT_GID|AT_FSID|AT_NODEID|\
			AT_NLINK|AT_SIZE|AT_ATIME|AT_MTIME|AT_CTIME|\
			AT_RDEV|AT_BLKSIZE|AT_NBLOCKS|AT_SEQ)
#define	AT_STAT		(AT_MODE|AT_UID|AT_GID|AT_FSID|AT_NODEID|AT_NLINK|\
			AT_SIZE|AT_ATIME|AT_MTIME|AT_CTIME|AT_RDEV)
#define	AT_TIMES	(AT_ATIME|AT_MTIME|AT_CTIME)
#define	AT_NOSET	(AT_NLINK|AT_RDEV|AT_FSID|AT_NODEID|\
			AT_BLKSIZE|AT_NBLOCKS|AT_SEQ)

/* Which fields of a NetBSD vattr were given, as illumos's mask says. */
static __inline void
vattr_init_mask(vattr_t *vap)
{

	vap->va_mask = 0;
	if (vap->va_type != VNON)
		vap->va_mask |= AT_TYPE;
	if (vap->va_uid != (uid_t)VNOVAL)
		vap->va_mask |= AT_UID;
	if (vap->va_gid != (gid_t)VNOVAL)
		vap->va_mask |= AT_GID;
	if (vap->va_size != (u_quad_t)VNOVAL)
		vap->va_mask |= AT_SIZE;
	if (vap->va_atime.tv_sec != VNOVAL)
		vap->va_mask |= AT_ATIME;
	if (vap->va_mtime.tv_sec != VNOVAL)
		vap->va_mask |= AT_MTIME;
	if (vap->va_mode != (mode_t)VNOVAL)
		vap->va_mask |= AT_MODE;
	if (vap->va_flags != VNOVAL)
		vap->va_mask |= AT_XVATTR;
}

#define	RLIM64_INFINITY	0

#include <sys/vnode_impl.h>

#include <sys/vfs.h>

#endif	/* _SPL_SYS_VNODE_H */
