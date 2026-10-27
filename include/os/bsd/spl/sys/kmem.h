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

#ifndef _SPL_SYS_KMEM_H
#define	_SPL_SYS_KMEM_H

/*
 * NetBSD's kmem(9) is illumos's, by descent: kmem_alloc(),
 * kmem_zalloc() and kmem_free() take the same arguments, and KM_SLEEP
 * and KM_NOSLEEP mean the same.  It differs in refusing a size of 0,
 * which illumos answers with NULL and the common code relies on, so the
 * three are wrapped to take it.
 *
 * kmem_cache_t is illumos's object cache, which NetBSD has as
 * pool_cache(9); see spl_kmem.c.
 */
#include_next <sys/kmem.h>
#include <sys/types.h>
#include <sys/stdarg.h>

#define	KM_PUSHPAGE		KM_SLEEP
#define	KM_NORMALPRI		0

#define	KMC_NODEBUG		0x1
#define	KMC_RECLAIMABLE		0x0

#define	POINTER_IS_VALID(p)	(!((uintptr_t)(p) & 0x3))
#define	POINTER_INVALIDATE(pp)	(*(pp) = (void *)((uintptr_t)(*(pp)) | 0x1))

static __inline void *
spl_kmem_alloc(size_t size, int flags)
{

	if (size == 0)
		return (NULL);
	return ((kmem_alloc)(size, (km_flag_t)flags));
}

static __inline void *
spl_kmem_zalloc(size_t size, int flags)
{

	if (size == 0)
		return (NULL);
	return ((kmem_zalloc)(size, (km_flag_t)flags));
}

static __inline void
spl_kmem_free(void *buf, size_t size)
{

	if (buf == NULL || size == 0)
		return;
	(kmem_free)(buf, size);
}

#define	kmem_alloc(size, flags)		spl_kmem_alloc((size), (flags))
#define	kmem_zalloc(size, flags)	spl_kmem_zalloc((size), (flags))
#define	kmem_free(buf, size)		spl_kmem_free((buf), (size))

/*
 * illumos's vmem_*() for whole allocations, which kmem(9) also serves.
 * NetBSD's <sys/vmem.h> declares functions of these names with other
 * arguments; it is read before the names become macros.
 */
#include <sys/vmem.h>
#define	vmem_alloc(size, flags)		spl_kmem_alloc((size), (flags))
#define	vmem_zalloc(size, flags)	spl_kmem_zalloc((size), (flags))
#define	vmem_free(buf, size)		spl_kmem_free((buf), (size))

extern char *kmem_asprintf(const char *, ...)
    __attribute__((format(__printf__, 1, 2)));
extern char *kmem_vasprintf(const char *, va_list)
    __attribute__((format(__printf__, 1, 0)));
extern int kmem_scnprintf(char *restrict, size_t, const char *restrict, ...)
    __attribute__((format(__printf__, 3, 4)));

typedef struct kmem_cache kmem_cache_t;

extern kmem_cache_t *kmem_cache_create(const char *, size_t, size_t,
    int (*)(void *, void *, int), void (*)(void *, void *),
    void (*)(void *), void *, vmem_t *, int);
extern void kmem_cache_destroy(kmem_cache_t *);
extern void *kmem_cache_alloc(kmem_cache_t *, int);
extern void kmem_cache_free(kmem_cache_t *, void *);
extern void kmem_cache_reap_soon(kmem_cache_t *);
extern boolean_t kmem_cache_reap_active(void);
extern uint64_t spl_kmem_cache_inuse(kmem_cache_t *);
extern uint64_t spl_kmem_cache_entry_size(kmem_cache_t *);
extern void kmem_reap(void);
extern int kmem_debugging(void);
extern uint64_t kmem_size(void);

#define	kmem_cache_reap_now	kmem_cache_reap_soon

#endif	/* _SPL_SYS_KMEM_H */
