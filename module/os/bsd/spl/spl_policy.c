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
 * illumos's privilege checks, through kauth(9).
 *
 * Administering pools and datasets, and injecting faults, are what
 * Linux asks CAP_SYS_ADMIN for and FreeBSD a privilege of their own;
 * NetBSD has no finer action for them than the superuser's, which is
 * also what its own ZFS asked.  A check on a vnode goes to
 * kauth_authorize_vnode() with the file system's own answer as "no",
 * so that the secmodel can grant it, as NetBSD's file systems do.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/kauth.h>
#include <sys/vnode.h>
#include <sys/policy.h>
#include <sys/zfs_znode.h>

int
secpolicy_zfs(cred_t *cr)
{

	return (kauth_authorize_generic(cr, KAUTH_GENERIC_ISSUSER, NULL));
}

int
secpolicy_sys_config(cred_t *cr, int checkonly)
{

	(void) checkonly;
	return (kauth_authorize_generic(cr, KAUTH_GENERIC_ISSUSER, NULL));
}

int
secpolicy_zinject(cred_t *cr)
{

	return (kauth_authorize_generic(cr, KAUTH_GENERIC_ISSUSER, NULL));
}

/* Whether a write may leave the set-id bits on the file it writes. */
int
secpolicy_vnode_setid_retain(struct znode *zp, cred_t *cr,
    boolean_t issuidroot)
{

	(void) issuidroot;
	return (kauth_authorize_vnode(cr, KAUTH_VNODE_RETAIN_SUID, ZTOV(zp),
	    NULL, EPERM));
}
