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
 * illumos's task queues, on kernel threads of their own.
 *
 * A task has an id, which is never 0 and never reused, and is on one of
 * three lists while it exists: pending, waiting for a thread; delayed,
 * waiting for its time; or running, in the slot of the thread running
 * it.  Waiting for a task, or for every task up to one, and cancelling
 * one, look for its id on all three.  A thread with nothing pending
 * sleeps until it is given work or the earliest delayed task is due,
 * and then moves the due ones to the pending list itself.
 *
 * NetBSD's workqueue(9) would run the work, but not cancel or wait for
 * one item, nor run one after a delay, all of which the common code
 * does; NetBSD's own ZFS had a taskq of its own for the same reason.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/lwp.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/taskq.h>
#include <sys/debug.h>
#include <sys/time.h>

#define	TQENT_QUEUED	0x1	/* on the pending or delayed list */
#define	TQENT_DELAYED	0x2	/* on the delayed list */
#define	TQENT_PREALLOC	0x4	/* the caller's, from taskq_dispatch_ent() */

TAILQ_HEAD(taskq_list, taskq_ent);

struct taskq {
	char			tq_name[TASKQ_NAMELEN + 1];
	kmutex_t		tq_lock;
	kcondvar_t		tq_work_cv;	/* work, or time, for threads */
	kcondvar_t		tq_done_cv;	/* a task finished */
	struct taskq_list	tq_pending;
	struct taskq_list	tq_delayed;	/* in order of tqent_when */
	int			tq_nthreads;
	int			tq_nidle;
	lwp_t			**tq_threads;
	taskqid_t		*tq_running;	/* each thread's task, or 0 */
	taskqid_t		tq_nextid;
	boolean_t		tq_suspended;
	boolean_t		tq_exiting;
	int			tq_nexited;
};

taskq_t *system_taskq;
taskq_t *system_delay_taskq;

static specificdata_key_t taskq_tsd_key;

/* The thread's index, passed to it through its argument. */
struct taskq_thread_arg {
	taskq_t		*tta_tq;
	int		tta_index;
};

static taskqid_t
taskq_newid(taskq_t *tq)
{

	ASSERT(MUTEX_HELD(&tq->tq_lock));
	if (++tq->tq_nextid == TASKQID_INVALID)
		++tq->tq_nextid;
	return (tq->tq_nextid);
}

/* Move every delayed task that is due to the end of the pending list. */
static void
taskq_move_due(taskq_t *tq)
{
	taskq_ent_t *ent;
	clock_t now = ddi_get_lbolt();

	while ((ent = TAILQ_FIRST(&tq->tq_delayed)) != NULL &&
	    ent->tqent_when <= now) {
		TAILQ_REMOVE(&tq->tq_delayed, ent, tqent_link);
		ent->tqent_flags &= ~TQENT_DELAYED;
		TAILQ_INSERT_TAIL(&tq->tq_pending, ent, tqent_link);
	}
}

static void
taskq_thread(void *arg)
{
	struct taskq_thread_arg *tta = arg;
	taskq_t *tq = tta->tta_tq;
	int index = tta->tta_index;
	taskq_ent_t *ent;

	kmem_free(tta, sizeof (*tta));
	lwp_setspecific(taskq_tsd_key, tq);

	mutex_enter(&tq->tq_lock);
	for (;;) {
		taskq_move_due(tq);
		if (!tq->tq_suspended &&
		    (ent = TAILQ_FIRST(&tq->tq_pending)) != NULL) {
			task_func_t *func = ent->tqent_func;
			void *farg = ent->tqent_arg;
			boolean_t prealloc =
			    (ent->tqent_flags & TQENT_PREALLOC) != 0;

			TAILQ_REMOVE(&tq->tq_pending, ent, tqent_link);
			ent->tqent_flags &= ~TQENT_QUEUED;
			tq->tq_running[index] = ent->tqent_id;
			mutex_exit(&tq->tq_lock);

			/*
			 * A caller's entry may be reused as soon as the
			 * task starts, so it is not touched after this.
			 */
			if (!prealloc)
				kmem_free(ent, sizeof (*ent));
			func(farg);

			mutex_enter(&tq->tq_lock);
			tq->tq_running[index] = TASKQID_INVALID;
			cv_broadcast(&tq->tq_done_cv);
			continue;
		}
		if (tq->tq_exiting && TAILQ_EMPTY(&tq->tq_pending))
			break;

		tq->tq_nidle++;
		if ((ent = TAILQ_FIRST(&tq->tq_delayed)) != NULL)
			(void) cv_timedwait(&tq->tq_work_cv, &tq->tq_lock,
			    ent->tqent_when);
		else
			cv_wait(&tq->tq_work_cv, &tq->tq_lock);
		tq->tq_nidle--;
	}
	tq->tq_nexited++;
	cv_broadcast(&tq->tq_done_cv);
	mutex_exit(&tq->tq_lock);
	kthread_exit(0);
}

