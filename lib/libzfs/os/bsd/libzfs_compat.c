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

#include <sys/module.h>
#include <sys/param.h>
#include <sys/stat.h>

#include <errno.h>
#include <libintl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libzfs.h>
#include <libzutil.h>

#include "../../libzfs_impl.h"

#define	ZFS_KMOD	"zfs"

static char errbuf[ERRBUFLEN];

const char *
libzfs_error_init(int error)
{
	(void) snprintf(errbuf, sizeof (errbuf), "%s", zfs_strerror(error));

	return (errbuf);
}

int
libzfs_load_module(void)
{
	modctl_load_t cmd;

	cmd.ml_filename = ZFS_KMOD;
	cmd.ml_flags = MODCTL_NO_PROP;
	cmd.ml_props = NULL;
	cmd.ml_propslen = 0;

	if (modctl(MODCTL_LOAD, &cmd) == -1 && errno != EEXIST)
		return (errno);
	return (0);
}

int
zpool_relabel_disk(libzfs_handle_t *hdl, const char *path, const char *msg)
{
	(void) hdl, (void) path, (void) msg;
	return (0);
}

int
zpool_label_disk(libzfs_handle_t *hdl, zpool_handle_t *zhp, const char *name)
{
	(void) hdl, (void) zhp, (void) name;
	return (0);
}

int
find_shares_object(differ_info_t *di)
{
	(void) di;
	return (0);
}

int
zfs_destroy_snaps_nvl_os(libzfs_handle_t *hdl, nvlist_t *snaps)
{
	(void) hdl, (void) snaps;
	return (0);
}

/*
 * Jails are FreeBSD's; there is nothing here to hand a dataset to.
 */
int
zfs_jail(zfs_handle_t *zhp, int jailid, int attach)
{
	(void) jailid, (void) attach;

	zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
	    "jails are not supported on this platform"));
	return (zfs_error(zhp->zfs_hdl, EZFS_NOTSUP,
	    dgettext(TEXT_DOMAIN, "cannot jail")));
}

/*
 * nextboot(8) is FreeBSD's; there is no one-shot boot pool to set.
 */
int
zpool_nextboot(libzfs_handle_t *hdl, uint64_t pool_guid, uint64_t dev_guid,
    const char *command)
{
	(void) pool_guid, (void) dev_guid, (void) command;

	zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
	    "nextboot is not supported on this platform"));
	return (zfs_error(hdl, EZFS_POOL_NOTSUP,
	    dgettext(TEXT_DOMAIN, "cannot set nextboot")));
}

/*
 * The version the kernel is running, which it does not report yet.
 * Callers take NULL for "not known" and say so.
 */
char *
zfs_version_kernel(void)
{
	errno = ENOTSUP;
	return (NULL);
}
