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

#ifndef	_LIBSPL_SYS_FCNTL_H
#define	_LIBSPL_SYS_FCNTL_H

#include_next <sys/fcntl.h>

#ifndef	O_LARGEFILE
#define	O_LARGEFILE	0
#endif
#ifndef	O_RSYNC
#define	O_RSYNC		0
#endif
#ifndef	O_DSYNC
#define	O_DSYNC		0
#endif

#endif	/* _LIBSPL_SYS_FCNTL_H */
