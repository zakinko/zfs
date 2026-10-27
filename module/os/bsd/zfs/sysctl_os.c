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
 * The set handlers of the tunables whose new value has to be acted on,
 * in the Linux shape that <sys/mod.h> gives them: the new value as a
 * string, and a negative errno.
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/arc_impl.h>
#include <sys/mmp.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_raidz.h>

int
param_set_arc_u64(const char *buf, zfs_kernel_param_t *kp)
{
	int error;

	if ((error = spl_param_set_u64(buf, kp)) < 0)
		return (error);
	arc_tuning_update(B_TRUE);
	return (0);
}

int
param_set_arc_min(const char *buf, zfs_kernel_param_t *kp)
{

	return (param_set_arc_u64(buf, kp));
}

int
param_set_arc_max(const char *buf, zfs_kernel_param_t *kp)
{

	return (param_set_arc_u64(buf, kp));
}

int
param_set_arc_int(const char *buf, zfs_kernel_param_t *kp)
{
	int error;

	if ((error = param_set_int(buf, kp)) < 0)
		return (error);
	arc_tuning_update(B_TRUE);
	return (0);
}

int
param_set_arc_no_grow_shift(const char *buf, zfs_kernel_param_t *kp)
{
	unsigned long val;
	int error;

	(void) kp;
	if ((error = ddi_strtoul(buf, NULL, 0, &val)) != 0)
		return (-error);
	if (val >= arc_shrink_shift)
		return (-EINVAL);
	zfs_arc_no_grow_shift = val;
	return (0);
}

int
param_set_l2arc_dwpd_limit(const char *buf, zfs_kernel_param_t *kp)
{
	uint64_t old_val = l2arc_dwpd_limit;
	int error;

	if ((error = spl_param_set_u64(buf, kp)) < 0)
		return (error);
	if (l2arc_dwpd_limit != old_val)
		l2arc_dwpd_bump_reset();
	return (0);
}

int
param_set_multihost_interval(const char *buf, zfs_kernel_param_t *kp)
{
	int error;

	if ((error = spl_param_set_u64(buf, kp)) < 0)
		return (error);
	if (spa_mode_global != SPA_MODE_UNINIT)
		mmp_signal_all_threads();
	return (0);
}

int
param_set_deadman_failmode(const char *buf, zfs_kernel_param_t *kp)
{
	int error;

	if ((error = param_set_deadman_failmode_common(buf)) != 0)
		return (-error);
	return (param_set_charp(buf, kp));
}

int
param_set_deadman_ziotime(const char *buf, zfs_kernel_param_t *kp)
{
	int error;

	if ((error = spl_param_set_u64(buf, kp)) < 0)
		return (error);
	spa_set_deadman_ziotime(MSEC2NSEC(zfs_deadman_ziotime_ms));
	return (0);
}

int
param_set_deadman_synctime(const char *buf, zfs_kernel_param_t *kp)
{
	int error;

	if ((error = spl_param_set_u64(buf, kp)) < 0)
		return (error);
	spa_set_deadman_synctime(MSEC2NSEC(zfs_deadman_synctime_ms));
	return (0);
}

int
param_set_slop_shift(const char *buf, zfs_kernel_param_t *kp)
{
	unsigned long val;
	int error;

	if ((error = ddi_strtoul(buf, NULL, 0, &val)) != 0)
		return (-error);
	if (val < 1 || val > 31)
		return (-EINVAL);
	return (param_set_int(buf, kp));
}

int
param_set_active_allocator(const char *buf, zfs_kernel_param_t *kp)
{
	int error;

	if ((error = param_set_active_allocator_common(buf)) != 0)
		return (-error);
	return (param_set_charp(buf, kp));
}

int
param_get_raidz_impl(char *buf, zfs_kernel_param_t *kp)
{

	(void) kp;
	return (vdev_raidz_impl_get(buf, PAGE_SIZE));
}

int
param_set_raidz_impl(const char *buf, zfs_kernel_param_t *kp)
{

	(void) kp;
	return (vdev_raidz_impl_set(buf));
}

int
param_set_min_auto_ashift(const char *buf, zfs_kernel_param_t *kp)
{
	unsigned long val;
	int error;

	if ((error = ddi_strtoul(buf, NULL, 0, &val)) != 0)
		return (-error);
	if (val < ASHIFT_MIN || val > zfs_vdev_max_auto_ashift)
		return (-EINVAL);
	return (param_set_uint(buf, kp));
}

int
param_set_max_auto_ashift(const char *buf, zfs_kernel_param_t *kp)
{
	unsigned long val;
	int error;

	if ((error = ddi_strtoul(buf, NULL, 0, &val)) != 0)
		return (-error);
	if (val > ASHIFT_MAX || val < zfs_vdev_min_auto_ashift)
		return (-EINVAL);
	return (param_set_uint(buf, kp));
}