taskq_t *
taskq_create(const char *name, int nthreads, pri_t pri, int minalloc,
    int maxalloc, uint_t flags)
{
	taskq_t *tq;
	int i;

	(void) minalloc, (void) maxalloc;

	/* The number is a percentage of the CPUs. */
	if (flags & TASKQ_THREADS_CPU_PCT)
		nthreads = MAX(1, (ncpu * nthreads) / 100);
	if (nthreads < 1)
		nthreads = 1;

	tq = kmem_zalloc(sizeof (*tq), KM_SLEEP);
	strlcpy(tq->tq_name, name, sizeof (tq->tq_name));
	mutex_init(&tq->tq_lock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&tq->tq_work_cv, NULL, CV_DEFAULT, NULL);
	cv_init(&tq->tq_done_cv, NULL, CV_DEFAULT, NULL);
	TAILQ_INIT(&tq->tq_pending);
	TAILQ_INIT(&tq->tq_delayed);
	tq->tq_nthreads = nthreads;
	tq->tq_threads = kmem_zalloc(sizeof (lwp_t *) * nthreads, KM_SLEEP);
	tq->tq_running = kmem_zalloc(sizeof (taskqid_t) * nthreads,
	    KM_SLEEP);

	for (i = 0; i < nthreads; i++) {
		struct taskq_thread_arg *tta;

		tta = kmem_alloc(sizeof (*tta), KM_SLEEP);
		tta->tta_tq = tq;
		tta->tta_index = i;
		VERIFY0(kthread_create(pri, KTHREAD_MPSAFE | KTHREAD_MUSTJOIN,
		    NULL, taskq_thread, tta, &tq->tq_threads[i], "%s",
		    tq->tq_name));
	}
	return (tq);
}

taskq_t *
taskq_create_synced(const char *name, int nthreads, pri_t pri, int minalloc,
    int maxalloc, uint_t flags, kthread_t ***ktpp)
{
	taskq_t *tq;
	kthread_t **kthreads;

	/*
	 * The caller frees the array it is given, so it gets a copy of
	 * the taskq's own, which taskq_destroy() joins the threads from.
	 */
	tq = taskq_create(name, nthreads, pri, minalloc, maxalloc, flags);
	VERIFY3S(tq->tq_nthreads, ==, nthreads);
	kthreads = kmem_alloc(sizeof (*kthreads) * tq->tq_nthreads, KM_SLEEP);
	memcpy(kthreads, tq->tq_threads, sizeof (*kthreads) * tq->tq_nthreads);
	*ktpp = kthreads;
	return (tq);
}

taskq_t *
taskq_create_proc(const char *name, int nthreads, pri_t pri, int minalloc,
    int maxalloc, struct proc *proc, uint_t flags)
{

	(void) proc;
	return (taskq_create(name, nthreads, pri, minalloc, maxalloc, flags));
}

/* Whether any task with an id up to max, or with id exactly, remains. */
static boolean_t
taskq_has(taskq_t *tq, taskqid_t id, boolean_t upto)
{
	taskq_ent_t *ent;
	int i;

	ASSERT(MUTEX_HELD(&tq->tq_lock));
#define	TASKQ_MATCH(x)	((x) != TASKQID_INVALID && \
	    (upto ? (x) <= id : (x) == id))
	TAILQ_FOREACH(ent, &tq->tq_pending, tqent_link)
		if (TASKQ_MATCH(ent->tqent_id))
			return (B_TRUE);
	TAILQ_FOREACH(ent, &tq->tq_delayed, tqent_link)
		if (TASKQ_MATCH(ent->tqent_id))
			return (B_TRUE);
	for (i = 0; i < tq->tq_nthreads; i++)
		if (TASKQ_MATCH(tq->tq_running[i]))
			return (B_TRUE);
#undef	TASKQ_MATCH
	return (B_FALSE);
}

/*
 * Wait until the taskq is empty, delayed tasks included, as illumos and
 * Linux do.  That takes in the tasks dispatched while waiting, the ones
 * a running task dispatches among them: spa_evicting_os_wait() waits
 * for the eviction of a dataset this way, and that eviction dispatches
 * the eviction of its directory, which has to be waited for as well.
 */
void
taskq_wait(taskq_t *tq)
{
	int i;

	mutex_enter(&tq->tq_lock);
	for (;;) {
		boolean_t busy = !TAILQ_EMPTY(&tq->tq_pending) ||
		    !TAILQ_EMPTY(&tq->tq_delayed);

		for (i = 0; !busy && i < tq->tq_nthreads; i++)
			if (tq->tq_running[i] != TASKQID_INVALID)
				busy = B_TRUE;
		if (!busy)
			break;
		cv_wait(&tq->tq_done_cv, &tq->tq_lock);
	}
	mutex_exit(&tq->tq_lock);
}

