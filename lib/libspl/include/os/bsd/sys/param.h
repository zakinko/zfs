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

#ifndef	_LIBSPL_SYS_PARAM_H
#define	_LIBSPL_SYS_PARAM_H

#include_next <sys/param.h>
#include <unistd.h>

/*
 * PAGESIZE is a compile time constant only where the page size is, so
 * ask the system; spl_pagesize() caches the answer.
 */
#ifdef	PAGESIZE
#undef	PAGESIZE
#endif
extern size_t spl_pagesize(void);
#define	PAGESIZE	(spl_pagesize())

#ifndef	ptob
#define	ptob(x)		((x) * PAGESIZE)
#endif

#define	UID_NOBODY	60001
#define	GID_NOBODY	UID_NOBODY
#define	UID_NOACCESS	60002

#define	MAXUID		UINT32_MAX
#define	MAXPROJID	MAXUID
#define	MAXNAMELEN	256
#define	MAXOFFSET_T	OFF_MAX

#endif	/* _LIBSPL_SYS_PARAM_H */
