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

#ifndef _SPL_SYS_MOD_H
#define	_SPL_SYS_MOD_H

/*
 * The module's tunables, as sysctl nodes under vfs.zfs, in the same
 * tree FreeBSD gives them: the scope "zfs_vdev_mirror" is the node
 * vfs.zfs.vdev.mirror.  A NetBSD module's SYSCTL_SETUP functions are
 * run when it is loaded and their nodes torn down when it is unloaded
 * (kern_module.c), so each tunable is one such function.  Under
 * _MODULE their names are global, hence the prefix.
 *
 * A tunable with a handler takes it in the Linux shape, a string to
 * set from and a buffer to print into, which a string node carries;
 * see spl_sysctl.c.
 */
#include <sys/sysctl.h>

#define	ZMOD_RW		CTLFLAG_READWRITE
#define	ZMOD_RD		CTLFLAG_READONLY

typedef struct zfs_kernel_param {
	const char	*zkp_name;
	void		*zkp_arg;
} zfs_kernel_param_t;

typedef int (*spl_param_set_t)(const char *, zfs_kernel_param_t *);
typedef int (*spl_param_get_t)(char *, zfs_kernel_param_t *);

#define	ZFS_MODULE_PARAM_ARGS	const char *buf, zfs_kernel_param_t *kp

/*
 * Linux's generic handlers, which the common code names for tunables
 * whose setter is its own: print the variable, or parse into it.
 */
extern int param_get_uint(char *, zfs_kernel_param_t *);
extern int param_set_uint(const char *, zfs_kernel_param_t *);
extern int param_get_charp(char *, zfs_kernel_param_t *);
extern int spl_param_get_u64(char *, zfs_kernel_param_t *);
extern int param_set_int(const char *, zfs_kernel_param_t *);
extern int spl_param_set_u64(const char *, zfs_kernel_param_t *);
extern int param_set_charp(const char *, zfs_kernel_param_t *);

extern const struct sysctlnode *spl_sysctl_scope(struct sysctllog **,
    const char *);
extern void spl_sysctl_param(struct sysctllog **, const char *,
    const char *, int, int, void *, size_t, const char *);
extern void spl_sysctl_param_call(struct sysctllog **, const char *,
    const char *, int, spl_param_set_t, spl_param_get_t, void *,
    const char *);

#define	SPL_PARAM_TYPE_INT	CTLTYPE_INT, 0, sizeof (int)
#define	SPL_PARAM_TYPE_UINT	CTLTYPE_INT, CTLFLAG_UNSIGNED, sizeof (uint_t)
#define	SPL_PARAM_TYPE_U64	CTLTYPE_QUAD, CTLFLAG_UNSIGNED, sizeof (uint64_t)
#ifdef _LP64
#define	SPL_PARAM_TYPE_ULONG	CTLTYPE_QUAD, CTLFLAG_UNSIGNED, sizeof (ulong_t)
#else
#define	SPL_PARAM_TYPE_ULONG	CTLTYPE_INT, CTLFLAG_UNSIGNED, sizeof (ulong_t)
#endif

#define	SPL_PARAM_NODE(fn, leaf, type, flags, size, scope, perm, desc, data) \
	SYSCTL_SETUP(fn, desc)						\
	{								\
		spl_sysctl_param(clog, #scope, leaf, type,		\
		    (perm) | (flags), data, size, desc);		\
	}
#define	SPL_PARAM_NODE_X(fn, leaf, scope, perm, desc, data, ...)	\
	SPL_PARAM_NODE(fn, leaf, __VA_ARGS__, scope, perm, desc, data)

#define	ZFS_MODULE_PARAM(scope_prefix, name_prefix, name, type, perm, desc) \
	SPL_PARAM_NODE_X(zfs_sysctl_ ## name_prefix ## name, #name,	\
	    scope_prefix, perm, desc, &name_prefix ## name,		\
	    SPL_PARAM_TYPE_ ## type)

#define	ZFS_MODULE_PARAM_CALL(					\
    scope_prefix, name_prefix, name, setfunc, getfunc, perm, desc)	\
	SYSCTL_SETUP(zfs_sysctl_ ## name_prefix ## name, desc)		\
	{								\
		spl_sysctl_param_call(clog, #scope_prefix, #name,	\
		    perm, setfunc, getfunc, &name_prefix ## name, desc); \
	}

/* A tunable with no variable of its own: the handlers are all of it. */
#define	ZFS_MODULE_VIRTUAL_PARAM_CALL(				\
    scope_prefix, name_prefix, name, setfunc, getfunc, perm, desc)	\
	SYSCTL_SETUP(zfs_sysctl_ ## name_prefix ## name, desc)		\
	{								\
		spl_sysctl_param_call(clog, #scope_prefix, #name,	\
		    perm, setfunc, getfunc, NULL, desc);		\
	}

/*
 * The module's initialisers run from its modcmd, in the order it
 * names them (zfs_kmod.c); these mark them for Linux and FreeBSD.
 */
#define	module_init(fn)
#define	module_init_early(fn)
#define	module_exit(fn)

#endif	/* _SPL_SYS_MOD_H */
