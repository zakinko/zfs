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
#include <sys/spa.h>
#include <sys/spa_impl.h>

/* What "zpool history -i" says a command ran in. */
const char *
spa_history_zone(void)
{

	return ("netbsd");
}

void
spa_import_os(spa_t *spa)
{

	(void) spa;
}

void
spa_export_os(spa_t *spa)
{

	(void) spa;
}

void
spa_activate_os(spa_t *spa)
{

	(void) spa;
}

void
spa_deactivate_os(spa_t *spa)
{

	(void) spa;
}
