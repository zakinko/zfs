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

#ifndef _SPL_SYS_STRING_H
#define	_SPL_SYS_STRING_H

#include <lib/libkern/libkern.h>
#include <sys/kmem.h>

/*
 * NetBSD's kmem_strdup() takes the allocation flags as illumos's does
 * not; the copy is made sleeping, as illumos's is.  kmem_strfree() is
 * NetBSD's own, and frees strlen() + 1 bytes, as illumos's does.
 */
#undef	kmem_strdup
#define	kmem_strdup(s)	kmem_strdupsize((s), NULL, KM_SLEEP)

void	strident_canon(char *, size_t);

#endif	/* _SPL_SYS_STRING_H */
