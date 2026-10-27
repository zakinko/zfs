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

#ifndef _SPL_SYS_CRED_H
#define	_SPL_SYS_CRED_H

/*
 * illumos's credentials are NetBSD's kauth_cred_t, which is opaque to
 * a module: everything goes through kauth(9).  illumos's crgetuid() and
 * crgetgid() are the effective ids.
 */
#include <sys/types.h>
#include <sys/kauth.h>

typedef struct kauth_cred cred_t;

/* proc0's credentials, which kern_proc.c defines and no header names. */
extern kauth_cred_t cred0;

#define	CRED()			(kauth_cred_get())
#define	kcred			(cred0)

#define	KUID_TO_SUID(x)		(x)
#define	KGID_TO_SGID(x)		(x)

#define	crgetuid(cr)		kauth_cred_geteuid(cr)
#define	crgetruid(cr)		kauth_cred_getuid(cr)
#define	crgetgid(cr)		kauth_cred_getegid(cr)
#define	crgetrgid(cr)		kauth_cred_getgid(cr)
#define	crgetngroups(cr)	((int)kauth_cred_ngroups(cr))
#define	crgetgroup(cr, i)	kauth_cred_group((cr), (u_int)(i))
#define	crgetzoneid(cr)		GLOBAL_ZONEID
#define	crhold(cr)		kauth_cred_hold(cr)
#define	crfree(cr)		kauth_cred_free(cr)

/* kauth answers 0 for a member, where illumos answers true. */
#define	groupmember(gid, cr)	(kauth_cred_groupmember((cr), (gid)) == 0)

/*
 * No crgetgroups(): it hands back the cred's array of groups, which a
 * NetBSD cred does not show.  Callers take them one at a time with
 * crgetgroup().
 */

#endif	/* _SPL_SYS_CRED_H */
