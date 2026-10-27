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

#ifndef _SPL_SYS_PROC_H
#define	_SPL_SYS_PROC_H

/*
 * illumos's kernel threads are NetBSD's LWPs.
 */
#include <sys/param.h>
#include_next <sys/proc.h>
#include <sys/lwp.h>
#include <sys/kthread.h>

#define	CPU		curcpu()
#define	curthread	curlwp

/*
 * Priorities run upwards on both systems.  FreeBSD puts the module's
 * threads from PRIBIO up to PVM, below its page daemon; the same band
 * here is PRI_BIO to PRI_VM, under PRI_PGDAEMON and PRI_XCALL.
 * MAXPRI_KTHREAD, which NetBSD's own ZFS used, is PRI_XCALL itself.
 */
#define	minclsyspri	PRI_BIO
#define	defclsyspri	minclsyspri
#define	wtqclsyspri	((PRI_BIO + PRI_VM) / 2)
#define	maxclsyspri	PRI_VM

#define	max_ncpus	ncpu
#define	boot_max_ncpus	ncpu

#define	TS_RUN		0
#define	p0		proc0

typedef struct lwp	_kthread;
typedef struct lwp	kthread_t;
typedef struct lwp	*kthread_id_t;
typedef struct proc	proc_t;

extern kthread_t *spl_thread_create(void (*)(void *), void *, pri_t,
    const char *);

#define	thread_create_named(name, stk, stksize, proc, arg, len, pp,	\
    state, pri)								\
	spl_thread_create((proc), (arg), (pri), (name))
#define	thread_create(stk, stksize, proc, arg, len, pp, state, pri)	\
	spl_thread_create((proc), (arg), (pri), #proc)
#define	thread_exit()	kthread_exit(0)

static __inline boolean_t
zfs_proc_is_caller(proc_t *p)
{

	return (p == curproc);
}

#endif	/* _SPL_SYS_PROC_H */
