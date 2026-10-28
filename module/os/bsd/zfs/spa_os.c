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
#include <sys/vdev_impl.h>
#include <sys/fs/zfs.h>
#include <sys/zfs_netbsd_dev.h>

/*
 * Point each leaf of a vdev tree at the device its guid was found on
 * now; the label holds the path the device had when it was written.
 */
static void
spa_rootconf_paths(nvlist_t *nv, nvlist_t *paths)
{
	nvlist_t **child;
	uint_t c, children;
	uint64_t guid;
	const char *path;
	char key[24];

	if (nvlist_lookup_nvlist_array(nv, ZPOOL_CONFIG_CHILDREN, &child,
	    &children) == 0) {
		for (c = 0; c < children; c++)
			spa_rootconf_paths(child[c], paths);
		return;
	}
	if (nvlist_lookup_uint64(nv, ZPOOL_CONFIG_GUID, &guid) != 0)
		return;
	(void) snprintf(key, sizeof (key), "%llu", (u_longlong_t)guid);
	if (nvlist_lookup_string(paths, key, &path) == 0)
		fnvlist_add_string(nv, ZPOOL_CONFIG_PATH, path);
}

/*
 * The root pool's configuration, put together from the labels on the
 * disks, as FreeBSD's spa_generate_rootconf() does from GEOM: the
 * newest label of each top-level vdev, holes and missing ones filled
 * in, under a root vdev.
 */
static nvlist_t *
spa_generate_rootconf(const char *name)
{
	nvlist_t **configs, **tops;
	nvlist_t *config, *best_cfg = NULL, *nvtop, *nvroot, *paths;
	uint64_t *holes;
	uint64_t best_txg = 0, nchildren = 1, pgid, count, i;
	uint_t nholes;

	if (vdev_disk_read_pool_label(name, &configs, &count, &paths) != 0) {
		fnvlist_free(paths);
		return (NULL);
	}
	for (i = 0; i < count; i++) {
		uint64_t txg;

		if (configs[i] == NULL)
			continue;
		txg = fnvlist_lookup_uint64(configs[i], ZPOOL_CONFIG_POOL_TXG);
		if (txg > best_txg) {
			best_txg = txg;
			best_cfg = configs[i];
		}
	}
	ASSERT3P(best_cfg, !=, NULL);

	(void) nvlist_lookup_uint64(best_cfg, ZPOOL_CONFIG_VDEV_CHILDREN,
	    &nchildren);
	holes = NULL;
	(void) nvlist_lookup_uint64_array(best_cfg, ZPOOL_CONFIG_HOLE_ARRAY,
	    &holes, &nholes);

	tops = kmem_zalloc(nchildren * sizeof (void *), KM_SLEEP);
	for (i = 0; i < nchildren && i < count; i++) {
		if (configs[i] == NULL)
			continue;
		nvtop = fnvlist_lookup_nvlist(configs[i],
		    ZPOOL_CONFIG_VDEV_TREE);
		tops[i] = fnvlist_dup(nvtop);
		spa_rootconf_paths(tops[i], paths);
	}
	for (i = 0; holes != NULL && i < nholes; i++) {
		if (holes[i] >= nchildren || tops[holes[i]] != NULL)
			continue;
		tops[holes[i]] = fnvlist_alloc();
		fnvlist_add_string(tops[holes[i]], ZPOOL_CONFIG_TYPE,
		    VDEV_TYPE_HOLE);
		fnvlist_add_uint64(tops[holes[i]], ZPOOL_CONFIG_ID, holes[i]);
		fnvlist_add_uint64(tops[holes[i]], ZPOOL_CONFIG_GUID, 0);
	}
	for (i = 0; i < nchildren; i++) {
		if (tops[i] != NULL)
			continue;
		tops[i] = fnvlist_alloc();
		fnvlist_add_string(tops[i], ZPOOL_CONFIG_TYPE,
		    VDEV_TYPE_MISSING);
		fnvlist_add_uint64(tops[i], ZPOOL_CONFIG_ID, i);
		fnvlist_add_uint64(tops[i], ZPOOL_CONFIG_GUID, 0);
	}

	config = fnvlist_dup(best_cfg);
	pgid = fnvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_GUID);
	nvroot = fnvlist_alloc();
	fnvlist_add_string(nvroot, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_uint64(nvroot, ZPOOL_CONFIG_ID, 0ULL);
	fnvlist_add_uint64(nvroot, ZPOOL_CONFIG_GUID, pgid);
	fnvlist_add_nvlist_array(nvroot, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t * const *)tops, nchildren);
	fnvlist_add_nvlist(config, ZPOOL_CONFIG_VDEV_TREE, nvroot);
	/* What only a leaf's label has. */
	fnvlist_remove(config, ZPOOL_CONFIG_GUID);
	fnvlist_remove(config, ZPOOL_CONFIG_TOP_GUID);

	for (i = 0; i < count; i++)
		if (configs[i] != NULL)
			fnvlist_free(configs[i]);
	kmem_free(configs, count * sizeof (void *));
	for (i = 0; i < nchildren; i++)
		fnvlist_free(tops[i]);
	kmem_free(tops, nchildren * sizeof (void *));
	fnvlist_free(nvroot);
	fnvlist_free(paths);
	return (config);
}