void
taskq_wait_outstanding(taskq_t *tq, taskqid_t id)
{

	mutex_enter(&tq->tq_lock);
	if (id == TASKQID_INVALID)
		id = tq->tq_nextid;
	while (taskq_has(tq, id, B_TRUE))
		cv_wait(&tq->tq_done_cv, &tq->tq_lock);
	mutex_exit(&tq->tq_lock);
}

void
taskq_wait_id(taskq_t *tq, taskqid_t id)
{

	mutex_enter(&tq->tq_lock);
	while (taskq_has(tq, id, B_FALSE))
		cv_wait(&tq->tq_done_cv, &tq->tq_lock);
	mutex_exit(&tq->tq_lock);
}

void
taskq_destroy(taskq_t *tq)
{
	taskq_ent_t *ent;
	int i;

	/* Delayed tasks are dropped; illumos runs no task after destroy. */
	mutex_enter(&tq->tq_lock);
	while ((ent = TAILQ_FIRST(&tq->tq_delayed)) != NULL) {
		TAILQ_REMOVE(&tq->tq_delayed, ent, tqent_link);
		ent->tqent_flags &= ~(TQENT_QUEUED | TQENT_DELAYED);
		if (!(ent->tqent_flags & TQENT_PREALLOC))
			kmem_free(ent, sizeof (*ent));
	}
	tq->tq_suspended = B_FALSE;
	tq->tq_exiting = B_TRUE;
	cv_broadcast(&tq->tq_work_cv);
	mutex_exit(&tq->tq_lock);

	for (i = 0; i < tq->tq_nthreads; i++)
		(void) kthread_join(tq->tq_threads[i]);

	cv_destroy(&tq->tq_done_cv);
	cv_destroy(&tq->tq_work_cv);
	mutex_destroy(&tq->tq_lock);
	kmem_free(tq->tq_running, sizeof (taskqid_t) * tq->tq_nthreads);
	kmem_free(tq->tq_threads, sizeof (lwp_t *) * tq->tq_nthreads);
	kmem_free(tq, sizeof (*tq));
}

static void
taskq_enqueue(taskq_t *tq, taskq_ent_t *ent, task_func_t func, void *arg,
    uint_t flags, clock_t when)
{

	ASSERT(MUTEX_HELD(&tq->tq_lock));
	ent->tqent_func = func;
	ent->tqent_arg = arg;
	ent->tqent_taskq = tq;
	ent->tqent_id = taskq_newid(tq);
	ent->tqent_when = when;
	ent->tqent_flags |= TQENT_QUEUED;
	if (when != 0) {
		taskq_ent_t *e;

		ent->tqent_flags |= TQENT_DELAYED;
		TAILQ_FOREACH(e, &tq->tq_delayed, tqent_link)
			if (e->tqent_when > when)
				break;
		if (e != NULL)
			TAILQ_INSERT_BEFORE(e, ent, tqent_link);
		else
			TAILQ_INSERT_TAIL(&tq->tq_delayed, ent, tqent_link);
	} else if (flags & TQ_FRONT) {
		TAILQ_INSERT_HEAD(&tq->tq_pending, ent, tqent_link);
	} else {
		TAILQ_INSERT_TAIL(&tq->tq_pending, ent, tqent_link);
	}
	/* Every thread, so that one sleeping until a time sees the new one. */
	cv_broadcast(&tq->tq_work_cv);
}

taskqid_t
taskq_dispatch(taskq_t *tq, task_func_t func, void *arg, uint_t flags)
{
	taskq_ent_t *ent;
	taskqid_t id;

	ent = kmem_zalloc(sizeof (*ent),
	    (flags & (TQ_NOSLEEP | TQ_NOQUEUE)) ? KM_NOSLEEP : KM_SLEEP);
	if (ent == NULL)
		return (TASKQID_INVALID);
	mutex_enter(&tq->tq_lock);
	/* TQ_NOQUEUE: only if a thread is free to take it now. */
	if ((flags & TQ_NOQUEUE) && tq->tq_nidle == 0) {
		mutex_exit(&tq->tq_lock);
		kmem_free(ent, sizeof (*ent));
		return (TASKQID_INVALID);
	}
	taskq_enqueue(tq, ent, func, arg, flags, 0);
	id = ent->tqent_id;
	mutex_exit(&tq->tq_lock);
	return (id);
}

