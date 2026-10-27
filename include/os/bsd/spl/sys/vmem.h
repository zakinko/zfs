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

#ifndef _SPL_SYS_VMEM_H
#define	_SPL_SYS_VMEM_H

/*
 * The common code includes <sys/vmem.h> for illumos's vmem_alloc(size,
 * flags), which <sys/kmem.h> here defines over kmem(9).  NetBSD's own
 * vmem(9) is an arena allocator with the same names and other
 * arguments, and uvm needs its types, so it is included first: once its
 * declarations are in, the macros can no longer collide with them.
 */
#include_next <sys/vmem.h>
#include <sys/kmem.h>

#endif	/* _SPL_SYS_VMEM_H */
