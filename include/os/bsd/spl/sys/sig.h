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

#ifndef _SPL_SYS_SIG_H
#define	_SPL_SYS_SIG_H

#include <sys/signalvar.h>
#include <sys/lwp.h>

/* Whether a signal is pending for this thread. */
static __inline int
issig(void)
{

	return (sigispending(curlwp, 0) != 0);
}

#endif	/* _SPL_SYS_SIG_H */
