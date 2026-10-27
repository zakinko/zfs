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

#ifndef _SPL_SYS_PARAM_H
#define	_SPL_SYS_PARAM_H

#include <sys/types.h>
#include_next <sys/param.h>

#define	PAGESIZE	PAGE_SIZE
#define	PAGESHIFT	PAGE_SHIFT
/* NetBSD's ptob() is the same shift, where the port defines it. */
#ifndef ptob
#define	ptob(x)		((uint64_t)(x) << PAGE_SHIFT)
#endif

#endif	/* _SPL_SYS_PARAM_H */
