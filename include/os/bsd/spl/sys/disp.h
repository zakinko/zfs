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

#ifndef _SPL_SYS_DISP_H
#define	_SPL_SYS_DISP_H

#include <sys/sched.h>

/*
 * illumos's kpreempt(KPREEMPT_SYNC) gives up the CPU to whatever else
 * is runnable, which is NetBSD's preempt().
 */
#define	KPREEMPT_SYNC		(-1)
#define	kpreempt(x)		preempt()

#endif	/* _SPL_SYS_DISP_H */
