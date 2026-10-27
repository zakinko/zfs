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

#ifndef _ABD_OS_H
#define	_ABD_OS_H

/*
 * A scattered ABD is an array of page sized chunks taken from the
 * kernel's own memory, as on FreeBSD; see abd_os.c.
 */
#include <sys/types.h>

struct abd;
struct vm_page;

struct abd_scatter {
	uint_t		abd_offset;
	void		*abd_chunks[1];	/* actually variable-length */
};

struct abd_linear {
	void		*abd_buf;
};

#ifdef _KERNEL
__attribute__((malloc))
struct abd *abd_alloc_from_pages(struct vm_page **, unsigned long, uint64_t);
#endif

#endif	/* _ABD_OS_H */
