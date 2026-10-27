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

#ifndef _SPL_SYS_TASKQ_H
#define	_SPL_SYS_TASKQ_H

/*
 * illumos's task queues.  NetBSD's workqueue(9) runs work but cannot
 * wait for one item, cancel one by id or run one after a delay, all of
 * which the common code asks of a taskq; so, as NetBSD's own ZFS did,
 * the queue is implemented here, with kernel threads of its own.  See
 * spl_taskq.c.
 */
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/thread.h>

#define	TASKQ_NAMELEN		31

typedef uintptr_t taskqid_t;
typedef void (task_func_t)(void *);

struct taskq;

typedef struct taskq_ent {
	TAILQ_ENTRY(taskq_ent)	tqent_link;
	task_func_t		*tqent_func;
	void			*tqent_arg;
	taskqid_t		tqent_id;
	clock_t			tqent_when;	/* lbolt to run at, or 0 */
	uint_t			tqent_flags;
	struct taskq		*tqent_taskq;
} taskq_ent_t;

typedef struct taskq taskq_t;

#define	TASKQ_PREPOPULATE	0x0001
#define	TASKQ_CPR_SAFE		0x0002
#define	TASKQ_DYNAMIC		0x0004
#define	TASKQ_THREADS_CPU_PCT	0x0008
#define	TASKQ_DC_BATCH		0x0010

#define	TQ_SLEEP		0x00
#define	TQ_NOSLEEP		0x01
#define	TQ_NOQUEUE		0x02
#define	TQ_NOALLOC		0x04
#define	TQ_FRONT		0x08

#define	TASKQID_INVALID		((taskqid_t)0)

extern taskq_t *system_taskq;
extern taskq_t *system_delay_taskq;

extern taskqid_t taskq_dispatch(taskq_t *, task_func_t, void *, uint_t);
extern taskqid_t taskq_dispatch_delay(taskq_t *, task_func_t, void *,
    uint_t, clock_t);
extern void taskq_dispatch_ent(taskq_t *, task_func_t, void *, uint_t,
    taskq_ent_t *);
extern int taskq_empty_ent(taskq_ent_t *);
extern void taskq_init_ent(taskq_ent_t *);
extern taskq_t *taskq_create(const char *, int, pri_t, int, int, uint_t);
extern taskq_t *taskq_create_synced(const char *, int, pri_t, int, int,
    uint_t, kthread_t ***);
extern taskq_t *taskq_create_proc(const char *, int, pri_t, int, int,
    struct proc *, uint_t);
extern taskq_t *taskq_create_sysdc(const char *, int, int, int,
    struct proc *, uint_t, uint_t);
extern void nulltask(void *);
extern void taskq_destroy(taskq_t *);
extern void taskq_wait_id(taskq_t *, taskqid_t);
extern void taskq_wait_outstanding(taskq_t *, taskqid_t);
extern void taskq_wait(taskq_t *);
extern int taskq_cancel_id(taskq_t *, taskqid_t, boolean_t);
extern int taskq_member(taskq_t *, kthread_t *);
extern taskq_t *taskq_of_curthread(void);
extern void taskq_suspend(taskq_t *);
extern int taskq_suspended(taskq_t *);
extern void taskq_resume(taskq_t *);

#endif	/* _SPL_SYS_TASKQ_H */
