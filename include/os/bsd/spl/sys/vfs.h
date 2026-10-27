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

#ifndef _SPL_SYS_VFS_H
#define	_SPL_SYS_VFS_H

/* illumos's vfs_t is NetBSD's struct mount. */
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/proc.h>

#define	rootdir		rootvnode

typedef struct mount	vfs_t;
typedef int		umode_t;

#define	vfs_flag	mnt_flag
#define	vfs_data	mnt_data
#define	vfs_count	mnt_refcnt
#define	vfs_fsid	mnt_stat.f_fsidx
#define	vfs_bsize	mnt_stat.f_bsize
#define	vfs_resource	mnt_stat.f_mntfromname

#define	v_flag		v_vflag
#define	v_vfsp		v_mount

#define	VFS_RDONLY	MNT_RDONLY
#define	VFS_NOSETUID	MNT_NOSUID
#define	VFS_NOEXEC	MNT_NOEXEC
#define	VROOT		VV_ROOT

typedef struct mntopt {
	char	*mo_name;
	char	**mo_cancel;
	char	*mo_arg;
	int	mo_flags;
	void	*mo_data;
} mntopt_t;

#define	MO_SET		0x01
#define	MO_NODISPLAY	0x02
#define	MO_HASVALUE	0x04
#define	MO_IGNORE	0x08
#define	MO_DEFAULT	MO_SET
#define	MO_TAG		0x10
#define	MO_EMPTY	0x20

#define	VFS_NOFORCEOPT	0x01
#define	VFS_DISPLAY	0x02
#define	VFS_NODISPLAY	0x04
#define	VFS_CREATEOPT	0x08

typedef struct mntopts {
	uint_t		mo_count;
	mntopt_t	*mo_list;
} mntopts_t;

extern void vfs_setmntopt(vfs_t *, const char *, const char *, int);
extern void vfs_clearmntopt(vfs_t *, const char *);
extern int vfs_optionisset(const vfs_t *, const char *, char **);

typedef uint64_t	vfs_feature_t;

#define	VFSFT_XVATTR		0x100000001
#define	VFSFT_CASEINSENSITIVE	0x100000002
#define	VFSFT_NOCASESENSITIVE	0x100000004
#define	VFSFT_DIRENTFLAGS	0x100000008
#define	VFSFT_ACLONCREATE	0x100000010
#define	VFSFT_ACEMASKONACCESS	0x100000020
#define	VFSFT_SYSATTR_VIEWS	0x100000040
#define	VFSFT_ACCESS_FILTER	0x100000080
#define	VFSFT_REPARSE		0x100000100
#define	VFSFT_ZEROCOPY_SUPPORTED	0x100000200

#endif	/* _SPL_SYS_VFS_H */
