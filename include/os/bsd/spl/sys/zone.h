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

#ifndef _SPL_SYS_ZONE_H
#define	_SPL_SYS_ZONE_H

/*
 * NetBSD has no zones and no jails: every process is in the global
 * zone, and no dataset can be delegated to another.
 */
#include <sys/types.h>
#include <sys/errno.h>

#define	GLOBAL_ZONEID		0
#define	INGLOBALZONE(proc)	(1)

typedef enum zone_uid_op {
	ZONE_OP_CREATE,
	ZONE_OP_SNAPSHOT,
	ZONE_OP_CLONE,
	ZONE_OP_DESTROY,
	ZONE_OP_RENAME,
	ZONE_OP_SETPROP
} zone_uid_op_t;

typedef enum zone_admin_result {
	ZONE_ADMIN_NOT_APPLICABLE,
	ZONE_ADMIN_ALLOWED,
	ZONE_ADMIN_DENIED
} zone_admin_result_t;

typedef uid_t (*zone_get_zoned_uid_fn_t)(const char *, char *, size_t);

static __inline int
zone_dataset_visible(const char *dataset, int *write)
{

	(void) dataset;
	if (write != NULL)
		*write = 1;
	return (1);
}

static __inline zone_admin_result_t
zone_dataset_admin_check(const char *dataset, zone_uid_op_t op,
    const char *aux_dataset)
{

	(void) dataset, (void) op, (void) aux_dataset;
	return (ZONE_ADMIN_NOT_APPLICABLE);
}

static __inline void
zone_register_zoned_uid_callback(zone_get_zoned_uid_fn_t fn)
{

	(void) fn;
}

static __inline void
zone_unregister_zoned_uid_callback(void)
{
}

static __inline int
zone_dataset_attach_uid(kauth_cred_t cred, const char *dataset, uid_t uid)
{

	(void) cred, (void) dataset, (void) uid;
	return (ENXIO);
}

static __inline int
zone_dataset_detach_uid(kauth_cred_t cred, const char *dataset, uid_t uid)
{

	(void) cred, (void) dataset, (void) uid;
	return (ENXIO);
}

extern uint32_t zone_get_hostid(void *);

#endif	/* _SPL_SYS_ZONE_H */
