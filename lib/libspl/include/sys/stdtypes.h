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

#ifndef	__SYS_STDTYPES_H
#define	__SYS_STDTYPES_H

typedef enum {
	B_FALSE = 0,
	B_TRUE = 1
} boolean_t;

typedef unsigned char		uchar_t;
typedef unsigned short		ushort_t;
typedef unsigned int		uint_t;
typedef unsigned long		ulong_t;
/*
 * NetBSD declares these in <sys/types.h> for XDR, as exact-width types.
 * They are the same width here, but a second typedef of a different type
 * is still a redefinition, so leave them to the system header.
 */
#ifndef	__NetBSD__
typedef unsigned long long	u_longlong_t;
typedef long long		longlong_t;
#endif

typedef longlong_t		offset_t;
typedef u_longlong_t		u_offset_t;
typedef u_longlong_t		len_t;
typedef longlong_t		diskaddr_t;

typedef ulong_t			pgcnt_t;	/* number of pages */
typedef long			spgcnt_t;	/* signed number of pages */

#ifndef	__NetBSD__
typedef short			pri_t;
#endif
typedef ushort_t		o_mode_t;	/* old file attribute type */

typedef int			major_t;
typedef int			minor_t;

typedef short			index_t;

#endif	/* __SYS_STDTYPES_H */
