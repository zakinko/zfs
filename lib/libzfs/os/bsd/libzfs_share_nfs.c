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

#include <stdio.h>

#include <libzfs.h>

#include "../../libzfs_share.h"

/*
 * Sharing waits on the kernel side of this platform layer; until a
 * data set can be mounted there is nothing to hand to NFS.
 */

static int
nfs_enable_share(sa_share_impl_t impl_share)
{
	(void) impl_share;
	fputs("No NFS sharing here yet.\n", stderr);
	return (SA_NOT_SUPPORTED);
}

static int
nfs_disable_share(sa_share_impl_t impl_share)
{
	(void) impl_share;
	fputs("No NFS sharing here yet.\n", stderr);
	return (SA_NOT_SUPPORTED);
}

static int
nfs_validate_shareopts(const char *shareopts)
{
	(void) shareopts;
	fputs("No NFS sharing here yet.\n", stderr);
	return (SA_NOT_SUPPORTED);
}

static boolean_t
nfs_is_share_active(sa_share_impl_t impl_share)
{
	(void) impl_share;
	return (B_FALSE);
}

static int
nfs_commit_shares(void)
{
	return (0);
}

const sa_fstype_t libshare_nfs_type = {
	.enable_share = nfs_enable_share,
	.disable_share = nfs_disable_share,
	.is_shared = nfs_is_share_active,

	.validate_shareopts = nfs_validate_shareopts,
	.commit_shares = nfs_commit_shares,
};
