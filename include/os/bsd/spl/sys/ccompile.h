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

#ifndef	_SYS_CCOMPILE_H
#define	_SYS_CCOMPILE_H

/*
 * Names the common code takes from illumos and Linux.  Force included
 * from config/Rules.am.  The guards matter: the three systems sharing
 * this layer have different subsets of these already, and the ones
 * they have are the ones to use.
 */

#include <sys/types.h>
#include <errno.h>

#ifdef	__cplusplus
extern "C" {
#endif

/* Substitutes follow the ones FreeBSD chose, so the two agree. */
#ifndef	ECHRNG
#define	ECHRNG		ENXIO
#endif
#ifndef	ETIME
#define	ETIME		ETIMEDOUT
#endif
#ifndef	ENOSTR
#define	ENOSTR		ENOTCONN
#endif
#ifndef	ENODATA
#define	ENODATA		EINVAL
#endif
#ifndef	EREMOTEIO
#define	EREMOTEIO	EREMOTE
#endif
/*
 * A checksum failure carries an errno of its own, which callers tell
 * apart from EIO, so it cannot borrow that.  FreeBSD spends
 * EINTEGRITY on it and Linux spends EBADE; where there is neither,
 * EBADMSG says the same thing.
 */
#ifndef	ECKSUM
#ifdef	EINTEGRITY
#define	ECKSUM		EINTEGRITY
#else
#define	ECKSUM		EBADMSG
#endif
#endif
#ifndef	EFRAGS
#define	EFRAGS		ENOSPC
#endif
#ifndef	ENOTACTIVE
#define	ENOTACTIVE	ECANCELED
#endif

/* ERESTART is kernel only here, and never reaches userland. */
#ifndef	ERESTART
#define	ERESTART	EAGAIN
#endif

typedef off_t loff_t;

/* The BSDs have had 64 bit file offsets all along. */
#define	open64		open
#define	pread64		pread
#define	pwrite64	pwrite
#define	lseek64		lseek
#define	ftruncate64	ftruncate
#define	stat64		stat
#define	lstat64		lstat
#define	statvfs64	statvfs
#define	readdir64	readdir
#define	dirent64	dirent

#ifdef	__cplusplus
}
#endif

/*
 * The common code reaches for these without including a header for them.
 * FreeBSD answers the same way, from its own ccompile.h.
 */
#define	ARRAY_SIZE(a)		(sizeof (a) / sizeof (a[0]))
#define	ABS(a)			((a) < 0 ? -(a) : (a))
#define	DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))
#define	ISP2(x)			(((x) & ((x) - 1)) == 0)
#define	IS_P2ALIGNED(v, a)	((((uintptr_t)(v)) & ((uintptr_t)(a) - 1)) == 0)
#define	P2CROSS(x, y, align)	(((x) ^ (y)) > (align) - 1)
#define	P2ROUNDUP(x, align)	((((x) - 1) | ((align) - 1)) + 1)
#define	P2PHASE(x, align)	((x) & ((align) - 1))
#define	P2NPHASE(x, align)	(-(x) & ((align) - 1))
#define	P2BOUNDARY(off, len, align) \
	(((off) ^ ((off) + (len) - 1)) > (align) - 1)

#endif	/* _SYS_CCOMPILE_H */
