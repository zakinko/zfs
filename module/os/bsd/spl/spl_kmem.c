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
 * illumos's object caches over NetBSD's pool_cache(9), and the string
 * allocators that go with kmem(9).
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kmem.h>
#include <sys/kmem_cache.h>
#include <sys/pool.h>
#include <sys/debug.h>
#include <uvm/uvm_extern.h>

struct kmem_cache {
	char		kc_name[32];
	pool_cache_t	kc_pc;
	size_t		kc_size;
	int		(*kc_constructor)(void *, void *, int);
	void		(*kc_destructor)(void *, void *);
	void		*kc_private;
};

/*
 * pool_cache(9) hands its constructor the cache's argument first and
 * the object second; illumos hands the object first and its private
 * pointer second.  The flags are KM_SLEEP or KM_NOSLEEP in both, which
 * NetBSD defines as PR_WAITOK and PR_NOWAIT.
 */
static int
kmem_cache_ctor(void *arg, void *obj, int flags)
{
	kmem_cache_t *kc = arg;

	if (kc->kc_constructor == NULL)
		return (0);
	return (kc->kc_constructor(obj, kc->kc_private, flags));
}

static void
kmem_cache_dtor(void *arg, void *obj)
{
	kmem_cache_t *kc = arg;

	if (kc->kc_destructor != NULL)
		kc->kc_destructor(obj, kc->kc_private);
}

/*
 * The reclaim callback and the vmem arena are illumos's own: NetBSD's
 * pool drains its caches when memory is short without being told how,
 * and every cache here takes kernel memory.  A size past a page goes to
 * one of pool(9)'s big allocators, which reach the largest block.
 */
kmem_cache_t *
kmem_cache_create(const char *name, size_t size, size_t align,
    int (*constructor)(void *, void *, int), void (*destructor)(void *, void *),
    void (*reclaim)(void *), void *private, vmem_t *vmp, int cflags)
{
	kmem_cache_t *kc;

	(void) reclaim, (void) vmp, (void) cflags;
	kc = kmem_zalloc(sizeof (*kc), KM_SLEEP);
	strlcpy(kc->kc_name, name, sizeof (kc->kc_name));
	kc->kc_size = size;
	kc->kc_constructor = constructor;
	kc->kc_destructor = destructor;
	kc->kc_private = private;
	kc->kc_pc = pool_cache_init(size, (u_int)align, 0, 0, kc->kc_name,
	    NULL, IPL_NONE, kmem_cache_ctor, kmem_cache_dtor, kc);
	return (kc);
}

void
kmem_cache_destroy(kmem_cache_t *kc)
{

	pool_cache_destroy(kc->kc_pc);
	kmem_free(kc, sizeof (*kc));
}

void *
kmem_cache_alloc(kmem_cache_t *kc, int flags)
{

	return (pool_cache_get(kc->kc_pc,
	    (flags & KM_NOSLEEP) ? PR_NOWAIT : PR_WAITOK));
}

void
kmem_cache_free(kmem_cache_t *kc, void *buf)
{

	pool_cache_put(kc->kc_pc, buf);
}

/* Give the cache's idle objects back to the system. */
void
kmem_cache_reap_soon(kmem_cache_t *kc)
{

	pool_cache_invalidate(kc->kc_pc);
}

/*
 * Whether a reap is under way.  illumos reaps asynchronously and ARC
 * waits for one to finish before starting another; pool_cache_invalidate()
 * is done when it returns, so there is never one in progress.
 */
boolean_t
kmem_cache_reap_active(void)
{

	return (B_FALSE);
}

void
kmem_reap(void)
{
}

uint64_t
spl_kmem_cache_inuse(kmem_cache_t *kc)
{

	return ((uint64_t)(pool_cache_nget(kc->kc_pc) -
	    pool_cache_nput(kc->kc_pc)));
}

uint64_t
spl_kmem_cache_entry_size(kmem_cache_t *kc)
{

	return (kc->kc_size);
}

/* illumos can move live objects between slabs; pool(9) cannot. */
void
spl_kmem_cache_set_move(kmem_cache_t *kc,
    kmem_cbrc_t (*move)(void *, void *, size_t, void *))
{

	(void) kc, (void) move;
}

int
kmem_debugging(void)
{

	return (0);
}

/* How much memory the kernel can allocate from: all of it. */
uint64_t
kmem_size(void)
{

	return ((uint64_t)physmem * PAGE_SIZE);
}

/*
 * The string is allocated at exactly its length plus one, because
 * kmem_strfree() frees strlen() + 1 bytes.
 */
char *
kmem_vasprintf(const char *fmt, va_list ap)
{
	va_list aq;
	char *buf;
	int len;

	va_copy(aq, ap);
	len = vsnprintf(NULL, 0, fmt, aq);
	va_end(aq);
	buf = kmem_alloc((size_t)len + 1, KM_SLEEP);
	(void) vsnprintf(buf, (size_t)len + 1, fmt, ap);
	return (buf);
}

char *
kmem_asprintf(const char *fmt, ...)
{
	va_list ap;
	char *buf;

	va_start(ap, fmt);
	buf = kmem_vasprintf(fmt, ap);
	va_end(ap);
	return (buf);
}

/*
 * Linux's scnprintf(): the number of characters written, not the number
 * that would have been, so that a caller adding them up stays within
 * its buffer.
 */
int
kmem_scnprintf(char *restrict str, size_t size, const char *restrict fmt,
    ...)
{
	va_list ap;
	int n;

	if (size == 0)
		return (0);
	va_start(ap, fmt);
	n = vsnprintf(str, size, fmt, ap);
	va_end(ap);
	if (n < 0)
		return (0);
	if ((size_t)n >= size)
		return ((int)(size - 1));
	return (n);
}
