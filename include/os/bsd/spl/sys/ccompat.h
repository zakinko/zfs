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

#ifndef _SPL_SYS_CCOMPAT_H
#define	_SPL_SYS_CCOMPAT_H

/*
 * The few Linux kernel types the common code's zvol uses: a hash
 * chain and an atomic counter.
 */
#include <sys/types.h>
#include <sys/atomic.h>

struct hlist_node {
	struct hlist_node *next, **pprev;
};

struct hlist_head {
	struct hlist_node *first;
};

typedef struct {
	volatile unsigned int counter;
} atomic_t;

#ifndef container_of
#define	container_of(ptr, type, member)					\
	((type *)(void *)((char *)(ptr) - offsetof(type, member)))
#endif

#define	hlist_for_each(p, head)						\
	for (p = (head)->first; p; p = (p)->next)
#define	hlist_entry(ptr, type, field)	container_of(ptr, type, field)

static __inline void
hlist_add_head(struct hlist_node *n, struct hlist_head *h)
{

	n->next = h->first;
	if (h->first != NULL)
		h->first->pprev = &n->next;
	atomic_store_relaxed(&h->first, n);
	n->pprev = &h->first;
}

static __inline void
hlist_del(struct hlist_node *n)
{

	atomic_store_relaxed(n->pprev, n->next);
	if (n->next != NULL)
		n->next->pprev = n->pprev;
}

#define	HLIST_HEAD_INIT		{ }
#define	HLIST_HEAD(name)	struct hlist_head name = HLIST_HEAD_INIT
#define	INIT_HLIST_HEAD(head)	((head)->first = NULL)
#define	INIT_HLIST_NODE(node)	do {					\
	(node)->next = NULL;						\
	(node)->pprev = NULL;						\
} while (0)

static __inline int
atomic_read(const atomic_t *v)
{

	return ((int)atomic_load_relaxed(&v->counter));
}

static __inline int
atomic_inc(atomic_t *v)
{

	return ((int)atomic_inc_uint_nv(&v->counter));
}

static __inline int
atomic_dec(atomic_t *v)
{

	return ((int)atomic_dec_uint_nv(&v->counter));
}

#endif	/* _SPL_SYS_CCOMPAT_H */
