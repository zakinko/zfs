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

#include <sys/types.h>
#include <sys/disk.h>
#include <sys/dkio.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/vdev_impl.h>

#include <libzutil.h>

#include "zutil_import.h"

/*
 * The control device is the one node under /dev this must not touch: it
 * is how the pool is reached, not something a pool can live on.
 */
static boolean_t
excluded_dev(const char *name)
{
	return (strcmp(name, _PATH_DEV "zfs") == 0);
}

/*
 * Update a leaf vdev's persistent device strings.
 *
 * Both strings are Linux's udev names for a disk.  Nothing here writes
 * them and nothing here could resolve them again, so strip them rather
 * than carry a name into the label that no one can follow.
 */
void
update_vdev_config_dev_strs(nvlist_t *nv)
{
	(void) nvlist_remove_all(nv, ZPOOL_CONFIG_DEVID);
	(void) nvlist_remove_all(nv, ZPOOL_CONFIG_PHYS_PATH);
}

/*
 * Common predicate for zpool_dev_probe_ok() and its fd variant: only a
 * disk device (character or block), or a regular file large enough to
 * hold a label, may be probed.  Anything else is refused.
 */
static boolean_t
dev_stat_probe_ok(const struct stat64 *statbuf)
{
	if (S_ISREG(statbuf->st_mode))
		return (statbuf->st_size >= SPA_MINDEVSIZE);

	return (S_ISCHR(statbuf->st_mode) || S_ISBLK(statbuf->st_mode));
}

/*
 * Determine if a path may be safely opened to probe for a vdev label.
 * Only regular files large enough to hold a label and disk devices
 * (character or block) are acceptable.  Anything else is refused,
 * opening other nodes can have side effects.  stat64() never blocks,
 * even on a FIFO.
 */
boolean_t
zpool_dev_probe_ok(const char *path)
{
	struct stat64 statbuf;

	if (excluded_dev(path))
		return (B_FALSE);

	if (stat64(path, &statbuf) != 0)
		return (B_FALSE);

	return (dev_stat_probe_ok(&statbuf));
}

/*
 * As zpool_dev_probe_ok(), but re-check the type of an object already
 * opened.  A path naming a symlink may have been repointed at a different
 * node between the stat64() above and the open(), so only trust a
 * descriptor which is still a disk device or a large enough regular file.
 */
boolean_t
zpool_dev_probe_ok_fd(int fd)
{
	struct stat64 statbuf;

	if (fstat64(fd, &statbuf) != 0)
		return (B_FALSE);

	return (dev_stat_probe_ok(&statbuf));
}

/*
 * Enumerate the disks and wedges to search for pools.  hw.disknames
 * names them all, whole disks and dk(4) wedges alike, and is the only
 * list that covers both; reading _PATH_DEV would miss a wedge whose
 * node has not been created yet and would offer a great many nodes
 * that are not disks at all.
 */
int
zpool_find_import_blkid(libpc_handle_t *hdl, pthread_mutex_t *lock,
    avl_tree_t **slice_cache)
{
	static const char mib[] = "hw.disknames";
	char *names, *name, *last, path[MAXPATHLEN];
	rdsk_node_t *slice;
	avl_index_t where;
	size_t len;

	if (sysctlbyname(mib, NULL, &len, NULL, 0) == -1)
		return (errno);
	names = zutil_alloc(hdl, len + 1);
	if (sysctlbyname(mib, names, &len, NULL, 0) == -1) {
		int error = errno;

		free(names);
		return (error);
	}
	names[len] = '\0';

	*slice_cache = zutil_alloc(hdl, sizeof (avl_tree_t));
	avl_create(*slice_cache, slice_cache_compare, sizeof (rdsk_node_t),
	    offsetof(rdsk_node_t, rn_node));

	for (name = strtok_r(names, " ", &last); name != NULL;
	    name = strtok_r(NULL, " ", &last)) {
		(void) snprintf(path, sizeof (path), "%s%s", _PATH_DEV, name);

		slice = zutil_alloc(hdl, sizeof (rdsk_node_t));
		slice->rn_name = zutil_strdup(hdl, path);
		slice->rn_vdev_guid = 0;
		slice->rn_lock = lock;
		slice->rn_avl = *slice_cache;
		slice->rn_hdl = hdl;
		slice->rn_labelpaths = B_FALSE;
		slice->rn_order = IMPORT_ORDER_DEFAULT;

		pthread_mutex_lock(lock);
		if (avl_find(*slice_cache, slice, &where)) {
			free(slice->rn_name);
			free(slice);
		} else {
			avl_insert(*slice_cache, slice, where);
		}
		pthread_mutex_unlock(lock);
	}

	free(names);
	return (0);
}

