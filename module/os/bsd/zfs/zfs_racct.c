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

#include <sys/zfs_context.h>
#include <sys/zfs_racct.h>
#include <sys/lwp.h>

/*
 * Count the I/O against the thread that asked for it, where getrusage(2)
 * finds it, as NetBSD's own file systems do through the buffer cache.
 */
void
zfs_racct_read(spa_t *spa, uint64_t size, uint64_t iops, dmu_flags_t flags)
{

	(void) spa, (void) size, (void) flags;
	curlwp->l_ru.ru_inblock += iops;
}

void
zfs_racct_write(spa_t *spa, uint64_t size, uint64_t iops, dmu_flags_t flags)
{

	(void) spa, (void) size, (void) flags;
	curlwp->l_ru.ru_oublock += iops;
}
