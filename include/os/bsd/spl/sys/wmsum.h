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

#ifndef _SPL_SYS_WMSUM_H
#define	_SPL_SYS_WMSUM_H

/*
 * A sum written far more often than it is read: a counter per CPU,
 * added to without a lock and summed when read, over percpu(9), as
 * FreeBSD's is over counter(9).  The value read is not a snapshot;
 * additions made while it is summed may or may not be in it.
 */
#include <sys/types.h>
#include <sys/percpu.h>

typedef struct {
	percpu_t	*ws_pc;
} wmsum_t;

static __inline void
wmsum_add(wmsum_t *ws, int64_t delta)
{
	uint64_t *p;

	p = percpu_getref(ws->ws_pc);
	*p += (uint64_t)delta;
	percpu_putref(ws->ws_pc);
}

static __inline void
wmsum_init(wmsum_t *ws, uint64_t value)
{

	ws->ws_pc = percpu_alloc(sizeof (uint64_t));
	wmsum_add(ws, (int64_t)value);
}

static __inline void
wmsum_fini(wmsum_t *ws)
{

	percpu_free(ws->ws_pc, sizeof (uint64_t));
}

static __inline void
wmsum_value_cb(void *p, void *arg, struct cpu_info *ci)
{

	(void) ci;
	*(uint64_t *)arg += *(uint64_t *)p;
}

static __inline uint64_t
wmsum_value(wmsum_t *ws)
{
	uint64_t sum = 0;

	percpu_foreach(ws->ws_pc, wmsum_value_cb, &sum);
	return (sum);
}

#endif	/* _SPL_SYS_WMSUM_H */
