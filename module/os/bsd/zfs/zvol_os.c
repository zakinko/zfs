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

/*
 * Volumes.
 *
 * Not yet: a volume is to be a NetBSD disk, a disk(9) driver with a
 * minor per volume, and that is not written.  Until it is, a volume
 * can be created, written by zfs receive and sent, but it has no
 * device node: making one fails, which the common code reports and
 * goes on from, and so nothing is ever open on one.
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/dmu.h>
#include <sys/dmu_tx.h>
#include <sys/dnode.h>
#include <sys/zil.h>
#include <sys/zfs_rlock.h>
#include <sys/dataset_kstats.h>
#include <sys/zvol.h>
#include <sys/zvol_impl.h>

int
zvol_init(void)
{

	return (zvol_init_impl());
}

void
zvol_fini(void)
{

	zvol_fini_impl();
}

int
zvol_busy(void)
{

	return (0);
}

int
zvol_os_create_minor(const char *name)
{

	(void) name;
	return (SET_ERROR(ENOTSUP));
}

void
zvol_os_remove_minor(zvol_state_t *zv)
{

	(void) zv;
}

void
zvol_os_free(zvol_state_t *zv)
{

	(void) zv;
}

int
zvol_os_rename_minor(zvol_state_t *zv, const char *newname)
{

	(void) zv, (void) newname;
	return (SET_ERROR(ENOTSUP));
}

int
zvol_os_update_volsize(zvol_state_t *zv, uint64_t volsize)
{

	zv->zv_volsize = volsize;
	return (0);
}

boolean_t
zvol_os_is_zvol(const char *path)
{

	(void) path;
	return (B_FALSE);
}

void
zvol_os_set_disk_ro(zvol_state_t *zv, int flags)
{

	(void) zv, (void) flags;
}

void
zvol_os_set_capacity(zvol_state_t *zv, uint64_t capacity)
{

	(void) zv, (void) capacity;
}

void
zvol_wait_close(zvol_state_t *zv)
{

	(void) zv;
}
