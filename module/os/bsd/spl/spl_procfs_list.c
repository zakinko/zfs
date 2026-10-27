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
 * Linux's procfs lists, the debug message log and the txg history among
 * them, as raw kstats: reading the node prints the header and then each
 * element in turn.  The kstat's lock is the list's, and spl_kstat.c
 * holds it for the whole walk, so the one cursor a list keeps is never
 * shared.
 */

#include <sys/types.h>
#include <sys/kmem.h>
#include <sys/list.h>
#include <sys/mutex.h>
#include <sys/kstat.h>
#include <sys/procfs_list.h>
#include <sys/debug.h>

/* What a record is: the element, and the list that knows how to show it. */
typedef struct procfs_list_cursor {
	procfs_list_t	*plc_pl;
	void		*plc_elt;
} procfs_list_cursor_t;

static int
procfs_list_update(kstat_t *ksp, int rw)
{
	procfs_list_t *pl = ksp->ks_private;

	if (rw == KSTAT_WRITE && pl->pl_clear != NULL)
		return (pl->pl_clear(pl));
	return (0);
}

static void *
procfs_list_addr(kstat_t *ksp, loff_t n)
{
	procfs_list_cursor_t *plc = ksp->ks_private1;
	procfs_list_t *pl = plc->plc_pl;

	ASSERT(MUTEX_HELD(&pl->pl_lock));
	if (n == 0)
		plc->plc_elt = list_head(&pl->pl_list);
	else if (plc->plc_elt != NULL)
		plc->plc_elt = list_next(&pl->pl_list, plc->plc_elt);
	return (plc->plc_elt != NULL ? plc : NULL);
}

static int
procfs_list_data(char *buf, size_t size, void *data)
{
	procfs_list_cursor_t *plc = data;
	struct seq_file f = { .sf_buf = buf, .sf_size = size };

	return (plc->plc_pl->pl_show(&f, plc->plc_elt));
}

void
procfs_list_install(const char *module, const char *submodule,
    const char *name, mode_t mode, procfs_list_t *pl,
    int (*show)(struct seq_file *, void *),
    int (*show_header)(struct seq_file *),
    int (*clear)(procfs_list_t *), size_t node_off)
{
	kstat_t *ksp;
	procfs_list_cursor_t *plc;

	(void) mode;
	mutex_init(&pl->pl_lock, NULL, MUTEX_DEFAULT, NULL);
	list_create(&pl->pl_list, node_off + sizeof (procfs_list_node_t),
	    node_off + offsetof(procfs_list_node_t, pln_link));
	pl->pl_show = show;
	pl->pl_show_header = show_header;
	pl->pl_clear = clear;
	pl->pl_next_id = 1;
	pl->pl_node_offset = node_off;

	ksp = kstat_create(module, 0, name, submodule != NULL ? submodule : "",
	    KSTAT_TYPE_RAW, 0, KSTAT_FLAG_VIRTUAL);
	if (ksp == NULL)
		return;
	plc = kmem_zalloc(sizeof (*plc), KM_SLEEP);
	plc->plc_pl = pl;
	ksp->ks_lock = &pl->pl_lock;
	ksp->ks_private = pl;
	ksp->ks_private1 = plc;
	ksp->ks_update = procfs_list_update;
	kstat_set_seq_raw_ops(ksp, show_header, procfs_list_data,
	    procfs_list_addr);
	kstat_install(ksp);
	pl->pl_private = ksp;
}

void
procfs_list_uninstall(procfs_list_t *pl)
{

	(void) pl;
}

void
procfs_list_destroy(procfs_list_t *pl)
{
	kstat_t *ksp = pl->pl_private;

	ASSERT(list_is_empty(&pl->pl_list));
	if (ksp != NULL) {
		kmem_free(ksp->ks_private1, sizeof (procfs_list_cursor_t));
		kstat_delete(ksp);
	}
	list_destroy(&pl->pl_list);
	mutex_destroy(&pl->pl_lock);
}

#define	NODE_ID(pl, obj)						\
	(((procfs_list_node_t *)(((char *)(obj)) +			\
	    (pl)->pl_node_offset))->pln_id)

void
procfs_list_add(procfs_list_t *pl, void *p)
{

	ASSERT(MUTEX_HELD(&pl->pl_lock));
	NODE_ID(pl, p) = pl->pl_next_id++;
	list_insert_tail(&pl->pl_list, p);
}