/*
 * Put the root pool in the namespace, from its labels, for
 * zfs_mountroot(); the open that follows loads it.  As FreeBSD's.
 */
int
spa_import_rootpool(const char *name, bool checkpointrewind)
{
	spa_t *spa;
	vdev_t *rvd;
	nvlist_t *config, *nvtop;
	const char *pname;
	int error;

	config = spa_generate_rootconf(name);

	spa_namespace_enter(FTAG);
	if (config != NULL) {
		pname = fnvlist_lookup_string(config, ZPOOL_CONFIG_POOL_NAME);
		VERIFY0(strcmp(name, pname));
		if ((spa = spa_lookup(pname)) != NULL) {
			if (spa->spa_state == POOL_STATE_ACTIVE) {
				spa_namespace_exit(FTAG);
				fnvlist_free(config);
				return (0);
			}
			spa_remove(spa);
		}
		spa = spa_add(pname, config, NULL);
		/* vdev_alloc() asks spa_version(). */
		if (nvlist_lookup_uint64(config, ZPOOL_CONFIG_VERSION,
		    &spa->spa_ubsync.ub_version) != 0)
			spa->spa_ubsync.ub_version = SPA_VERSION_INITIAL;
	} else if ((spa = spa_lookup(name)) == NULL) {
		spa_namespace_exit(FTAG);
		cmn_err(CE_NOTE, "Cannot find the pool label for '%s'", name);
		return (SET_ERROR(EIO));
	} else {
		config = fnvlist_dup(spa->spa_config);
	}
	spa->spa_is_root = B_TRUE;
	spa->spa_import_flags = ZFS_IMPORT_VERBATIM;
	if (checkpointrewind)
		spa->spa_import_flags |= ZFS_IMPORT_CHECKPOINT;

	nvtop = fnvlist_lookup_nvlist(config, ZPOOL_CONFIG_VDEV_TREE);
	spa_config_enter(spa, SCL_ALL, FTAG, RW_WRITER);
	error = spa_config_parse(spa, &rvd, nvtop, NULL, 0,
	    VDEV_ALLOC_ROOTPOOL);
	spa_config_exit(spa, SCL_ALL, FTAG);
	if (error != 0) {
		spa_namespace_exit(FTAG);
		fnvlist_free(config);
		cmn_err(CE_NOTE, "Can not parse the config for pool '%s'",
		    name);
		return (error);
	}
	spa_config_enter(spa, SCL_ALL, FTAG, RW_WRITER);
	vdev_free(rvd);
	spa_config_exit(spa, SCL_ALL, FTAG);
	spa_namespace_exit(FTAG);
	fnvlist_free(config);
	return (0);
}

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
