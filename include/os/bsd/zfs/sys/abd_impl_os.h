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

#ifndef _ABD_IMPL_OS_H
#define	_ABD_IMPL_OS_H

#include <sys/systm.h>

/* FreeBSD's critical_enter(): the thread stays on this CPU. */
#define	abd_enter_critical(flags)	kpreempt_disable()
#define	abd_exit_critical(flags)	kpreempt_enable()

#endif	/* _ABD_IMPL_OS_H */
