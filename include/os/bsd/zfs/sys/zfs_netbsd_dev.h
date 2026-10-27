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

#ifndef _SYS_ZFS_NETBSD_DEV_H
#define	_SYS_ZFS_NETBSD_DEV_H

/*
 * The zfs major, character and block, and the volumes on its minors
 * other than 0, which is /dev/zfs.
 */

struct buf;
struct lwp;
struct uio;

extern devmajor_t zfsdev_bmajor, zfsdev_cmajor;

int zvol_netbsd_open(dev_t, int, int, struct lwp *);
int zvol_netbsd_close(dev_t, int, int, struct lwp *);
int zvol_netbsd_read(dev_t, struct uio *, int);
int zvol_netbsd_write(dev_t, struct uio *, int);
int zvol_netbsd_ioctl(dev_t, u_long, void *, int, struct lwp *);
void zvol_netbsd_strategy(struct buf *);
int zvol_netbsd_discard(dev_t, off_t, off_t);
int zvol_netbsd_psize(dev_t);

#endif /* _SYS_ZFS_NETBSD_DEV_H */
