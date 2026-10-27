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
#include <sys/zio.h>
#include <sys/arc.h>
#include <sys/arc_impl.h>
#include <sys/sdt.h>
#include <uvm/uvm_extern.h>

/*
 * Memory the ARC may count on, in bytes, before the page daemon starts
 * to run: what is free beyond the page daemon's target.  The ARC's own
 * reap thread asks this every second and shrinks when it is negative;
 * NetBSD has no low-memory event to hear it from sooner.
 */
int64_t
arc_available_memory(void)
{
	int64_t lowest;

	lowest = ptob((int64_t)uvm_availmem(false) - uvmexp.freetarg);
	DTRACE_PROBE1(arc__available_memory, int64_t, lowest);
	return (lowest);
}

/*
 * Return a default max arc size based on the amount of physical memory.
 */
uint64_t
arc_default_max(uint64_t min, uint64_t allmem)
{
	uint64_t size;

	if (allmem >= 1 << 30)
		size = allmem - (1 << 30);
	else
		size = min;
	return (MAX(allmem * 5 / 8, size));
}

uint64_t
arc_all_memory(void)
{

	return (ptob((uint64_t)physmem));
}

int
arc_memory_throttle(spa_t *spa, uint64_t reserve, uint64_t txg)
{

	(void) spa, (void) reserve, (void) txg;
	return (0);
}

uint64_t
arc_free_memory(void)
{

	return (ptob((uint64_t)uvm_availmem(false)));
}

void
arc_lowmem_init(void)
{
}

void
arc_lowmem_fini(void)
{
}

void
arc_register_hotplug(void)
{
}

void
arc_unregister_hotplug(void)
{
}
