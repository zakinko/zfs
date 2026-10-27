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

#ifndef _SPL_SYS_TYPES_H
#define	_SPL_SYS_TYPES_H

/*
 * The illumos names the common code uses for types, over NetBSD's own
 * <sys/types.h>.  NetBSD already has longlong_t, u_longlong_t, pri_t,
 * boolean_t, id_t and caddr_t, with the widths illumos gives them, so
 * those are its own; what is missing is added here.
 */
#include_next <sys/types.h>
#include <sys/null.h>
#include <sys/stdint.h>
#include <sys/stdarg.h>

typedef unsigned int	uint_t;
typedef unsigned char	uchar_t;
typedef unsigned short	ushort_t;
typedef unsigned long	ulong_t;

typedef int64_t		hrtime_t;
typedef int		minor_t;
typedef int		major_t;
typedef off_t		off64_t;
typedef off_t		offset_t;
typedef id_t		taskid_t;
typedef id_t		projid_t;
typedef id_t		poolid_t;
typedef uint_t		zoneid_t;
typedef id_t		ctid_t;
typedef mode_t		o_mode_t;
typedef uint64_t	pgcnt_t;
typedef short		index_t;
typedef int64_t		rlim64_t;
typedef u_longlong_t	u_offset_t;
typedef u_longlong_t	len_t;
typedef longlong_t	diskaddr_t;

/*
 * NetBSD keeps caddr_t out of its kernel on purpose; the common code
 * still uses illumos's, which is a char pointer.
 */
typedef char		*caddr_t;

typedef struct timespec	timestruc_t;
typedef struct timespec	timespec_t;
typedef struct timespec	inode_timespec_t;

/*
 * NetBSD's boolean_t is an int, as illumos's enum is in size; only the
 * illumos names for its values are missing.
 */
#define	B_FALSE	0
#define	B_TRUE	1

#define	MAXNAMELEN	256

#include <sys/debug.h>

#endif	/* _SPL_SYS_TYPES_H */