taskqid_t
taskq_dispatch_delay(taskq_t *tq, task_func_t func, void *arg, uint_t flags,
    clock_t expire_time)
{
	taskq_ent_t *ent;
	taskqid_t id;

	ent = kmem_zalloc(sizeof (*ent),
	    (flags & TQ_NOSLEEP) ? KM_NOSLEEP : KM_SLEEP);
	if (ent == NULL)
		return (TASKQID_INVALID);
	mutex_enter(&tq->tq_lock);
	/* A time already past is due now; 0 would read as "not delayed". */
	taskq_enqueue(tq, ent, func, arg, flags, MAX(expire_time, 1));
	id = ent->tqent_id;
	mutex_exit(&tq->tq_lock);
	return (id);
}

void
taskq_dispatch_ent(taskq_t *tq, task_func_t func, void *arg, uint_t flags,
    taskq_ent_t *ent)
{

	ASSERT(!(ent->tqent_flags & TQENT_QUEUED));
	mutex_enter(&tq->tq_lock);
	ent->tqent_flags = TQENT_PREALLOC;
	taskq_enqueue(tq, ent, func, arg, flags, 0);
	mutex_exit(&tq->tq_lock);
}

void
taskq_init_ent(taskq_ent_t *ent)
{

	memset(ent, 0, sizeof (*ent));
}

int
taskq_empty_ent(taskq_ent_t *ent)
{

	return (!(ent->tqent_flags & TQENT_QUEUED));
}

/*
 * illumos's answer: 0 if the task was taken off the queue before it
 * ran, EBUSY if it was running (after waiting for it, when asked to),
 * and ENOENT if there was no such task left.
 */
int
taskq_cancel_id(taskq_t *tq, taskqid_t id, boolean_t wait)
{
	taskq_ent_t *ent;
	int i;

	if (id == TASKQID_INVALID)
		return (ENOENT);
	mutex_enter(&tq->tq_lock);
	TAILQ_FOREACH(ent, &tq->tq_pending, tqent_link)
		if (ent->tqent_id == id)
			break;
	if (ent != NULL) {
		TAILQ_REMOVE(&tq->tq_pending, ent, tqent_link);
	} else {
		TAILQ_FOREACH(ent, &tq->tq_delayed, tqent_link)
			if (ent->tqent_id == id)
				break;
		if (ent != NULL)
			TAILQ_REMOVE(&tq->tq_delayed, ent, tqent_link);
	}
	if (ent != NULL) {
		ent->tqent_flags &= ~(TQENT_QUEUED | TQENT_DELAYED);
		cv_broadcast(&tq->tq_done_cv);
		mutex_exit(&tq->tq_lock);
		if (!(ent->tqent_flags & TQENT_PREALLOC))
			kmem_free(ent, sizeof (*ent));
		return (0);
	}
	for (i = 0; i < tq->tq_nthreads; i++)
		if (tq->tq_running[i] == id)
			break;
	if (i == tq->tq_nthreads) {
		mutex_exit(&tq->tq_lock);
		return (ENOENT);
	}
	if (wait)
		while (taskq_has(tq, id, B_FALSE))
			cv_wait(&tq->tq_done_cv, &tq->tq_lock);
	mutex_exit(&tq->tq_lock);
	return (EBUSY);
}

int
taskq_member(taskq_t *tq, kthread_t *thread)
{
	int i;

	for (i = 0; i < tq->tq_nthreads; i++)
		if (tq->tq_threads[i] == thread)
			return (1);
	return (0);
}

taskq_t *
taskq_of_curthread(void)
{

	return (lwp_getspecific(taskq_tsd_key));
}

void
taskq_suspend(taskq_t *tq)
{

	mutex_enter(&tq->tq_lock);
	tq->tq_suspended = B_TRUE;
	mutex_exit(&tq->tq_lock);
}

int
taskq_suspended(taskq_t *tq)
{

	return (tq->tq_suspended);
}

void
taskq_resume(taskq_t *tq)
{

	mutex_enter(&tq->tq_lock);
	tq->tq_suspended = B_FALSE;
	cv_broadcast(&tq->tq_work_cv);
	mutex_exit(&tq->tq_lock);
}

void
nulltask(void *arg)
{

	(void) arg;
}

int
spl_taskq_init(void)
{
	int error;

	error = lwp_specific_key_create(&taskq_tsd_key, NULL);
	if (error != 0)
		return (error);
	system_taskq = taskq_create("system_taskq", MAX(1, ncpu), minclsyspri,
	    0, 0, 0);
	system_delay_taskq = taskq_create("system_delay_taskq", MAX(1, ncpu),
	    minclsyspri, 0, 0, 0);
	return (0);
}

void
spl_taskq_fini(void)
{

	taskq_destroy(system_delay_taskq);
	taskq_destroy(system_taskq);
	lwp_specific_key_delete(taskq_tsd_key);
}