/*
 * Open the character device of the same name as a block device, that is
 * /dev/rld0 beside /dev/ld0.  Reading a label through the raw device
 * keeps it out of the buffer cache, which is what NetBSD's own zpool
 * does.  Returns -1 when there is no such node, and the caller then
 * stays with the block device it has.
 */
static int
open_raw_twin(const char *path)
{
	char raw[MAXPATHLEN];
	struct stat64 statbuf;
	const char *name;
	int fd;

	if ((name = strrchr(path, '/')) == NULL)
		return (-1);
	name++;

	if ((size_t)snprintf(raw, sizeof (raw), "%.*sr%s",
	    (int)(name - path), path, name) >= sizeof (raw))
		return (-1);

	if ((fd = open(raw, O_RDONLY|O_NONBLOCK|O_CLOEXEC)) < 0)
		return (-1);

	if (fstat64(fd, &statbuf) != 0 || !S_ISCHR(statbuf.st_mode)) {
		(void) close(fd);
		return (-1);
	}

	return (fd);
}

void
zpool_open_func(void *arg)
{
	rdsk_node_t *rn = arg;
	struct dkwedge_list dkwl;
	struct stat64 statbuf;
	nvlist_t *config;
	off_t mediasize;
	int num_labels;
	int fd, rfd;

	if (excluded_dev(rn->rn_name))
		return;

	/*
	 * O_NONBLOCK so we don't hang trying to open things like serial ports.
	 */
	if ((fd = open(rn->rn_name, O_RDONLY|O_NONBLOCK|O_CLOEXEC)) < 0)
		return;

	if (fstat64(fd, &statbuf) != 0)
		goto out;

	if (S_ISREG(statbuf.st_mode)) {
		/* Too small to hold a label. */
		if (statbuf.st_size < SPA_MINDEVSIZE)
			goto out;
	} else if (S_ISCHR(statbuf.st_mode) || S_ISBLK(statbuf.st_mode)) {
		if (S_ISBLK(statbuf.st_mode) &&
		    (rfd = open_raw_twin(rn->rn_name)) >= 0) {
			(void) close(fd);
			fd = rfd;
		}

		/*
		 * A disk carved into wedges is not itself a vdev: the label
		 * belongs to the wedge, and dk(4) hands the wedge out
		 * separately, so probing the disk as well would find the
		 * same label under a name the pool must not be opened by.
		 */
		memset(&dkwl, 0, sizeof (dkwl));
		if (ioctl(fd, DIOCLWEDGES, &dkwl) == 0 && dkwl.dkwl_nwedges > 0)
			goto out;

		if (ioctl(fd, DIOCGMEDIASIZE, &mediasize) != 0 ||
		    mediasize < SPA_MINDEVSIZE)
			goto out;
	} else {
		goto out;
	}

	if (zpool_read_label(fd, &config, &num_labels) != 0)
		goto out;
	if (num_labels == 0) {
		nvlist_free(config);
		goto out;
	}

	rn->rn_config = config;
	rn->rn_num_labels = num_labels;
out:
	(void) close(fd);
}

/*
 * Every disk node lives directly under /dev; there is no by-id or by-path
 * directory to look in as well.
 */
static const char * const
zpool_default_import_path[] = {
	_PATH_DEV
};

const char * const *
zpool_default_search_paths(size_t *count)
{
	*count = ARRAY_SIZE(zpool_default_import_path);
	return (zpool_default_import_path);
}

int
zfs_dev_flush(int fd)
{
	(void) fd;
	return (0);
}

void
update_vdev_config_dev_sysfs_path(nvlist_t *nv, const char *path,
    const char *key)
{
	(void) nv;
	(void) path;
	(void) key;
}

void
update_vdevs_config_dev_sysfs_path(nvlist_t *config)
{
	(void) config;
}

int
zpool_disk_wait(const char *path)
{
	(void) path;
	return (ENOTSUP);
}
