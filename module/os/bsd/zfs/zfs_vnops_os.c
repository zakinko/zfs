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
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2012, 2015 by Delphix. All rights reserved.
 * Copyright (c) 2014 Integros [integros.com]
 * Copyright 2017 Nexenta Systems, Inc.
 * Copyright (c) 2025, Klara, Inc.
 * Copyright (c) 2026, TrueNAS.
 */

/* Portions Copyright 2007 Jeremy Teo */
/* Portions Copyright 2010 Robert Milkowski */

#include <sys/param.h>
#include <sys/time.h>
#include <sys/systm.h>
#include <sys/sysmacros.h>
/*
 * The vnode operations of the ZFS file system, for NetBSD.
 *
 * The operations themselves -- creating, removing, renaming, the
 * attributes -- are FreeBSD's, whose vnode protocol is close to
 * NetBSD's: VOP_RENAME hands both systems the same locks, and a new
 * file's vnode comes back from zfs_mknode() locked.  What is NetBSD's
 * own is below them: the lookup, which returns its vnode unlocked; the
 * directory entries; the page cache of a mapped file; and the VOP
 * entry points.
 *
 * The page cache and the DMU are two copies of a mapped file's data,
 * kept in step as on FreeBSD.  A write through the DMU is copied into
 * the pages that are resident (update_pages()), and a read takes them
 * in preference to the DMU (mappedread()); the pager fills pages from
 * the DMU (zfs_getpages()) and writes dirty ones back into it
 * (zfs_gop_write()).  The pager busies its pages first and then takes
 * the range lock; read and write take the range lock and never wait on
 * a busy page, so the two orders do not meet.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/systm.h>
#include <sys/sysmacros.h>
#include <sys/resource.h>
#include <sys/vfs.h>
#include <sys/endian.h>
#include <sys/vnode.h>
#include <sys/dirent.h>
#include <sys/extattr.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/kmem.h>
#include <sys/malloc.h>
#include <sys/taskq.h>
#include <sys/uio.h>
#include <sys/atomic.h>
#include <sys/namei.h>
#include <sys/mman.h>
#include <sys/cmn_err.h>
#include <sys/errno.h>
#include <sys/unistd.h>
#include <sys/fcntl.h>
#include <sys/filio.h>
#include <sys/lockf.h>
#include <sys/buf.h>
#include <sys/zfs_dir.h>
#include <sys/zfs_ioctl.h>
#include <sys/fs/zfs.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/dsl_dataset.h>
#include <sys/spa.h>
#include <sys/txg.h>
#include <sys/dbuf.h>
#include <sys/zap.h>
#include <sys/sa.h>
#include <sys/policy.h>
#include <sys/sunddi.h>
#include <sys/sid.h>
#include <sys/zfs_ctldir.h>
#include <sys/zfs_fuid.h>
#include <sys/zfs_quota.h>
#include <sys/zfs_sa.h>
#include <sys/zfs_rlock.h>
#include <sys/zfs_project.h>
#include <sys/zil.h>
#include <sys/zfs_vnops.h>
#include <sys/dmu_impl.h>
#include <sys/brt.h>
#include <sys/zfeature.h>
#include <miscfs/fifofs/fifo.h>
#include <miscfs/genfs/genfs.h>
#include <miscfs/genfs/genfs_node.h>
#include <miscfs/specfs/specdev.h>
#include <uvm/uvm_extern.h>
#include <uvm/uvm_page.h>
#include <uvm/uvm_pager.h>

int (**zfs_vnodeop_p)(void *);
int (**zfs_fifoop_p)(void *);
int (**zfs_specop_p)(void *);

static int zfs_gop_write(vnode_t *, struct vm_page **, int, int);

/*
 * genfs_do_putpages() calls gop_putrange without asking; of these only
 * gop_markupdate may be left NULL.
 */
const struct genfs_ops zfs_genfsops = {
	.gop_size = genfs_size,
	.gop_write = zfs_gop_write,
	.gop_putrange = genfs_gop_putrange,
};

/*
 * Programming rules.
 *
 * Each vnode op performs some logical unit of work.  To do this, the ZPL must
 * properly lock its in-core state, create a DMU transaction, do the work,
 * record this work in the intent log (ZIL), commit the DMU transaction,
 * and wait for the intent log to commit if it is a synchronous operation.
 * Moreover, the vnode ops must work in both normal and log replay context.
 * The ordering of events is important to avoid deadlocks and references
 * to freed memory.  The example below illustrates the following Big Rules:
 *
 *  (1)	A check must be made in each zfs thread for a mounted file system.
 *	This is done avoiding races using zfs_enter(zfsvfs).
 *	A zfs_exit(zfsvfs) is needed before all returns.  Any znodes
 *	must be checked with zfs_verify_zp(zp).  Both of these macros
 *	can return EIO from the calling function.
 *
 *  (2)	VN_RELE() should always be the last thing except for zil_commit()
 *	(if necessary) and zfs_exit(). This is for 3 reasons:
 *	First, if it's the last reference, the vnode/znode
 *	can be freed, so the zp may point to freed memory.  Second, the last
 *	reference will call zfs_zinactive(), which may induce a lot of work --
 *	pushing cached pages (which acquires range locks) and syncing out
 *	cached atime changes.  Third, zfs_zinactive() may require a new tx,
 *	which could deadlock the system if you were already holding one.
 *	If you must call VN_RELE() within a tx then use VN_RELE_ASYNC().
 *
 *  (3)	All range locks must be grabbed before calling dmu_tx_assign(),
 *	as they can span dmu_tx_assign() calls.
 *
 *  (4) If ZPL locks are held, pass DMU_TX_NOWAIT as the second argument to
 *      dmu_tx_assign().  This is critical because we don't want to block
 *      while holding locks.
 *
 *	If no ZPL locks are held (aside from zfs_enter()), use DMU_TX_WAIT.
 *	This reduces lock contention and CPU usage when we must wait (note
 *	that if throughput is constrained by the storage, nearly every
 *	transaction must wait).
 *
 *      Note, in particular, that if a lock is sometimes acquired before
 *      the tx assigns, and sometimes after (e.g. z_lock), then failing
 *      to use a non-blocking assign can deadlock the system.  The scenario:
 *
 *	Thread A has grabbed a lock before calling dmu_tx_assign().
 *	Thread B is in an already-assigned tx, and blocks for this lock.
 *	Thread A calls dmu_tx_assign(DMU_TX_WAIT) and blocks in
 *	txg_wait_open() forever, because the previous txg can't quiesce
 *	until B's tx commits.
 *
 *	If dmu_tx_assign() returns ERESTART and zfsvfs->z_assign is
 *	DMU_TX_NOWAIT, then drop all locks, call dmu_tx_wait(), and try
 *	again.  On subsequent calls to dmu_tx_assign(), pass
 *	DMU_TX_NOTHROTTLE in addition to DMU_TX_NOWAIT, to indicate that
 *	this operation has already called dmu_tx_wait().  This will ensure
 *	that we don't retry forever, waiting a short bit each time.
 *
 *  (5)	If the operation succeeded, generate the intent log entry for it
 *	before dropping locks.  This ensures that the ordering of events
 *	in the intent log matches the order in which they actually occurred.
 *	During ZIL replay the zfs_log_* functions will update the sequence
 *	number to indicate the zil transaction has replayed.
 *
 *  (6)	At the end of each vnode op, the DMU tx must always commit,
 *	regardless of whether there were any errors.
 *
 *  (7)	After dropping all locks, invoke zil_commit(zilog, foid)
 *	to ensure that synchronous semantics are provided when necessary.
 *
 * In general, this is how things should be ordered in each vnode op:
 *
 *	zfs_enter(zfsvfs);		// exit if unmounted
 * top:
 *	zfs_dirent_lookup(&dl, ...)	// lock directory entry (may VN_HOLD())
 *	rw_enter(...);			// grab any other locks you need
 *	tx = dmu_tx_create(...);	// get DMU tx
 *	dmu_tx_hold_*();		// hold each object you might modify
 *	error = dmu_tx_assign(tx,
 *	    (waited ? DMU_TX_NOTHROTTLE : 0) | DMU_TX_NOWAIT);
 *	if (error) {
 *		rw_exit(...);		// drop locks
 *		zfs_dirent_unlock(dl);	// unlock directory entry
 *		VN_RELE(...);		// release held vnodes
 *		if (error == ERESTART) {
 *			waited = B_TRUE;
 *			dmu_tx_wait(tx);
 *			dmu_tx_abort(tx);
 *			goto top;
 *		}
 *		dmu_tx_abort(tx);	// abort DMU tx
 *		zfs_exit(zfsvfs);	// finished in zfs
 *		return (error);		// really out of space
 *	}
 *	error = do_real_work();		// do whatever this VOP does
 *	if (error == 0)
 *		zfs_log_*(...);		// on success, make ZIL entry
 *	dmu_tx_commit(tx);		// commit DMU tx -- error or not
 *	rw_exit(...);			// drop locks
 *	zfs_dirent_unlock(dl);		// unlock directory entry
 *	VN_RELE(...);			// release held vnodes
 *	zil_commit(zilog, foid);	// synchronous when necessary
 *	zfs_exit(zfsvfs);		// finished in zfs
 *	return (error);			// done, report error
 */
static int
zfs_open(vnode_t **vpp, int flag, cred_t *cr)
{
	(void) cr;
	znode_t	*zp = VTOZ(*vpp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	int error;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	if ((flag & FWRITE) && (zp->z_pflags & ZFS_APPENDONLY) &&
	    ((flag & FAPPEND) == 0)) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EPERM));
	}

	/*
	 * Keep a count of the synchronous opens in the znode.  On first
	 * synchronous open we must convert all previous async transactions
	 * into sync to keep correct ordering.
	 * Skip it for snapshot, as it won't have any transactions.
	 */
	if (!zfsvfs->z_issnap && (flag & O_SYNC)) {
		if (atomic_inc_32_nv(&zp->z_sync_cnt) == 1)
			zil_async_to_sync(zfsvfs->z_log, zp->z_id);
	}

	zfs_exit(zfsvfs, FTAG);
	return (0);
}

static int
zfs_close(vnode_t *vp, int flag, int count, offset_t offset, cred_t *cr)
{
	(void) offset, (void) cr;
	znode_t	*zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	int error;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	/* Decrement the synchronous opens in the znode */
	if (!zfsvfs->z_issnap && (flag & O_SYNC) && (count == 1))
		atomic_dec_32(&zp->z_sync_cnt);

	zfs_exit(zfsvfs, FTAG);
	return (0);
}

static int
zfs_ioctl_getxattr(vnode_t *vp, zfsxattr_t *fsx)
{
	znode_t *zp = VTOZ(vp);

	memset(fsx, 0, sizeof (*fsx));
	fsx->fsx_xflags = (zp->z_pflags & ZFS_PROJINHERIT) ?
	    FS_PROJINHERIT_FL : 0;
	fsx->fsx_projid = zp->z_projid;

	return (0);
}

static int
zfs_ioctl_setflags(vnode_t *vp, uint32_t ioctl_flags, xvattr_t *xva)
{
	uint64_t zfs_flags = VTOZ(vp)->z_pflags;
	xoptattr_t *xoap;

	if (ioctl_flags & ~(FS_PROJINHERIT_FL))
		return (SET_ERROR(EOPNOTSUPP));

	xva_init(xva);
	xoap = xva_getxoptattr(xva);

#define	FLAG_CHANGE(iflag, zflag, xflag, xfield)	do {		\
	if (((ioctl_flags & (iflag)) && !(zfs_flags & (zflag))) ||	\
	    ((zfs_flags & (zflag)) && !(ioctl_flags & (iflag)))) {	\
		XVA_SET_REQ(xva, (xflag));				\
		(xfield) = ((ioctl_flags & (iflag)) != 0);		\
	}								\
} while (0)

	FLAG_CHANGE(FS_PROJINHERIT_FL, ZFS_PROJINHERIT, XAT_PROJINHERIT,
	    xoap->xoa_projinherit);

#undef	FLAG_CHANGE

	return (0);
}

static int
zfs_ioctl_setxattr(vnode_t *vp, zfsxattr_t *fsx, cred_t *cr)
{
	znode_t *zp = VTOZ(vp);
	xvattr_t xva;
	xoptattr_t *xoap;
	int err;

	if (!zpl_is_valid_projid(fsx->fsx_projid))
		return (SET_ERROR(EINVAL));

	err = zfs_ioctl_setflags(vp, fsx->fsx_xflags, &xva);
	if (err)
		return (err);

	xoap = xva_getxoptattr(&xva);
	XVA_SET_REQ(&xva, XAT_PROJID);
	xoap->xoa_projid = fsx->fsx_projid;

	err = zfs_setattr(zp, (vattr_t *)&xva, 0, cr);

	return (err);
}

static int
zfs_ioctl(vnode_t *vp, ulong_t com, intptr_t data, int flag, cred_t *cred,
    int *rvalp)
{
	(void) flag, (void) cred, (void) rvalp;
	loff_t off;
	int error;

	switch (com) {
	case F_SEEK_DATA:
	case F_SEEK_HOLE:
	{
		off = *(offset_t *)data;
		error = vn_lock(vp, LK_SHARED);
		if (error)
			return (error);
		/* offset parameter is in/out */
		error = zfs_holey(VTOZ(vp), com, &off);
		VOP_UNLOCK(vp);
		if (error)
			return (error);
		*(offset_t *)data = off;
		return (0);
	}
	case ZFS_IOC_FSGETXATTR: {
		zfsxattr_t *fsx = (zfsxattr_t *)data;
		error = vn_lock(vp, LK_SHARED);
		if (error)
			return (error);
		error = zfs_ioctl_getxattr(vp, fsx);
		VOP_UNLOCK(vp);
		return (error);
	}
	case ZFS_IOC_FSSETXATTR: {
		zfsxattr_t *fsx = (zfsxattr_t *)data;
		error = vn_lock(vp, LK_EXCLUSIVE);
		if (error)
			return (error);
		error = zfs_ioctl_setxattr(vp, fsx, cred);
		VOP_UNLOCK(vp);
		return (error);
	}
	case ZFS_IOC_REWRITE: {
		zfs_rewrite_args_t *args = (zfs_rewrite_args_t *)data;
		if ((flag & FWRITE) == 0)
			return (SET_ERROR(EBADF));
		error = vn_lock(vp, LK_SHARED);
		if (error)
			return (error);
		error = zfs_rewrite(VTOZ(vp), args->off, args->len,
		    args->flags, args->arg);
		VOP_UNLOCK(vp);
		return (error);
	}
	}
	return (SET_ERROR(ENOTTY));
}

/*
 * A page of kernel address space to look at a page through.  It is
 * taken before the object is locked, since getting it may sleep, and
 * entering a page into it does not.
 */
static vaddr_t
zfs_page_window(void)
{

	return (uvm_km_alloc(kernel_map, PAGE_SIZE, 0,
	    UVM_KMF_VAONLY | UVM_KMF_WAITVA));
}

static void
zfs_page_window_free(vaddr_t win)
{

	uvm_km_free(kernel_map, win, PAGE_SIZE, UVM_KMF_VAONLY);
}

/*
 * Copy between a resident page and a buffer, with the object locked so
 * that the page stays where it is, whoever has it busy: the copy only
 * looks at it through the window.
 */
static void
zfs_page_copy(struct uvm_object *uobj, struct vm_page *pg, vaddr_t win,
    int off, void *buf, int len, boolean_t topage)
{
	char *va = (char *)win;

	KASSERT(rw_lock_held(uobj->vmobjlock));
	pmap_kenter_pa(win, VM_PAGE_TO_PHYS(pg),
	    topage ? VM_PROT_READ | VM_PROT_WRITE : VM_PROT_READ, 0);
	pmap_update(pmap_kernel());
	if (topage)
		memcpy(va + off, buf, len);
	else
		memcpy(buf, va + off, len);
	pmap_kremove(win, PAGE_SIZE);
	pmap_update(pmap_kernel());
}

/*
 * A write through the DMU is copied into the pages of the range that
 * are resident.  A page being filled by zfs_getpages() is left alone:
 * it reads the DMU once this write has let go of the range lock.
 */
void
update_pages(znode_t *zp, int64_t start, int len, objset_t *os)
{
	vnode_t *vp = ZTOV(zp);
	struct uvm_object *uobj = &vp->v_uobj;
	struct vm_page *pg;
	vaddr_t win;
	char *buf;
	int off, nbytes;

	buf = kmem_alloc(PAGE_SIZE, KM_SLEEP);
	win = zfs_page_window();
	off = start & PAGE_MASK;
	for (start = trunc_page(start); len > 0; start += PAGE_SIZE) {
		nbytes = MIN(PAGE_SIZE - off, len);
		if (dmu_read(os, zp->z_id, start + off, nbytes, buf,
		    DMU_READ_PREFETCH) == 0) {
			rw_enter(uobj->vmobjlock, RW_WRITER);
			pg = uvm_pagelookup(uobj, start);
			if (pg != NULL && (pg->flags & PG_FAKE) == 0)
				zfs_page_copy(uobj, pg, win, off, buf,
				    nbytes, B_TRUE);
			rw_exit(uobj->vmobjlock);
		}
		len -= nbytes;
		off = 0;
	}
	zfs_page_window_free(win);
	kmem_free(buf, PAGE_SIZE);
}

/*
 * A read of a range some of whose pages are resident takes those from
 * the pages, which may hold a mapping's writes not yet in the DMU, and
 * the rest from the DMU.  A page is copied out under the object lock
 * and handed to the caller from a buffer, so that a fault on the
 * caller's own buffer cannot wait on it.
 */
int
mappedread(znode_t *zp, int nbytes, zfs_uio_t *uio)
{
	vnode_t *vp = ZTOV(zp);
	struct uvm_object *uobj = &vp->v_uobj;
	struct vm_page *pg;
	int64_t start;
	int len = nbytes, off, bytes;
	vaddr_t win;
	char *buf;
	int error = 0;

	buf = kmem_alloc(PAGE_SIZE, KM_SLEEP);
	win = zfs_page_window();
	start = zfs_uio_offset(uio);
	off = start & PAGE_MASK;
	for (start = trunc_page(start); len > 0; start += PAGE_SIZE) {
		bytes = MIN(PAGE_SIZE - off, len);
		rw_enter(uobj->vmobjlock, RW_WRITER);
		pg = uvm_pagelookup(uobj, start);
		if (pg != NULL && (pg->flags & PG_FAKE) == 0) {
			zfs_page_copy(uobj, pg, win, off, buf, bytes,
			    B_FALSE);
			rw_exit(uobj->vmobjlock);
			error = zfs_uiomove(buf, bytes, UIO_READ, uio);
		} else {
			rw_exit(uobj->vmobjlock);
			error = dmu_read_uio_dbuf(sa_get_db(zp->z_sa_hdl),
			    uio, bytes, DMU_READ_PREFETCH);
		}
		len -= bytes;
		off = 0;
		if (error != 0)
			break;
	}
	zfs_page_window_free(win);
	kmem_free(buf, PAGE_SIZE);
	return (error);
}

/*
 * A write fault starts tracking whether the pages are dirtied, as
 * genfs_getpages() does: a page left clean is mapped read-only, to
 * catch the write, and the write would fault on it again for ever.
 */
static void
zfs_getpages_markdirty(struct vm_page **pgs, int npages)
{
	struct vm_page *pg;
	int i;

	for (i = 0; i < npages; i++) {
		pg = pgs[i];
		if (pg == NULL || pg == PGO_DONTCARE)
			continue;
		if (uvm_pagegetdirty(pg) == UVM_PAGE_STATUS_CLEAN)
			uvm_pagemarkdirty(pg, UVM_PAGE_STATUS_UNKNOWN);
	}
}

/*
 * Fill pages of a mapped file from the DMU.  With the object locked
 * only the resident pages can be had, and they are handed back without
 * being busied, as genfs_getpages() does and the fault code expects;
 * otherwise the pages are found or made busy, and the new ones read,
 * under the range lock and with the object unlocked.
 */
static int
zfs_getpages(void *v)
{
	struct vop_getpages_args *ap = v;
	vnode_t *vp = ap->a_vp;
	znode_t *zp;
	zfsvfs_t *zfsvfs;
	struct uvm_object *uobj = &vp->v_uobj;
	struct vm_page **pgs = ap->a_m, *pg;
	const bool memwrite = (ap->a_access_type & VM_PROT_WRITE) != 0;
	zfs_locked_range_t *lr;
	voff_t start = ap->a_offset, off;
	vaddr_t kva;
	int i, npages, error = 0;
	size_t len;

	/* A reclaimed vnode has no znode to read from. */
	if (__predict_false((atomic_load_relaxed(&vp->v_iflag) &
	    VI_DEADCHECK) != 0)) {
		mutex_enter(vp->v_interlock);
		error = vdead_check(vp, VDEAD_NOWAIT);
		mutex_exit(vp->v_interlock);
		if (error != 0) {
			if ((ap->a_flags & PGO_LOCKED) == 0)
				rw_exit(uobj->vmobjlock);
			return (error);
		}
	}
	zp = VTOZ(vp);
	zfsvfs = zp->z_zfsvfs;

	if (ap->a_flags & PGO_LOCKED) {
		npages = *ap->a_count;
		(void) uvn_findpages(uobj, start, &npages, pgs, NULL,
		    UFP_NOWAIT | UFP_NOALLOC | UFP_NOBUSY |
		    (memwrite ? UFP_NORDONLY : 0));
		if (pgs[ap->a_centeridx] == NULL)
			return (EBUSY);
		if (memwrite)
			zfs_getpages_markdirty(pgs, npages);
		return (0);
	}
	if (start + ((voff_t)ap->a_centeridx << PAGE_SHIFT) >= vp->v_size) {
		rw_exit(uobj->vmobjlock);
		return (SET_ERROR(EINVAL));
	}
	if ((ap->a_flags & PGO_SYNCIO) == 0) {
		/* Read-ahead is not done. */
		rw_exit(uobj->vmobjlock);
		return (0);
	}
	npages = *ap->a_count;
	uvn_findpages(uobj, start, &npages, pgs, NULL, UFP_ALL);
	rw_exit(uobj->vmobjlock);

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		goto out;
	lr = zfs_rangelock_enter(&zp->z_rangelock, start,
	    (uint64_t)npages << PAGE_SHIFT, RL_READER);
	kva = uvm_pagermapin(pgs, npages,
	    UVMPAGER_MAPIN_READ | UVMPAGER_MAPIN_WAITOK);
	for (i = 0; i < npages; i++) {
		char *va = (char *)kva + ((vaddr_t)i << PAGE_SHIFT);

		pg = pgs[i];
		if ((pg->flags & PG_FAKE) == 0)
			continue;
		off = start + ((voff_t)i << PAGE_SHIFT);
		len = off < zp->z_size ? MIN(PAGE_SIZE, zp->z_size - off) : 0;
		if (len > 0 && (error = dmu_read(zfsvfs->z_os, zp->z_id, off,
		    len, va, DMU_READ_PREFETCH)) != 0)
			break;
		if (len < PAGE_SIZE)
			memset(va + len, 0, PAGE_SIZE - len);
	}
	uvm_pagermapout(kva, npages);
	zfs_rangelock_exit(lr);
	zfs_exit(zfsvfs, FTAG);
out:
	rw_enter(uobj->vmobjlock, RW_WRITER);
	for (i = 0; i < npages; i++) {
		pg = pgs[i];
		if (error != 0 && (pg->flags & PG_FAKE) != 0) {
			pg->flags |= PG_RELEASED;
		} else {
			/* What was just read is what the DMU has. */
			if (pg->flags & PG_FAKE) {
				pg->flags &= ~PG_FAKE;
				uvm_pagemarkdirty(pg, UVM_PAGE_STATUS_CLEAN);
			}
			uvm_pagelock(pg);
			uvm_pageactivate(pg);
			uvm_pageunlock(pg);
		}
	}
	if (error != 0)
		uvm_page_unbusy(pgs, npages);
	else if (memwrite)
		zfs_getpages_markdirty(pgs, npages);
	rw_exit(uobj->vmobjlock);
	return (error);
}

/*
 * Write a mapped file's dirty pages back into the DMU, for
 * genfs_putpages(), and log the write as on FreeBSD.  The pages come
 * busy; the write is done before this returns, and finishing it the way
 * genfs_compat_gop_write() does marks them clean and lets them go.
 */
static int
zfs_gop_write(vnode_t *vp, struct vm_page **pgs, int npages, int flags)
{
	znode_t *zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	zfs_locked_range_t *lr;
	dmu_tx_t *tx;
	struct buf *bp;
	voff_t off = pgs[0]->offset;
	size_t len = (size_t)npages << PAGE_SHIFT;
	boolean_t commit;
	vaddr_t kva;
	int error;

	kva = uvm_pagermapin(pgs, npages,
	    UVMPAGER_MAPIN_WRITE | UVMPAGER_MAPIN_WAITOK);
	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		goto done;
	/*
	 * Committing the log looks the file up again (zfs_get_data()),
	 * and vcache_get() waits for a vnode being reclaimed, which is
	 * this one when vcache_reclaim() is flushing it.  Reclaiming only
	 * needs the data in a txg, so its write is not committed.
	 */
	commit = (flags & PGO_RECLAIM) == 0 &&
	    ((flags & PGO_SYNCIO) != 0 ||
	    zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS);
	lr = zfs_rangelock_enter(&zp->z_rangelock, off, len, RL_WRITER);
	/* What lies past the end of the file is not written. */
	if (off >= zp->z_size)
		len = 0;
	else if (off + len > zp->z_size)
		len = zp->z_size - off;
	if (len == 0)
		goto unlock;
	if (zfs_id_overblockquota(zfsvfs, DMU_USERUSED_OBJECT, zp->z_uid) ||
	    zfs_id_overblockquota(zfsvfs, DMU_GROUPUSED_OBJECT, zp->z_gid) ||
	    (zp->z_projid != ZFS_DEFAULT_PROJID &&
	    zfs_id_overblockquota(zfsvfs, DMU_PROJECTUSED_OBJECT,
	    zp->z_projid))) {
		error = SET_ERROR(EDQUOT);
		goto unlock;
	}
	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_write(tx, zp->z_id, off, len);
	dmu_tx_hold_sa(tx, zp->z_sa_hdl, ZFS_SEQ_MAY_GROW(zp));
	zfs_sa_upgrade_txholds(tx, zp);
	if ((error = dmu_tx_assign(tx, DMU_TX_WAIT)) != 0) {
		dmu_tx_abort(tx);
		goto unlock;
	}
	dmu_write(zfsvfs->z_os, zp->z_id, off, len, (void *)kva, tx,
	    DMU_READ_PREFETCH);
	{
		uint64_t mtime[2], ctime[2];
		sa_bulk_attr_t bulk[4];
		int count = 0;

		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MTIME(zfsvfs), NULL,
		    &mtime, 16);
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_CTIME(zfsvfs), NULL,
		    &ctime, 16);
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_FLAGS(zfsvfs), NULL,
		    &zp->z_pflags, 8);
		zfs_tstamp_update_setup(zp, CONTENT_MODIFIED, mtime, ctime);
		ZFS_PERSIST_SEQ(zp, bulk, count);
		ASSERT3S(count, <=, ARRAY_SIZE(bulk));
		VERIFY0(sa_bulk_update(zp->z_sa_hdl, bulk, count, tx));
	}
	/*
	 * A reclaim's write is not logged either: committing the record
	 * would look the file up after its vnode is gone, from an unmount
	 * past the point where it can have vnodes again.
	 */
	if ((flags & PGO_RECLAIM) == 0)
		zfs_log_write(zfsvfs->z_log, tx, TX_WRITE, zp, off, len,
		    commit, B_FALSE, NULL, NULL);
	dmu_tx_commit(tx);
	dataset_kstats_update_write_kstats(&zfsvfs->z_kstat, len);
unlock:
	zfs_rangelock_exit(lr);
	if (error == 0 && commit)
		error = zil_commit(zfsvfs->z_log, zp->z_id);
	zfs_exit(zfsvfs, FTAG);
done:
	mutex_enter(vp->v_interlock);
	vp->v_numoutput++;
	mutex_exit(vp->v_interlock);
	bp = getiobuf(vp, true);
	bp->b_cflags |= BC_BUSY | BC_AGE;
	bp->b_lblkno = off >> vp->v_mount->mnt_fs_bshift;
	bp->b_data = (void *)kva;
	bp->b_bcount = (size_t)npages << PAGE_SHIFT;
	bp->b_bufsize = (size_t)npages << PAGE_SHIFT;
	bp->b_resid = 0;
	bp->b_error = error;
	uvm_aio_aiodone(bp);
	return (error);
}

int
zfs_write_simple(znode_t *zp, const void *data, size_t len,
    loff_t pos, size_t *presid)
{
	int error = 0;
	size_t resid;

	error = vn_rdwr(UIO_WRITE, ZTOV(zp), (void *)__UNCONST(data), len, pos,
	    UIO_SYSSPACE, IO_SYNC, kcred, &resid, curlwp);

	if (error) {
		return (SET_ERROR(error));
	} else if (presid == NULL) {
		if (resid != 0) {
			error = SET_ERROR(EIO);
		}
	} else {
		*presid = resid;
	}
	return (error);
}

void
zfs_zrele_async(znode_t *zp)
{
	vnode_t *vp = ZTOV(zp);
	objset_t *os = ITOZSB(vp)->z_os;

	/*
	 * Into the pool's zrele taskq, as on FreeBSD and Linux, rather
	 * than NetBSD's vrele_async(): zfsvfs_teardown() waits for that
	 * taskq before the file system goes, and nothing waits for the
	 * other.
	 */
	VERIFY(taskq_dispatch(dsl_pool_zrele_taskq(dmu_objset_pool(os)),
	    (task_func_t *)vrele, vp, TQ_SLEEP) != TASKQID_INVALID);
}

/*
 * Look a name up in a directory.  NetBSD's VOP_LOOKUP returns the
 * vnode referenced and unlocked, whatever it is, so there is none of
 * the relocking FreeBSD's needs for "..".
 */
static int
zfs_lookup(vnode_t *dvp, const char *nm, vnode_t **vpp,
    struct componentname *cnp, int nameiop, cred_t *cr, int flags)
{
	znode_t *zdp = VTOZ(dvp);
	znode_t *zp;
	zfsvfs_t *zfsvfs = zdp->z_zfsvfs;
	int error;

	if (!(flags & LOOKUP_XATTR)) {
		if (dvp->v_type != VDIR)
			return (SET_ERROR(ENOTDIR));
		if (zdp->z_sa_hdl == NULL)
			return (SET_ERROR(EIO));
	}
	if ((error = zfs_enter_verify_zp(zfsvfs, zdp, FTAG)) != 0)
		return (error);
	*vpp = NULL;

	if (flags & LOOKUP_XATTR) {
		if (!(zfsvfs->z_flags & ZSB_XATTR)) {
			zfs_exit(zfsvfs, FTAG);
			return (SET_ERROR(EOPNOTSUPP));
		}
		/* An attribute has no attributes of its own. */
		if (zdp->z_pflags & ZFS_XATTR) {
			zfs_exit(zfsvfs, FTAG);
			return (SET_ERROR(EINVAL));
		}
		if ((error = zfs_get_xattrdir(zdp, &zp, cr, flags)) != 0) {
			zfs_exit(zfsvfs, FTAG);
			return (error);
		}
		*vpp = ZTOV(zp);
		if ((error = zfs_zaccess(zp, ACE_EXECUTE, 0, B_FALSE,
		    cr)) != 0) {
			vrele(*vpp);
			*vpp = NULL;
		}
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	if ((error = zfs_zaccess(zdp, ACE_EXECUTE, 0, B_FALSE, cr)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}
	if (zfsvfs->z_utf8 && u8_validate(nm, strlen(nm), NULL,
	    U8_VALIDATE_ENTIRE, &error) < 0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EILSEQ));
	}

	if (zfs_has_ctldir(zdp) && strcmp(nm, ZFS_CTLDIR_NAME) == 0) {
		zfs_exit(zfsvfs, FTAG);
		if ((cnp->cn_flags & ISLASTCN) && nameiop != LOOKUP)
			return (SET_ERROR(EROFS));
		error = zfsctl_root(zfsvfs, 0, vpp);
		/*
		 * getcwd(3) finds a name through the cache, or else by
		 * reading the parent, where a hidden .zfs is not.
		 */
		if (error == 0 && zfsvfs->z_use_namecache)
			cache_enter(dvp, *vpp, cnp->cn_nameptr,
			    cnp->cn_namelen, cnp->cn_flags);
		return (error);
	}

	error = zfs_dirlook(zdp, nm, &zp);
	zfs_exit(zfsvfs, FTAG);
	if (error == 0) {
		/*
		 * zfs_dirlook() hands "." back without a hold, which
		 * FreeBSD's caller adds; a lookup returns it referenced.
		 */
		if (zp == zdp)
			vref(dvp);
		*vpp = ZTOV(zp);
	}

	/*
	 * A name that is not there is what a create or a rename's target
	 * wants to find.
	 */
	if ((cnp->cn_flags & ISLASTCN) && error == ENOENT &&
	    (nameiop == CREATE || nameiop == RENAME)) {
		if (dvp->v_mount->mnt_flag & MNT_RDONLY)
			error = SET_ERROR(EROFS);
		else
			error = EJUSTRETURN;
	}

	if (zfsvfs->z_use_namecache && !zfsvfs->z_replay &&
	    (error == 0 || error == ENOENT) &&
	    !((cnp->cn_flags & ISLASTCN) &&
	    (nameiop == DELETE || nameiop == RENAME)))
		cache_enter(dvp, *vpp, cnp->cn_nameptr, cnp->cn_namelen,
		    cnp->cn_flags);
	return (error);
}

static inline bool
is_nametoolong(zfsvfs_t *zfsvfs, const char *name)
{
	size_t dlen = strlen(name);
	return ((!zfsvfs->z_longname && dlen >= ZAP_MAXNAMELEN) ||
	    dlen >= ZAP_MAXNAMELEN_NEW);
}

/*
 * Attempt to create a new entry in a directory.  If the entry
 * already exists, truncate the file if permissible, else return
 * an error.  Return the vp of the created or trunc'd file.
 *
 *	IN:	dvp	- vnode of directory to put new file entry in.
 *		name	- name of new file entry.
 *		vap	- attributes of new file.
 *		excl	- flag indicating exclusive or non-exclusive mode.
 *		mode	- mode to open file with.
 *		cr	- credentials of caller.
 *		flag	- large file flag [UNUSED].
 *		ct	- caller context
 *		vsecp	- ACL to be set
 *
 *	OUT:	vpp	- vnode of created or trunc'd entry.
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	dvp - ctime|mtime updated if new entry created
 *	 vp - ctime|mtime always, atime if new
 */
int
zfs_create(znode_t *dzp, const char *name, vattr_t *vap, int excl, int mode,
    znode_t **zpp, cred_t *cr, int flag, vsecattr_t *vsecp)
{
	(void) excl, (void) mode, (void) flag;
	znode_t		*zp;
	zfsvfs_t	*zfsvfs = dzp->z_zfsvfs;
	zilog_t		*zilog;
	objset_t	*os;
	dmu_tx_t	*tx;
	int		error;
	uid_t		uid = crgetuid(cr);
	gid_t		gid = crgetgid(cr);
	uint64_t	projid = ZFS_DEFAULT_PROJID;
	zfs_acl_ids_t   acl_ids;
	boolean_t	fuid_dirtied;
	uint64_t	txtype;

	if (is_nametoolong(zfsvfs, name))
		return (SET_ERROR(ENAMETOOLONG));

	/*
	 * If we have an ephemeral id, ACL, or XVATTR then
	 * make sure file system is at proper version
	 */
	if (zfsvfs->z_use_fuids == B_FALSE &&
	    (vsecp || (vap->va_mask & AT_XVATTR) ||
	    IS_EPHEMERAL(uid) || IS_EPHEMERAL(gid)))
		return (SET_ERROR(EINVAL));

	if ((error = zfs_enter_verify_zp(zfsvfs, dzp, FTAG)) != 0)
		return (error);
	os = zfsvfs->z_os;
	zilog = zfsvfs->z_log;

	if (zfsvfs->z_utf8 && u8_validate(name, strlen(name),
	    NULL, U8_VALIDATE_ENTIRE, &error) < 0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EILSEQ));
	}

	if (vap->va_mask & AT_XVATTR) {
		if ((error = secpolicy_xvattr(ZTOV(dzp), (xvattr_t *)vap,
		    crgetuid(cr), cr, vap->va_type)) != 0) {
			zfs_exit(zfsvfs, FTAG);
			return (error);
		}
	}

	*zpp = NULL;

	if ((vap->va_mode & S_ISVTX) && secpolicy_vnode_stky_modify(cr))
		vap->va_mode &= ~S_ISVTX;

	error = zfs_dirent_lookup(dzp, name, &zp, ZNEW);
	if (error) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}
	ASSERT0P(zp);

	/*
	 * Create a new file object and update the directory
	 * to reference it.
	 */
	if ((error = zfs_zaccess(dzp, ACE_ADD_FILE, 0, B_FALSE, cr))) {
		goto out;
	}

	/*
	 * We only support the creation of regular files in
	 * extended attribute directories.
	 */

	if ((dzp->z_pflags & ZFS_XATTR) &&
	    (vap->va_type != VREG)) {
		error = SET_ERROR(EINVAL);
		goto out;
	}

	if ((error = zfs_acl_ids_create(dzp, 0, vap,
	    cr, vsecp, &acl_ids)) != 0)
		goto out;

	projid = zfs_inherit_projid(dzp);
	if (zfs_acl_ids_overquota(zfsvfs, &acl_ids, projid)) {
		zfs_acl_ids_free(&acl_ids);
		error = SET_ERROR(EDQUOT);
		goto out;
	}

	getnewvnode_reserve();

	tx = dmu_tx_create(os);

	dmu_tx_hold_sa_create(tx, acl_ids.z_aclp->z_acl_bytes +
	    ZFS_SA_BASE_ATTR_SIZE);

	fuid_dirtied = zfsvfs->z_fuid_dirty;
	if (fuid_dirtied)
		zfs_fuid_txhold(zfsvfs, tx);
	dmu_tx_hold_zap(tx, dzp->z_id, TRUE, name);
	dmu_tx_hold_sa(tx, dzp->z_sa_hdl, ZFS_SEQ_MAY_GROW(dzp));
	if (!zfsvfs->z_use_sa &&
	    acl_ids.z_aclp->z_acl_bytes > ZFS_ACE_SPACE) {
		dmu_tx_hold_write(tx, DMU_NEW_OBJECT,
		    0, acl_ids.z_aclp->z_acl_bytes);
	}
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error) {
		zfs_acl_ids_free(&acl_ids);
		dmu_tx_abort(tx);
		getnewvnode_drop_reserve();
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}
	zfs_mknode(dzp, vap, tx, cr, 0, &zp, &acl_ids);

	error = zfs_link_create(dzp, name, zp, tx, ZNEW);
	if (error != 0) {
		/*
		 * Since, we failed to add the directory entry for it,
		 * delete the newly created dnode.
		 */
		zfs_znode_delete(zp, tx);
		VOP_UNLOCK(ZTOV(zp));
		zrele(zp);
		zfs_acl_ids_free(&acl_ids);
		dmu_tx_commit(tx);
		getnewvnode_drop_reserve();
		goto out;
	}

	if (fuid_dirtied)
		zfs_fuid_sync(zfsvfs, tx);

	txtype = zfs_log_create_txtype(Z_FILE, vsecp, vap);
	zfs_log_create(zilog, tx, txtype, dzp, zp, name,
	    vsecp, acl_ids.z_fuidp, vap);
	zfs_acl_ids_free(&acl_ids);
	dmu_tx_commit(tx);

	getnewvnode_drop_reserve();

out:
	if (error == 0) {
		*zpp = zp;
	}

	if (error == 0 && zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		error = zil_commit(zilog, 0);

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

/*
 * Remove an entry from a directory.
 *
 *	IN:	dvp	- vnode of directory to remove entry from.
 *		name	- name of entry to remove.
 *		cr	- credentials of caller.
 *		ct	- caller context
 *		flags	- case flags
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	dvp - ctime|mtime
 *	 vp - ctime (if nlink > 0)
 */
static int
zfs_remove_(vnode_t *dvp, vnode_t *vp, const char *name, cred_t *cr)
{
	znode_t		*dzp = VTOZ(dvp);
	znode_t		*zp;
	znode_t		*xzp;
	zfsvfs_t	*zfsvfs = dzp->z_zfsvfs;
	zilog_t		*zilog;
	uint64_t	xattr_obj;
	uint64_t	obj = 0;
	dmu_tx_t	*tx;
	boolean_t	unlinked;
	uint64_t	txtype;
	int		error;


	if ((error = zfs_enter_verify_zp(zfsvfs, dzp, FTAG)) != 0)
		return (error);
	zp = VTOZ(vp);
	if ((error = zfs_verify_zp(zp)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}
	zilog = zfsvfs->z_log;

	xattr_obj = 0;
	xzp = NULL;

	if ((error = zfs_zaccess_delete(dzp, zp, cr))) {
		goto out;
	}

	/*
	 * Need to use rmdir for removing directories.
	 */
	if (vp->v_type == VDIR) {
		error = SET_ERROR(EPERM);
		goto out;
	}

	vnevent_remove(vp, dvp, name, ct);

	obj = zp->z_id;

	/* are there any extended attributes? */
	error = sa_lookup(zp->z_sa_hdl, SA_ZPL_XATTR(zfsvfs),
	    &xattr_obj, sizeof (xattr_obj));
	if (error == 0 && xattr_obj) {
		error = zfs_zget(zfsvfs, xattr_obj, &xzp);
		ASSERT0(error);
	}

	/*
	 * We may delete the znode now, or we may put it in the unlinked set;
	 * it depends on whether we're the last link, and on whether there are
	 * other holds on the vnode.  So we dmu_tx_hold() the right things to
	 * allow for either case.
	 */
	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_zap(tx, dzp->z_id, FALSE, name);
	dmu_tx_hold_sa(tx, zp->z_sa_hdl, ZFS_SEQ_MAY_GROW(zp));
	dmu_tx_hold_sa(tx, dzp->z_sa_hdl, ZFS_SEQ_MAY_GROW(dzp));
	zfs_sa_upgrade_txholds(tx, zp);
	zfs_sa_upgrade_txholds(tx, dzp);

	if (xzp) {
		dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_TRUE);
		dmu_tx_hold_sa(tx, xzp->z_sa_hdl, B_FALSE);
	}

	/* charge as an update -- would be nice not to charge at all */
	dmu_tx_hold_zap(tx, zfsvfs->z_unlinkedobj, FALSE, NULL);

	/*
	 * Mark this transaction as typically resulting in a net free of space
	 */
	dmu_tx_mark_netfree(tx);

	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error) {
		dmu_tx_abort(tx);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * Remove the directory entry.
	 */
	error = zfs_link_destroy(dzp, name, zp, tx, ZEXISTS, &unlinked);

	if (error) {
		dmu_tx_commit(tx);
		goto out;
	}

	if (unlinked) {
		zfs_unlinked_add(zp, tx);
	}
	/* XXX check changes to linux vnops */
	txtype = TX_REMOVE;
	zfs_log_remove(zilog, tx, txtype, dzp, name, obj, unlinked);

	dmu_tx_commit(tx);
out:

	if (xzp)
		vrele(ZTOV(xzp));

	if (error == 0 && zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		error = zil_commit(zilog, 0);

	zfs_exit(zfsvfs, FTAG);
	return (error);
}


/*
 * Look a name up for the replay of the intent log, which removes and
 * renames by name: the vnode comes back locked exclusively.  A name
 * that is not there is EJUSTRETURN to a rename's target, as a lookup
 * through namei() would say.
 */
static int
zfs_lookup_internal(znode_t *dzp, const char *name, vnode_t **vpp,
    struct componentname *cnp, int nameiop)
{
	zfsvfs_t *zfsvfs = dzp->z_zfsvfs;
	znode_t *zp;
	int error;

	cnp->cn_nameptr = name;
	cnp->cn_namelen = strlen(name);
	cnp->cn_flags = ISLASTCN;
	cnp->cn_cred = kcred;
	if ((error = zfs_enter_verify_zp(zfsvfs, dzp, FTAG)) != 0)
		return (error);
	error = zfs_dirlook(dzp, name, &zp);
	zfs_exit(zfsvfs, FTAG);
	if (error == ENOENT && (nameiop == CREATE || nameiop == RENAME))
		return (EJUSTRETURN);
	if (error != 0)
		return (error);
	if (zp == dzp)
		zhold(zp);	/* "." comes back without one */
	*vpp = ZTOV(zp);
	vn_lock(*vpp, LK_EXCLUSIVE | LK_RETRY);
	return (0);
}

int
zfs_remove(znode_t *dzp, const char *name, cred_t *cr, int flags)
{
	vnode_t *vp;
	int error;
	struct componentname cn;

	if ((error = zfs_lookup_internal(dzp, name, &vp, &cn, DELETE)))
		return (error);

	error = zfs_remove_(ZTOV(dzp), vp, name, cr);
	vput(vp);
	return (error);
}
/*
 * Create a new directory and insert it into dvp using the name
 * provided.  Return a pointer to the inserted directory.
 *
 *	IN:	dvp	- vnode of directory to add subdir to.
 *		dirname	- name of new directory.
 *		vap	- attributes of new directory.
 *		cr	- credentials of caller.
 *		ct	- caller context
 *		flags	- case flags
 *		vsecp	- ACL to be set
 *
 *	OUT:	vpp	- vnode of created directory.
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	dvp - ctime|mtime updated
 *	 vp - ctime|mtime|atime updated
 */
int
zfs_mkdir(znode_t *dzp, const char *dirname, vattr_t *vap, znode_t **zpp,
    cred_t *cr, int flags, vsecattr_t *vsecp)
{
	(void) flags, (void) vsecp;
	znode_t		*zp;
	zfsvfs_t	*zfsvfs = dzp->z_zfsvfs;
	zilog_t		*zilog;
	uint64_t	txtype;
	dmu_tx_t	*tx;
	int		error;
	uid_t		uid = crgetuid(cr);
	gid_t		gid = crgetgid(cr);
	zfs_acl_ids_t   acl_ids;
	boolean_t	fuid_dirtied;

	ASSERT3U(vap->va_type, ==, VDIR);

	if (is_nametoolong(zfsvfs, dirname))
		return (SET_ERROR(ENAMETOOLONG));

	/*
	 * If we have an ephemeral id, ACL, or XVATTR then
	 * make sure file system is at proper version
	 */
	if (zfsvfs->z_use_fuids == B_FALSE &&
	    ((vap->va_mask & AT_XVATTR) ||
	    IS_EPHEMERAL(uid) || IS_EPHEMERAL(gid)))
		return (SET_ERROR(EINVAL));

	if ((error = zfs_enter_verify_zp(zfsvfs, dzp, FTAG)) != 0)
		return (error);
	zilog = zfsvfs->z_log;

	if (dzp->z_pflags & ZFS_XATTR) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EINVAL));
	}

	if (zfsvfs->z_utf8 && u8_validate(dirname,
	    strlen(dirname), NULL, U8_VALIDATE_ENTIRE, &error) < 0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EILSEQ));
	}

	if (vap->va_mask & AT_XVATTR) {
		if ((error = secpolicy_xvattr(ZTOV(dzp), (xvattr_t *)vap,
		    crgetuid(cr), cr, vap->va_type)) != 0) {
			zfs_exit(zfsvfs, FTAG);
			return (error);
		}
	}

	if ((error = zfs_acl_ids_create(dzp, 0, vap, cr,
	    NULL, &acl_ids)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * First make sure the new directory doesn't exist.
	 *
	 * Existence is checked first to make sure we don't return
	 * EACCES instead of EEXIST which can cause some applications
	 * to fail.
	 */
	*zpp = NULL;

	if ((error = zfs_dirent_lookup(dzp, dirname, &zp, ZNEW))) {
		zfs_acl_ids_free(&acl_ids);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}
	ASSERT0P(zp);

	if ((error = zfs_zaccess(dzp, ACE_ADD_SUBDIRECTORY, 0, B_FALSE, cr))) {
		zfs_acl_ids_free(&acl_ids);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	if (zfs_acl_ids_overquota(zfsvfs, &acl_ids, zfs_inherit_projid(dzp))) {
		zfs_acl_ids_free(&acl_ids);
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EDQUOT));
	}

	/*
	 * Add a new entry to the directory.
	 */
	getnewvnode_reserve();
	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_zap(tx, dzp->z_id, TRUE, dirname);
	dmu_tx_hold_zap(tx, DMU_NEW_OBJECT, FALSE, NULL);
	dmu_tx_hold_sa(tx, dzp->z_sa_hdl, ZFS_SEQ_MAY_GROW(dzp));
	fuid_dirtied = zfsvfs->z_fuid_dirty;
	if (fuid_dirtied)
		zfs_fuid_txhold(zfsvfs, tx);
	if (!zfsvfs->z_use_sa && acl_ids.z_aclp->z_acl_bytes > ZFS_ACE_SPACE) {
		dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0,
		    acl_ids.z_aclp->z_acl_bytes);
	}

	dmu_tx_hold_sa_create(tx, acl_ids.z_aclp->z_acl_bytes +
	    ZFS_SA_BASE_ATTR_SIZE);

	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error) {
		zfs_acl_ids_free(&acl_ids);
		dmu_tx_abort(tx);
		getnewvnode_drop_reserve();
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * Create new node.
	 */
	zfs_mknode(dzp, vap, tx, cr, 0, &zp, &acl_ids);

	/*
	 * Now put new name in parent dir.
	 */
	error = zfs_link_create(dzp, dirname, zp, tx, ZNEW);
	if (error != 0) {
		zfs_znode_delete(zp, tx);
		VOP_UNLOCK(ZTOV(zp));
		zrele(zp);
		goto out;
	}

	if (fuid_dirtied)
		zfs_fuid_sync(zfsvfs, tx);

	*zpp = zp;

	txtype = zfs_log_create_txtype(Z_DIR, NULL, vap);
	zfs_log_create(zilog, tx, txtype, dzp, zp, dirname, NULL,
	    acl_ids.z_fuidp, vap);

out:
	zfs_acl_ids_free(&acl_ids);

	dmu_tx_commit(tx);

	getnewvnode_drop_reserve();

	if (error == 0 && zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		error = zil_commit(zilog, 0);

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

/*
 * Remove a directory subdir entry.  If the current working
 * directory is the same as the subdir to be removed, the
 * remove will fail.
 *
 *	IN:	dvp	- vnode of directory to remove from.
 *		name	- name of directory to be removed.
 *		cwd	- vnode of current working directory.
 *		cr	- credentials of caller.
 *		ct	- caller context
 *		flags	- case flags
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	dvp - ctime|mtime updated
 */
static int
zfs_rmdir_(vnode_t *dvp, vnode_t *vp, const char *name, cred_t *cr)
{
	znode_t		*dzp = VTOZ(dvp);
	znode_t		*zp = VTOZ(vp);
	zfsvfs_t	*zfsvfs = dzp->z_zfsvfs;
	zilog_t		*zilog;
	dmu_tx_t	*tx;
	int		error;

	if ((error = zfs_enter_verify_zp(zfsvfs, dzp, FTAG)) != 0)
		return (error);
	if ((error = zfs_verify_zp(zp)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}
	zilog = zfsvfs->z_log;


	if ((error = zfs_zaccess_delete(dzp, zp, cr))) {
		goto out;
	}

	if (vp->v_type != VDIR) {
		error = SET_ERROR(ENOTDIR);
		goto out;
	}

	vnevent_rmdir(vp, dvp, name, ct);

	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_zap(tx, dzp->z_id, FALSE, name);
	dmu_tx_hold_sa(tx, zp->z_sa_hdl, ZFS_SEQ_MAY_GROW(zp));
	dmu_tx_hold_sa(tx, dzp->z_sa_hdl, ZFS_SEQ_MAY_GROW(dzp));
	dmu_tx_hold_zap(tx, zfsvfs->z_unlinkedobj, FALSE, NULL);
	zfs_sa_upgrade_txholds(tx, zp);
	zfs_sa_upgrade_txholds(tx, dzp);
	dmu_tx_mark_netfree(tx);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error) {
		dmu_tx_abort(tx);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	error = zfs_link_destroy(dzp, name, zp, tx, ZEXISTS, NULL);

	if (error == 0) {
		uint64_t txtype = TX_RMDIR;
		zfs_log_remove(zilog, tx, txtype, dzp, name,
		    ZFS_NO_OBJECT, B_FALSE);
	}

	dmu_tx_commit(tx);

	if (zfsvfs->z_use_namecache) {
		cache_purge(dvp);
		cache_purge(vp);
	}
out:
	if (error == 0 && zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		error = zil_commit(zilog, 0);

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

int
zfs_rmdir(znode_t *dzp, const char *name, znode_t *cwd, cred_t *cr, int flags)
{
	struct componentname cn;
	vnode_t *vp;
	int error;

	if ((error = zfs_lookup_internal(dzp, name, &vp, &cn, DELETE)))
		return (error);

	error = zfs_rmdir_(ZTOV(dzp), vp, name, cr);
	vput(vp);
	return (error);
}

/*
 * Read the entries of a directory, as struct dirent, with the cookie of
 * each -- the offset to read from after it -- for NFS.
 *
 * The offset is 0 for ".", 1 for "..", 2 for ".zfs" when it is shown,
 * and past that a serialized ZAP cursor, whose low bits are never all
 * clear and so never one of those.
 */
static int
zfs_readdir(vnode_t *vp, zfs_uio_t *uio, cred_t *cr, int *eofp,
    off_t **cookies, int *ncookies)
{
	znode_t *zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	objset_t *os;
	struct dirent *dp;
	zap_cursor_t zc;
	zap_attribute_t *zap;
	uint64_t offset;	/* unsigned; checked for < 3 */
	uint64_t parent;
	off_t *cooks = NULL;
	ssize_t orig_resid;
	int ncooks = 0, maxcooks = 0, local_eof, error;
	uint8_t prefetch, type;

	(void) cr;
	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);
	if ((error = sa_lookup(zp->z_sa_hdl, SA_ZPL_PARENT(zfsvfs), &parent,
	    sizeof (parent))) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}
	if (eofp == NULL)
		eofp = &local_eof;
	/* A directory that has been removed reads as empty. */
	if ((*eofp = (zp->z_unlinked != 0)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (0);
	}

	os = zfsvfs->z_os;
	offset = zfs_uio_offset(uio);
	orig_resid = zfs_uio_resid(uio);
	prefetch = zp->z_zn_prefetch;
	zap = zap_attribute_long_alloc();
	if (offset <= 3)
		zap_cursor_init(&zc, os, zp->z_id);
	else
		zap_cursor_init_serialized(&zc, os, zp->z_id, offset);

	dp = kmem_zalloc(sizeof (*dp), KM_SLEEP);
	if (cookies != NULL) {
		maxcooks = orig_resid / _DIRENT_MINSIZE(dp) + 1;
		cooks = malloc(maxcooks * sizeof (off_t), M_TEMP, M_WAITOK);
	}

	for (;;) {
		uint64_t objnum;

		if (offset == 0) {
			(void) strcpy(zap->za_name, ".");
			objnum = zp->z_id;
			type = DT_DIR;
		} else if (offset == 1) {
			(void) strcpy(zap->za_name, "..");
			objnum = parent;
			type = DT_DIR;
		} else if (offset == 2 && zfs_show_ctldir(zp)) {
			(void) strcpy(zap->za_name, ZFS_CTLDIR_NAME);
			objnum = ZFSCTL_INO_ROOT;
			type = DT_DIR;
		} else {
			if ((error = zap_cursor_retrieve(&zc, zap)) != 0) {
				if ((*eofp = (error == ENOENT)) != 0)
					error = 0;
				break;
			}
			if (zap->za_integer_length != 8 ||
			    zap->za_num_integers != 1) {
				cmn_err(CE_WARN, "zap_readdir: bad directory "
				    "entry, obj = %lld, offset = %lld\n",
				    (u_longlong_t)zp->z_id,
				    (u_longlong_t)offset);
				error = SET_ERROR(ENXIO);
				break;
			}
			objnum = ZFS_DIRENT_OBJ(zap->za_first_integer);
			type = ZFS_DIRENT_TYPE(zap->za_first_integer);
		}

		/* A name longer than a dirent holds is skipped. */
		if (strlen(zap->za_name) <= MAXNAMLEN) {
			dp->d_fileno = objnum;
			dp->d_type = type;
			dp->d_namlen = strlen(zap->za_name);
			(void) strlcpy(dp->d_name, zap->za_name,
			    sizeof (dp->d_name));
			dp->d_reclen = _DIRENT_SIZE(dp);
			if (dp->d_reclen > zfs_uio_resid(uio)) {
				if (zfs_uio_resid(uio) == orig_resid)
					error = SET_ERROR(EINVAL);
				break;
			}
			if ((error = zfs_uiomove(dp, dp->d_reclen, UIO_READ,
			    uio)) != 0)
				break;
		}
		if (prefetch)
			dmu_prefetch_dnode(os, objnum,
			    ZIO_PRIORITY_SYNC_READ);

		if (offset > 2 || (offset == 2 && !zfs_show_ctldir(zp))) {
			zap_cursor_advance(&zc);
			offset = zap_cursor_serialize(&zc);
		} else {
			offset += 1;
		}
		if (cooks != NULL && ncooks < maxcooks)
			cooks[ncooks++] = offset;
	}
	zp->z_zn_prefetch = B_FALSE;	/* a lookup turns it on again */

	zap_cursor_fini(&zc);
	zap_attribute_free(zap);
	kmem_free(dp, sizeof (*dp));
	ZFS_ACCESSTIME_STAMP(zfsvfs, zp);
	zfs_uio_setoffset(uio, offset);
	zfs_exit(zfsvfs, FTAG);

	if (cooks != NULL) {
		if (error != 0 || ncooks == 0) {
			free(cooks, M_TEMP);
			*cookies = NULL;
			*ncookies = 0;
		} else {
			*cookies = cooks;
			*ncookies = ncooks;
		}
	}
	return (error);
}

/*
 * Get the requested file attributes and place them in the provided
 * vattr structure.
 *
 *	IN:	vp	- vnode of file.
 *		vap	- va_mask identifies requested attributes.
 *			  If AT_XVATTR set, then optional attrs are requested
 *		flags	- ATTR_NOACLCHECK (CIFS server context)
 *		cr	- credentials of caller.
 *
 *	OUT:	vap	- attribute values.
 *
 *	RETURN:	0 (always succeeds).
 */
static int
zfs_getattr(vnode_t *vp, vattr_t *vap, int flags, cred_t *cr)
{
	znode_t *zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	int	error = 0;
	uint32_t blksize;
	u_longlong_t nblocks;
	uint64_t mtime[2], ctime[2], crtime[2], rdev;
	xvattr_t *xvap = (xvattr_t *)vap;	/* vap may be an xvattr_t * */
	xoptattr_t *xoap = NULL;
	boolean_t skipaclchk = (flags & ATTR_NOACLCHECK) ? B_TRUE : B_FALSE;
	sa_bulk_attr_t bulk[4];
	int count = 0;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	zfs_fuid_map_ids(zp, cr, &vap->va_uid, &vap->va_gid);

	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MTIME(zfsvfs), NULL, &mtime, 16);
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_CTIME(zfsvfs), NULL, &ctime, 16);
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_CRTIME(zfsvfs), NULL, &crtime, 16);
	if (vp->v_type == VBLK || vp->v_type == VCHR)
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_RDEV(zfsvfs), NULL,
		    &rdev, 8);

	if ((error = sa_bulk_lookup(zp->z_sa_hdl, bulk, count)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * If ACL is trivial don't bother looking for ACE_READ_ATTRIBUTES.
	 * Also, if we are the owner don't bother, since owner should
	 * always be allowed to read basic attributes of file.
	 */
	if (!(zp->z_pflags & ZFS_ACL_TRIVIAL) &&
	    (vap->va_uid != crgetuid(cr))) {
		if ((error = zfs_zaccess(zp, ACE_READ_ATTRIBUTES, 0,
		    skipaclchk, cr))) {
			zfs_exit(zfsvfs, FTAG);
			return (error);
		}
	}

	/*
	 * Return all attributes.  It's cheaper to provide the answer
	 * than to determine whether we were asked the question.
	 */

	vap->va_type = IFTOVT(zp->z_mode);
	vap->va_mode = zp->z_mode & ~S_IFMT;
	vap->va_fsid = vp->v_mount->mnt_stat.f_fsidx.__fsid_val[0];
	vap->va_nodeid = zp->z_id;
	vap->va_nlink = zp->z_links;
	if ((vp->v_flag & VROOT) && zfs_show_ctldir(zp) &&
	    zp->z_links < ZFS_LINK_MAX)
		vap->va_nlink++;
	vap->va_size = zp->z_size;
	if (vp->v_type == VBLK || vp->v_type == VCHR)
		vap->va_rdev = zfs_cmpldev(rdev);
	else
		vap->va_rdev = NODEV;
	vap->va_gen = zp->z_gen;
	vap->va_flags = 0;	/* the VOP sets chflags(2) flags */
	vap->va_filerev = atomic_load_64(&zp->z_seq);

	/*
	 * Add in any requested optional attributes and the create time.
	 * Also set the corresponding bits in the returned attribute bitmap.
	 */
	if ((xoap = xva_getxoptattr(xvap)) != NULL && zfsvfs->z_use_fuids) {
		if (XVA_ISSET_REQ(xvap, XAT_ARCHIVE)) {
			xoap->xoa_archive =
			    ((zp->z_pflags & ZFS_ARCHIVE) != 0);
			XVA_SET_RTN(xvap, XAT_ARCHIVE);
		}

		if (XVA_ISSET_REQ(xvap, XAT_READONLY)) {
			xoap->xoa_readonly =
			    ((zp->z_pflags & ZFS_READONLY) != 0);
			XVA_SET_RTN(xvap, XAT_READONLY);
		}

		if (XVA_ISSET_REQ(xvap, XAT_SYSTEM)) {
			xoap->xoa_system =
			    ((zp->z_pflags & ZFS_SYSTEM) != 0);
			XVA_SET_RTN(xvap, XAT_SYSTEM);
		}

		if (XVA_ISSET_REQ(xvap, XAT_HIDDEN)) {
			xoap->xoa_hidden =
			    ((zp->z_pflags & ZFS_HIDDEN) != 0);
			XVA_SET_RTN(xvap, XAT_HIDDEN);
		}

		if (XVA_ISSET_REQ(xvap, XAT_NOUNLINK)) {
			xoap->xoa_nounlink =
			    ((zp->z_pflags & ZFS_NOUNLINK) != 0);
			XVA_SET_RTN(xvap, XAT_NOUNLINK);
		}

		if (XVA_ISSET_REQ(xvap, XAT_IMMUTABLE)) {
			xoap->xoa_immutable =
			    ((zp->z_pflags & ZFS_IMMUTABLE) != 0);
			XVA_SET_RTN(xvap, XAT_IMMUTABLE);
		}

		if (XVA_ISSET_REQ(xvap, XAT_APPENDONLY)) {
			xoap->xoa_appendonly =
			    ((zp->z_pflags & ZFS_APPENDONLY) != 0);
			XVA_SET_RTN(xvap, XAT_APPENDONLY);
		}

		if (XVA_ISSET_REQ(xvap, XAT_NODUMP)) {
			xoap->xoa_nodump =
			    ((zp->z_pflags & ZFS_NODUMP) != 0);
			XVA_SET_RTN(xvap, XAT_NODUMP);
		}

		if (XVA_ISSET_REQ(xvap, XAT_OPAQUE)) {
			xoap->xoa_opaque =
			    ((zp->z_pflags & ZFS_OPAQUE) != 0);
			XVA_SET_RTN(xvap, XAT_OPAQUE);
		}

		if (XVA_ISSET_REQ(xvap, XAT_AV_QUARANTINED)) {
			xoap->xoa_av_quarantined =
			    ((zp->z_pflags & ZFS_AV_QUARANTINED) != 0);
			XVA_SET_RTN(xvap, XAT_AV_QUARANTINED);
		}

		if (XVA_ISSET_REQ(xvap, XAT_AV_MODIFIED)) {
			xoap->xoa_av_modified =
			    ((zp->z_pflags & ZFS_AV_MODIFIED) != 0);
			XVA_SET_RTN(xvap, XAT_AV_MODIFIED);
		}

		if (XVA_ISSET_REQ(xvap, XAT_AV_SCANSTAMP) &&
		    vp->v_type == VREG) {
			zfs_sa_get_scanstamp(zp, xvap);
		}

		if (XVA_ISSET_REQ(xvap, XAT_REPARSE)) {
			xoap->xoa_reparse = ((zp->z_pflags & ZFS_REPARSE) != 0);
			XVA_SET_RTN(xvap, XAT_REPARSE);
		}
		if (XVA_ISSET_REQ(xvap, XAT_GEN)) {
			xoap->xoa_generation = zp->z_gen;
			XVA_SET_RTN(xvap, XAT_GEN);
		}

		if (XVA_ISSET_REQ(xvap, XAT_OFFLINE)) {
			xoap->xoa_offline =
			    ((zp->z_pflags & ZFS_OFFLINE) != 0);
			XVA_SET_RTN(xvap, XAT_OFFLINE);
		}

		if (XVA_ISSET_REQ(xvap, XAT_SPARSE)) {
			xoap->xoa_sparse =
			    ((zp->z_pflags & ZFS_SPARSE) != 0);
			XVA_SET_RTN(xvap, XAT_SPARSE);
		}

		if (XVA_ISSET_REQ(xvap, XAT_PROJINHERIT)) {
			xoap->xoa_projinherit =
			    ((zp->z_pflags & ZFS_PROJINHERIT) != 0);
			XVA_SET_RTN(xvap, XAT_PROJINHERIT);
		}

		if (XVA_ISSET_REQ(xvap, XAT_PROJID)) {
			xoap->xoa_projid = zp->z_projid;
			XVA_SET_RTN(xvap, XAT_PROJID);
		}
	}

	ZFS_TIME_DECODE(&vap->va_atime, zp->z_atime);
	ZFS_TIME_DECODE(&vap->va_mtime, mtime);
	ZFS_TIME_DECODE(&vap->va_ctime, ctime);
	ZFS_TIME_DECODE(&vap->va_birthtime, crtime);


	sa_object_size(zp->z_sa_hdl, &blksize, &nblocks);
	vap->va_blksize = blksize;
	vap->va_bytes = nblocks << 9;	/* nblocks * 512 */

	if (zp->z_blksz == 0) {
		/*
		 * Block size hasn't been set; suggest maximal I/O transfers.
		 */
		vap->va_blksize = zfsvfs->z_max_blksz;
	}

	zfs_exit(zfsvfs, FTAG);
	return (0);
}

/*
 * For the operation of changing file's user/group/project, we need to
 * handle not only the main object that is assigned to the file directly,
 * but also the ones that are used by the file via hidden xattr directory.
 *
 * Because the xattr directory may contains many EA entries, as to it may
 * be impossible to change all of them via the transaction of changing the
 * main object's user/group/project attributes. Then we have to change them
 * via other multiple independent transactions one by one. It may be not good
 * solution, but we have no better idea yet.
 */
static int
zfs_setattr_dir(znode_t *dzp)
{
	zfsvfs_t	*zfsvfs = dzp->z_zfsvfs;
	objset_t	*os = zfsvfs->z_os;
	zap_cursor_t	zc;
	zap_attribute_t	*zap;
	znode_t		*zp = NULL;
	dmu_tx_t	*tx = NULL;
	uint64_t	uid, gid;
	sa_bulk_attr_t	bulk[4];
	int		count;
	int		err;

	zap = zap_attribute_alloc();
	zap_cursor_init(&zc, os, dzp->z_id);
	while ((err = zap_cursor_retrieve(&zc, zap)) == 0) {
		count = 0;
		if (zap->za_integer_length != 8 || zap->za_num_integers != 1) {
			err = ENXIO;
			break;
		}

		err = zfs_dirent_lookup(dzp, zap->za_name, &zp, ZEXISTS);
		if (err == ENOENT)
			goto next;
		if (err)
			break;

		if (zp->z_uid == dzp->z_uid &&
		    zp->z_gid == dzp->z_gid &&
		    zp->z_projid == dzp->z_projid)
			goto next;

		tx = dmu_tx_create(os);
		if (!(zp->z_pflags & ZFS_PROJID))
			dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_TRUE);
		else
			dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);

		err = dmu_tx_assign(tx, DMU_TX_WAIT);
		if (err)
			break;

		mutex_enter(&dzp->z_lock);

		if (zp->z_uid != dzp->z_uid) {
			uid = dzp->z_uid;
			SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_UID(zfsvfs), NULL,
			    &uid, sizeof (uid));
			zp->z_uid = uid;
		}

		if (zp->z_gid != dzp->z_gid) {
			gid = dzp->z_gid;
			SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_GID(zfsvfs), NULL,
			    &gid, sizeof (gid));
			zp->z_gid = gid;
		}

		uint64_t projid = dzp->z_projid;
		if (zp->z_projid != projid) {
			if (!(zp->z_pflags & ZFS_PROJID)) {
				err = sa_add_projid(zp->z_sa_hdl, tx, projid);
				if (unlikely(err == EEXIST)) {
					err = 0;
				} else if (err != 0) {
					goto sa_add_projid_err;
				} else {
					projid = ZFS_INVALID_PROJID;
				}
			}

			if (projid != ZFS_INVALID_PROJID) {
				zp->z_projid = projid;
				SA_ADD_BULK_ATTR(bulk, count,
				    SA_ZPL_PROJID(zfsvfs), NULL, &zp->z_projid,
				    sizeof (zp->z_projid));
			}
		}

sa_add_projid_err:
		mutex_exit(&dzp->z_lock);

		if (likely(count > 0)) {
			err = sa_bulk_update(zp->z_sa_hdl, bulk, count, tx);
			dmu_tx_commit(tx);
		} else if (projid == ZFS_INVALID_PROJID) {
			dmu_tx_commit(tx);
		} else {
			dmu_tx_abort(tx);
		}
		tx = NULL;
		if (err != 0 && err != ENOENT)
			break;

next:
		if (zp) {
			zrele(zp);
			zp = NULL;
		}
		zap_cursor_advance(&zc);
	}

	if (tx)
		dmu_tx_abort(tx);
	if (zp) {
		zrele(zp);
	}
	zap_cursor_fini(&zc);
	zap_attribute_free(zap);

	return (err == ENOENT ? 0 : err);
}

/*
 * Set the file attributes to the values contained in the
 * vattr structure.
 *
 *	IN:	zp	- znode of file to be modified.
 *		vap	- new attribute values.
 *			  If AT_XVATTR set, then optional attrs are being set
 *		flags	- ATTR_UTIME set if non-default time values provided.
 *			- ATTR_NOACLCHECK (CIFS context only).
 *		cr	- credentials of caller.
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	vp - ctime updated, mtime updated if size changed.
 */
int
zfs_setattr(znode_t *zp, vattr_t *vap, int flags, cred_t *cr)
{
	vnode_t		*vp = ZTOV(zp);
	zfsvfs_t	*zfsvfs = zp->z_zfsvfs;
	objset_t	*os;
	zilog_t		*zilog;
	dmu_tx_t	*tx;
	vattr_t		oldva;
	xvattr_t	tmpxvattr;
	uint_t		mask = vap->va_mask;
	uint_t		saved_mask = 0;
	uint64_t	saved_mode;
	int		trim_mask = 0;
	uint64_t	new_mode;
	uint64_t	new_uid, new_gid;
	uint64_t	xattr_obj;
	uint64_t	mtime[2], ctime[2];
	uint64_t	projid = ZFS_INVALID_PROJID;
	znode_t		*attrzp;
	int		need_policy = FALSE;
	int		err, err2;
	zfs_fuid_info_t *fuidp = NULL;
	xvattr_t *xvap = (xvattr_t *)vap;	/* vap may be an xvattr_t * */
	xoptattr_t	*xoap;
	zfs_acl_t	*aclp;
	boolean_t skipaclchk = (flags & ATTR_NOACLCHECK) ? B_TRUE : B_FALSE;
	boolean_t	fuid_dirtied = B_FALSE;
	boolean_t	handle_eadir = B_FALSE;
	sa_bulk_attr_t	bulk[9], xattr_bulk[6];
	int		count = 0, xattr_count = 0;

	if (mask == 0)
		return (0);

	if (mask & AT_NOSET)
		return (SET_ERROR(EINVAL));

	if ((err = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (err);

	os = zfsvfs->z_os;
	zilog = zfsvfs->z_log;

	/*
	 * Make sure that if we have ephemeral uid/gid or xvattr specified
	 * that file system is at proper version level
	 */

	if (zfsvfs->z_use_fuids == B_FALSE &&
	    (((mask & AT_UID) && IS_EPHEMERAL(vap->va_uid)) ||
	    ((mask & AT_GID) && IS_EPHEMERAL(vap->va_gid)) ||
	    (mask & AT_XVATTR))) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EINVAL));
	}

	if (mask & AT_SIZE && vp->v_type == VDIR) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EISDIR));
	}

	if (mask & AT_SIZE && vp->v_type != VREG && vp->v_type != VFIFO) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EINVAL));
	}

	/*
	 * If this is an xvattr_t, then get a pointer to the structure of
	 * optional attributes.  If this is NULL, then we have a vattr_t.
	 */
	xoap = xva_getxoptattr(xvap);

	xva_init(&tmpxvattr);

	/*
	 * Immutable files can only alter immutable bit and atime
	 */
	if ((zp->z_pflags & ZFS_IMMUTABLE) &&
	    ((mask & (AT_SIZE|AT_UID|AT_GID|AT_MTIME|AT_MODE)) ||
	    ((mask & AT_XVATTR) && XVA_ISSET_REQ(xvap, XAT_CREATETIME)))) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EPERM));
	}

	/*
	 * Note: ZFS_READONLY is handled in zfs_zaccess_common.
	 */

	/*
	 * Verify timestamps doesn't overflow 32 bits.
	 * ZFS can handle large timestamps, but 32bit syscalls can't
	 * handle times greater than 2039.  This check should be removed
	 * once large timestamps are fully supported.
	 */
	if (mask & (AT_ATIME | AT_MTIME)) {
		if (((mask & AT_ATIME) && TIMESPEC_OVERFLOW(&vap->va_atime)) ||
		    ((mask & AT_MTIME) && TIMESPEC_OVERFLOW(&vap->va_mtime))) {
			zfs_exit(zfsvfs, FTAG);
			return (SET_ERROR(EOVERFLOW));
		}
	}
	if (xoap != NULL && (mask & AT_XVATTR)) {
		if (XVA_ISSET_REQ(xvap, XAT_CREATETIME) &&
		    TIMESPEC_OVERFLOW(&vap->va_birthtime)) {
			zfs_exit(zfsvfs, FTAG);
			return (SET_ERROR(EOVERFLOW));
		}

		if (XVA_ISSET_REQ(xvap, XAT_PROJID)) {
			if (!dmu_objset_projectquota_enabled(os) ||
			    (!S_ISREG(zp->z_mode) && !S_ISDIR(zp->z_mode))) {
				zfs_exit(zfsvfs, FTAG);
				return (SET_ERROR(EOPNOTSUPP));
			}

			projid = xoap->xoa_projid;
			if (unlikely(projid == ZFS_INVALID_PROJID)) {
				zfs_exit(zfsvfs, FTAG);
				return (SET_ERROR(EINVAL));
			}

			if (projid == zp->z_projid && zp->z_pflags & ZFS_PROJID)
				projid = ZFS_INVALID_PROJID;
			else
				need_policy = TRUE;
		}

		if (XVA_ISSET_REQ(xvap, XAT_PROJINHERIT) &&
		    (xoap->xoa_projinherit !=
		    ((zp->z_pflags & ZFS_PROJINHERIT) != 0)) &&
		    (!dmu_objset_projectquota_enabled(os) ||
		    (!S_ISREG(zp->z_mode) && !S_ISDIR(zp->z_mode)))) {
			zfs_exit(zfsvfs, FTAG);
			return (SET_ERROR(EOPNOTSUPP));
		}
	}

	attrzp = NULL;
	aclp = NULL;

	if (zfsvfs->z_vfs->vfs_flag & VFS_RDONLY) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EROFS));
	}

	/*
	 * First validate permissions
	 */

	if (mask & AT_SIZE) {
		/*
		 * XXX - Note, we are not providing any open
		 * mode flags here (like FNDELAY), so we may
		 * block if there are locks present... this
		 * should be addressed in openat().
		 */
		/* XXX - would it be OK to generate a log record here? */
		err = zfs_freesp(zp, vap->va_size, 0, 0, FALSE);
		if (err) {
			zfs_exit(zfsvfs, FTAG);
			return (err);
		}
	}

	if (mask & (AT_ATIME|AT_MTIME) ||
	    ((mask & AT_XVATTR) && (XVA_ISSET_REQ(xvap, XAT_HIDDEN) ||
	    XVA_ISSET_REQ(xvap, XAT_READONLY) ||
	    XVA_ISSET_REQ(xvap, XAT_ARCHIVE) ||
	    XVA_ISSET_REQ(xvap, XAT_OFFLINE) ||
	    XVA_ISSET_REQ(xvap, XAT_SPARSE) ||
	    XVA_ISSET_REQ(xvap, XAT_CREATETIME) ||
	    XVA_ISSET_REQ(xvap, XAT_SYSTEM)))) {
		need_policy = zfs_zaccess(zp, ACE_WRITE_ATTRIBUTES, 0,
		    skipaclchk, cr);
	}

	if (mask & (AT_UID|AT_GID)) {
		int	idmask = (mask & (AT_UID|AT_GID));
		int	take_owner;
		int	take_group;

		/*
		 * NOTE: even if a new mode is being set,
		 * we may clear S_ISUID/S_ISGID bits.
		 */

		if (!(mask & AT_MODE))
			vap->va_mode = zp->z_mode;

		/*
		 * Take ownership or chgrp to group we are a member of
		 */

		take_owner = (mask & AT_UID) && (vap->va_uid == crgetuid(cr));
		take_group = (mask & AT_GID) &&
		    zfs_groupmember(zfsvfs, vap->va_gid, cr);

		/*
		 * If both AT_UID and AT_GID are set then take_owner and
		 * take_group must both be set in order to allow taking
		 * ownership.
		 *
		 * Otherwise, send the check through secpolicy_vnode_setattr()
		 *
		 */

		if (((idmask == (AT_UID|AT_GID)) && take_owner && take_group) ||
		    ((idmask == AT_UID) && take_owner) ||
		    ((idmask == AT_GID) && take_group)) {
			if (zfs_zaccess(zp, ACE_WRITE_OWNER, 0,
			    skipaclchk, cr) == 0) {
				/*
				 * Remove setuid/setgid for non-privileged users
				 */
				secpolicy_setid_clear(vap, vp, cr);
				trim_mask = (mask & (AT_UID|AT_GID));
			} else {
				need_policy =  TRUE;
			}
		} else {
			need_policy =  TRUE;
		}
	}

	oldva.va_mode = zp->z_mode;
	zfs_fuid_map_ids(zp, cr, &oldva.va_uid, &oldva.va_gid);
	if (mask & AT_XVATTR) {
		/*
		 * Update xvattr mask to include only those attributes
		 * that are actually changing.
		 *
		 * the bits will be restored prior to actually setting
		 * the attributes so the caller thinks they were set.
		 */
		if (XVA_ISSET_REQ(xvap, XAT_APPENDONLY)) {
			if (xoap->xoa_appendonly !=
			    ((zp->z_pflags & ZFS_APPENDONLY) != 0)) {
				need_policy = TRUE;
			} else {
				XVA_CLR_REQ(xvap, XAT_APPENDONLY);
				XVA_SET_REQ(&tmpxvattr, XAT_APPENDONLY);
			}
		}

		if (XVA_ISSET_REQ(xvap, XAT_PROJINHERIT)) {
			if (xoap->xoa_projinherit !=
			    ((zp->z_pflags & ZFS_PROJINHERIT) != 0)) {
				need_policy = TRUE;
			} else {
				XVA_CLR_REQ(xvap, XAT_PROJINHERIT);
				XVA_SET_REQ(&tmpxvattr, XAT_PROJINHERIT);
			}
		}

		if (XVA_ISSET_REQ(xvap, XAT_NOUNLINK)) {
			if (xoap->xoa_nounlink !=
			    ((zp->z_pflags & ZFS_NOUNLINK) != 0)) {
				need_policy = TRUE;
			} else {
				XVA_CLR_REQ(xvap, XAT_NOUNLINK);
				XVA_SET_REQ(&tmpxvattr, XAT_NOUNLINK);
			}
		}

		if (XVA_ISSET_REQ(xvap, XAT_IMMUTABLE)) {
			if (xoap->xoa_immutable !=
			    ((zp->z_pflags & ZFS_IMMUTABLE) != 0)) {
				need_policy = TRUE;
			} else {
				XVA_CLR_REQ(xvap, XAT_IMMUTABLE);
				XVA_SET_REQ(&tmpxvattr, XAT_IMMUTABLE);
			}
		}

		if (XVA_ISSET_REQ(xvap, XAT_NODUMP)) {
			if (xoap->xoa_nodump !=
			    ((zp->z_pflags & ZFS_NODUMP) != 0)) {
				need_policy = TRUE;
			} else {
				XVA_CLR_REQ(xvap, XAT_NODUMP);
				XVA_SET_REQ(&tmpxvattr, XAT_NODUMP);
			}
		}

		if (XVA_ISSET_REQ(xvap, XAT_AV_MODIFIED)) {
			if (xoap->xoa_av_modified !=
			    ((zp->z_pflags & ZFS_AV_MODIFIED) != 0)) {
				need_policy = TRUE;
			} else {
				XVA_CLR_REQ(xvap, XAT_AV_MODIFIED);
				XVA_SET_REQ(&tmpxvattr, XAT_AV_MODIFIED);
			}
		}

		if (XVA_ISSET_REQ(xvap, XAT_AV_QUARANTINED)) {
			if ((vp->v_type != VREG &&
			    xoap->xoa_av_quarantined) ||
			    xoap->xoa_av_quarantined !=
			    ((zp->z_pflags & ZFS_AV_QUARANTINED) != 0)) {
				need_policy = TRUE;
			} else {
				XVA_CLR_REQ(xvap, XAT_AV_QUARANTINED);
				XVA_SET_REQ(&tmpxvattr, XAT_AV_QUARANTINED);
			}
		}

		if (XVA_ISSET_REQ(xvap, XAT_REPARSE)) {
			zfs_exit(zfsvfs, FTAG);
			return (SET_ERROR(EPERM));
		}

		if (need_policy == FALSE &&
		    (XVA_ISSET_REQ(xvap, XAT_AV_SCANSTAMP) ||
		    XVA_ISSET_REQ(xvap, XAT_OPAQUE))) {
			need_policy = TRUE;
		}
	}

	if (mask & AT_MODE) {
		if (zfs_zaccess(zp, ACE_WRITE_ACL, 0, skipaclchk, cr) == 0) {
			err = secpolicy_setid_setsticky_clear(vp, vap,
			    &oldva, cr);
			if (err) {
				zfs_exit(zfsvfs, FTAG);
				return (err);
			}
			trim_mask |= AT_MODE;
		} else {
			need_policy = TRUE;
		}
	}

	if (need_policy) {
		/*
		 * If trim_mask is set then take ownership
		 * has been granted or write_acl is present and user
		 * has the ability to modify mode.  In that case remove
		 * UID|GID and or MODE from mask so that
		 * secpolicy_vnode_setattr() doesn't revoke it.
		 */

		if (trim_mask) {
			saved_mask = vap->va_mask;
			vap->va_mask &= ~trim_mask;
			if (trim_mask & AT_MODE) {
				/*
				 * Save the mode, as secpolicy_vnode_setattr()
				 * will overwrite it with ova.va_mode.
				 */
				saved_mode = vap->va_mode;
			}
		}
		err = secpolicy_vnode_setattr(cr, vp, vap, &oldva, flags,
		    (int (*)(void *, int, cred_t *))zfs_zaccess_unix, zp);
		if (err) {
			zfs_exit(zfsvfs, FTAG);
			return (err);
		}

		if (trim_mask) {
			vap->va_mask |= saved_mask;
			if (trim_mask & AT_MODE) {
				/*
				 * Recover the mode after
				 * secpolicy_vnode_setattr().
				 */
				vap->va_mode = saved_mode;
			}
		}
	}

	/*
	 * secpolicy_vnode_setattr, or take ownership may have
	 * changed va_mask
	 */
	mask = vap->va_mask;

	if ((mask & (AT_UID | AT_GID)) || projid != ZFS_INVALID_PROJID) {
		handle_eadir = B_TRUE;
		err = sa_lookup(zp->z_sa_hdl, SA_ZPL_XATTR(zfsvfs),
		    &xattr_obj, sizeof (xattr_obj));

		if (err == 0 && xattr_obj) {
			err = zfs_zget(zp->z_zfsvfs, xattr_obj, &attrzp);
			if (err == 0) {
				err = vn_lock(ZTOV(attrzp), LK_EXCLUSIVE);
				if (err != 0)
					vrele(ZTOV(attrzp));
			}
			if (err)
				goto out2;
		}
		if (mask & AT_UID) {
			new_uid = zfs_fuid_create(zfsvfs,
			    (uint64_t)vap->va_uid, cr, ZFS_OWNER, &fuidp);
			if (new_uid != zp->z_uid &&
			    zfs_id_overquota(zfsvfs, DMU_USERUSED_OBJECT,
			    new_uid)) {
				if (attrzp)
					vput(ZTOV(attrzp));
				err = SET_ERROR(EDQUOT);
				goto out2;
			}
		}

		if (mask & AT_GID) {
			new_gid = zfs_fuid_create(zfsvfs, (uint64_t)vap->va_gid,
			    cr, ZFS_GROUP, &fuidp);
			if (new_gid != zp->z_gid &&
			    zfs_id_overquota(zfsvfs, DMU_GROUPUSED_OBJECT,
			    new_gid)) {
				if (attrzp)
					vput(ZTOV(attrzp));
				err = SET_ERROR(EDQUOT);
				goto out2;
			}
		}

		if (projid != ZFS_INVALID_PROJID &&
		    zfs_id_overquota(zfsvfs, DMU_PROJECTUSED_OBJECT, projid)) {
			if (attrzp)
				vput(ZTOV(attrzp));
			err = SET_ERROR(EDQUOT);
			goto out2;
		}
	}
	tx = dmu_tx_create(os);

	if (mask & AT_MODE) {
		uint64_t pmode = zp->z_mode;
		uint64_t acl_obj;
		new_mode = (pmode & S_IFMT) | (vap->va_mode & ~S_IFMT);

		if (zp->z_zfsvfs->z_acl_mode == ZFS_ACL_RESTRICTED &&
		    !(zp->z_pflags & ZFS_ACL_TRIVIAL)) {
			err = SET_ERROR(EPERM);
			goto out;
		}

		if ((err = zfs_acl_chmod_setattr(zp, &aclp, new_mode)))
			goto out;

		if (!zp->z_is_sa && ((acl_obj = zfs_external_acl(zp)) != 0)) {
			/*
			 * Are we upgrading ACL from old V0 format
			 * to V1 format?
			 */
			if (zfsvfs->z_version >= ZPL_VERSION_FUID &&
			    zfs_znode_acl_version(zp) ==
			    ZFS_ACL_VERSION_INITIAL) {
				dmu_tx_hold_free(tx, acl_obj, 0,
				    DMU_OBJECT_END);
				dmu_tx_hold_write(tx, DMU_NEW_OBJECT,
				    0, aclp->z_acl_bytes);
			} else {
				dmu_tx_hold_write(tx, acl_obj, 0,
				    aclp->z_acl_bytes);
			}
		} else if (!zp->z_is_sa && aclp->z_acl_bytes > ZFS_ACE_SPACE) {
			dmu_tx_hold_write(tx, DMU_NEW_OBJECT,
			    0, aclp->z_acl_bytes);
		}
		dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_TRUE);
	} else {
		if (((mask & AT_XVATTR) &&
		    XVA_ISSET_REQ(xvap, XAT_AV_SCANSTAMP)) ||
		    (projid != ZFS_INVALID_PROJID &&
		    !(zp->z_pflags & ZFS_PROJID)) ||
		    !zp->z_has_seq)
			dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_TRUE);
		else
			dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
	}

	if (attrzp) {
		dmu_tx_hold_sa(tx, attrzp->z_sa_hdl, ZFS_SEQ_MAY_GROW(attrzp));
	}

	fuid_dirtied = zfsvfs->z_fuid_dirty;
	if (fuid_dirtied)
		zfs_fuid_txhold(zfsvfs, tx);

	zfs_sa_upgrade_txholds(tx, zp);

	err = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (err)
		goto out;

	count = 0;
	/*
	 * Set each attribute requested.
	 * We group settings according to the locks they need to acquire.
	 *
	 * Note: you cannot set ctime directly, although it will be
	 * updated as a side-effect of calling this function.
	 */

	if (projid != ZFS_INVALID_PROJID && !(zp->z_pflags & ZFS_PROJID)) {
		/*
		 * For the existed object that is upgraded from old system,
		 * its on-disk layout has no slot for the project ID attribute.
		 * But quota accounting logic needs to access related slots by
		 * offset directly. So we need to adjust old objects' layout
		 * to make the project ID to some unified and fixed offset.
		 */
		if (attrzp)
			err = sa_add_projid(attrzp->z_sa_hdl, tx, projid);
		if (err == 0)
			err = sa_add_projid(zp->z_sa_hdl, tx, projid);

		if (unlikely(err == EEXIST))
			err = 0;
		else if (err != 0)
			goto out;
		else
			projid = ZFS_INVALID_PROJID;
	}

	if (mask & (AT_UID|AT_GID|AT_MODE))
		mutex_enter(&zp->z_acl_lock);

	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_FLAGS(zfsvfs), NULL,
	    &zp->z_pflags, sizeof (zp->z_pflags));

	if (attrzp) {
		if (mask & (AT_UID|AT_GID|AT_MODE))
			mutex_enter(&attrzp->z_acl_lock);
		SA_ADD_BULK_ATTR(xattr_bulk, xattr_count,
		    SA_ZPL_FLAGS(zfsvfs), NULL, &attrzp->z_pflags,
		    sizeof (attrzp->z_pflags));
		if (projid != ZFS_INVALID_PROJID) {
			attrzp->z_projid = projid;
			SA_ADD_BULK_ATTR(xattr_bulk, xattr_count,
			    SA_ZPL_PROJID(zfsvfs), NULL, &attrzp->z_projid,
			    sizeof (attrzp->z_projid));
		}
	}

	if (mask & (AT_UID|AT_GID)) {

		if (mask & AT_UID) {
			SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_UID(zfsvfs), NULL,
			    &new_uid, sizeof (new_uid));
			zp->z_uid = new_uid;
			if (attrzp) {
				SA_ADD_BULK_ATTR(xattr_bulk, xattr_count,
				    SA_ZPL_UID(zfsvfs), NULL, &new_uid,
				    sizeof (new_uid));
				attrzp->z_uid = new_uid;
			}
		}

		if (mask & AT_GID) {
			SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_GID(zfsvfs),
			    NULL, &new_gid, sizeof (new_gid));
			zp->z_gid = new_gid;
			if (attrzp) {
				SA_ADD_BULK_ATTR(xattr_bulk, xattr_count,
				    SA_ZPL_GID(zfsvfs), NULL, &new_gid,
				    sizeof (new_gid));
				attrzp->z_gid = new_gid;
			}
		}
		if (!(mask & AT_MODE)) {
			SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MODE(zfsvfs),
			    NULL, &new_mode, sizeof (new_mode));
			new_mode = zp->z_mode;
		}
		err = zfs_acl_chown_setattr(zp);
		ASSERT0(err);
		if (attrzp) {
			err = zfs_acl_chown_setattr(attrzp);
			ASSERT0(err);
		}
	}

	if (mask & AT_MODE) {
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MODE(zfsvfs), NULL,
		    &new_mode, sizeof (new_mode));
		zp->z_mode = new_mode;
		ASSERT3P(aclp, !=, NULL);
		err = zfs_aclset_common(zp, aclp, cr, tx);
		ASSERT0(err);
		if (zp->z_acl_cached)
			zfs_acl_free(zp->z_acl_cached);
		zp->z_acl_cached = aclp;
		aclp = NULL;
	}


	if (mask & AT_ATIME) {
		ZFS_TIME_ENCODE(&vap->va_atime, zp->z_atime);
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_ATIME(zfsvfs), NULL,
		    &zp->z_atime, sizeof (zp->z_atime));
	}

	if (mask & AT_MTIME) {
		ZFS_TIME_ENCODE(&vap->va_mtime, mtime);
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MTIME(zfsvfs), NULL,
		    mtime, sizeof (mtime));
	}

	if (projid != ZFS_INVALID_PROJID) {
		zp->z_projid = projid;
		SA_ADD_BULK_ATTR(bulk, count,
		    SA_ZPL_PROJID(zfsvfs), NULL, &zp->z_projid,
		    sizeof (zp->z_projid));
	}

	/* XXX - shouldn't this be done *before* the ATIME/MTIME checks? */
	if (mask & AT_SIZE && !(mask & AT_MTIME)) {
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MTIME(zfsvfs),
		    NULL, mtime, sizeof (mtime));
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_CTIME(zfsvfs), NULL,
		    &ctime, sizeof (ctime));
		zfs_tstamp_update_setup(zp, CONTENT_MODIFIED, mtime, ctime);
		ZFS_PERSIST_SEQ(zp, bulk, count);
	} else if (mask != 0) {
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_CTIME(zfsvfs), NULL,
		    &ctime, sizeof (ctime));
		zfs_tstamp_update_setup(zp, STATE_CHANGED, mtime, ctime);
		ZFS_PERSIST_SEQ(zp, bulk, count);
		if (attrzp) {
			SA_ADD_BULK_ATTR(xattr_bulk, xattr_count,
			    SA_ZPL_CTIME(zfsvfs), NULL,
			    &ctime, sizeof (ctime));
			zfs_tstamp_update_setup(attrzp, STATE_CHANGED,
			    mtime, ctime);
			ZFS_PERSIST_SEQ(attrzp, xattr_bulk, xattr_count);
		}
	}

	/*
	 * Do this after setting timestamps to prevent timestamp
	 * update from toggling bit
	 */

	if (xoap && (mask & AT_XVATTR)) {

		if (XVA_ISSET_REQ(xvap, XAT_CREATETIME))
			xoap->xoa_createtime = vap->va_birthtime;
		/*
		 * restore trimmed off masks
		 * so that return masks can be set for caller.
		 */

		if (XVA_ISSET_REQ(&tmpxvattr, XAT_APPENDONLY)) {
			XVA_SET_REQ(xvap, XAT_APPENDONLY);
		}
		if (XVA_ISSET_REQ(&tmpxvattr, XAT_NOUNLINK)) {
			XVA_SET_REQ(xvap, XAT_NOUNLINK);
		}
		if (XVA_ISSET_REQ(&tmpxvattr, XAT_IMMUTABLE)) {
			XVA_SET_REQ(xvap, XAT_IMMUTABLE);
		}
		if (XVA_ISSET_REQ(&tmpxvattr, XAT_NODUMP)) {
			XVA_SET_REQ(xvap, XAT_NODUMP);
		}
		if (XVA_ISSET_REQ(&tmpxvattr, XAT_AV_MODIFIED)) {
			XVA_SET_REQ(xvap, XAT_AV_MODIFIED);
		}
		if (XVA_ISSET_REQ(&tmpxvattr, XAT_AV_QUARANTINED)) {
			XVA_SET_REQ(xvap, XAT_AV_QUARANTINED);
		}
		if (XVA_ISSET_REQ(&tmpxvattr, XAT_PROJINHERIT)) {
			XVA_SET_REQ(xvap, XAT_PROJINHERIT);
		}

		if (XVA_ISSET_REQ(xvap, XAT_AV_SCANSTAMP))
			ASSERT3S(vp->v_type, ==, VREG);

		zfs_xvattr_set(zp, xvap, tx);
	}

	if (fuid_dirtied)
		zfs_fuid_sync(zfsvfs, tx);

	if (mask != 0)
		zfs_log_setattr(zilog, tx, TX_SETATTR, zp, vap, mask, fuidp);

	if (mask & (AT_UID|AT_GID|AT_MODE))
		mutex_exit(&zp->z_acl_lock);

	if (attrzp) {
		if (mask & (AT_UID|AT_GID|AT_MODE))
			mutex_exit(&attrzp->z_acl_lock);
	}
out:
	if (err == 0 && attrzp) {
		ASSERT3S(xattr_count, <=, ARRAY_SIZE(xattr_bulk));
		err2 = sa_bulk_update(attrzp->z_sa_hdl, xattr_bulk,
		    xattr_count, tx);
		ASSERT0(err2);
	}

	if (aclp)
		zfs_acl_free(aclp);

	if (fuidp) {
		zfs_fuid_info_free(fuidp);
		fuidp = NULL;
	}

	if (err) {
		dmu_tx_abort(tx);
		if (attrzp)
			vput(ZTOV(attrzp));
	} else {
		ASSERT3S(count, <=, ARRAY_SIZE(bulk));
		err2 = sa_bulk_update(zp->z_sa_hdl, bulk, count, tx);
		dmu_tx_commit(tx);
		if (attrzp) {
			if (err2 == 0 && handle_eadir)
				err = zfs_setattr_dir(attrzp);
			vput(ZTOV(attrzp));
		}
	}

out2:
	if (err == 0 && os->os_sync == ZFS_SYNC_ALWAYS)
		err = zil_commit(zilog, 0);

	zfs_exit(zfsvfs, FTAG);
	return (err);
}

/*
 * Look up the directory entries corresponding to the source and target
 * directory/name pairs.
 */
static int
zfs_rename_relock_lookup(znode_t *sdzp, const struct componentname *scnp,
    znode_t **szpp, znode_t *tdzp, const struct componentname *tcnp,
    znode_t **tzpp)
{
	zfsvfs_t *zfsvfs;
	znode_t *szp, *tzp;
	int error;

	/*
	 * Before using sdzp and tdzp we must ensure that they are live.
	 * As a porting legacy from illumos we have two things to worry
	 * about.  One is typical for FreeBSD and it is that the vnode is
	 * not reclaimed (doomed).  The other is that the znode is live.
	 * The current code can invalidate the znode without acquiring the
	 * corresponding vnode lock if the object represented by the znode
	 * and vnode is no longer valid after a rollback or receive operation.
	 * z_teardown_lock hidden behind zfs_enter and zfs_exit is the lock
	 * that protects the znodes from the invalidation.
	 */
	zfsvfs = sdzp->z_zfsvfs;
	ASSERT3P(zfsvfs, ==, tdzp->z_zfsvfs);
	if ((error = zfs_enter_verify_zp(zfsvfs, sdzp, FTAG)) != 0)
		return (error);
	if ((error = zfs_verify_zp(tdzp)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * Re-resolve svp to be certain it still exists and fetch the
	 * correct vnode.
	 */
	error = zfs_dirent_lookup(sdzp, scnp->cn_nameptr, &szp, ZEXISTS);
	if (error != 0) {
		/* Source entry invalid or not there. */
		if ((scnp->cn_flags & ISDOTDOT) != 0 ||
		    (scnp->cn_namelen == 1 && scnp->cn_nameptr[0] == '.'))
			error = SET_ERROR(EINVAL);
		goto out;
	}
	*szpp = szp;

	/*
	 * Re-resolve tvp, if it disappeared we just carry on.
	 */
	error = zfs_dirent_lookup(tdzp, tcnp->cn_nameptr, &tzp, 0);
	if (error != 0) {
		vrele(ZTOV(szp));
		if ((tcnp->cn_flags & ISDOTDOT) != 0)
			error = SET_ERROR(EINVAL);
		goto out;
	}
	*tzpp = tzp;
out:
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

/*
 * Lock the four vnodes of a rename.  The source directory is locked
 * waiting and the rest without; when one is busy, all are let go, that
 * one is waited for and let go too, and the whole starts again, so that
 * nothing is held while waiting.  NetBSD's vnode locks do not recurse,
 * as FreeBSD's ZFS vnodes' do, so a vnode that is two of the four -- the
 * one directory, or two names of one file -- is locked once.  On error
 * nothing is left locked.
 */
static int
zfs_rename_relock(struct vnode *sdvp, struct vnode **svpp,
    struct vnode *tdvp, struct vnode **tvpp,
    const struct componentname *scnp, const struct componentname *tcnp)
{
	struct vnode *svp, *tvp, *busy;
	znode_t *szp, *tzp;
	int error;

	VOP_UNLOCK(tdvp);
	if (*tvpp != NULL && *tvpp != tdvp)
		VOP_UNLOCK(*tvpp);

relock:
	if ((error = vn_lock(sdvp, LK_EXCLUSIVE)) != 0)
		return (error);
	if (tdvp != sdvp &&
	    (error = vn_lock(tdvp, LK_EXCLUSIVE | LK_NOWAIT)) != 0) {
		VOP_UNLOCK(sdvp);
		if (error != EBUSY)
			return (error);
		if ((error = vn_lock(tdvp, LK_EXCLUSIVE)) != 0)
			return (error);
		VOP_UNLOCK(tdvp);
		goto relock;
	}

	error = zfs_rename_relock_lookup(VTOZ(sdvp), scnp, &szp, VTOZ(tdvp),
	    tcnp, &tzp);
	if (error != 0)
		goto unlock_dirs;
	svp = ZTOV(szp);
	tvp = tzp != NULL ? ZTOV(tzp) : NULL;

	/*
	 * Neither file can be one of the directories: that is a directory
	 * moved into itself, or a "." or ".." the lookup let through.
	 */
	if (svp == sdvp || svp == tdvp || tvp == sdvp || tvp == tdvp) {
		vrele(svp);
		if (tvp != NULL)
			vrele(tvp);
		error = SET_ERROR(EINVAL);
		goto unlock_dirs;
	}

	busy = NULL;
	if ((error = vn_lock(svp, LK_EXCLUSIVE | LK_NOWAIT)) != 0) {
		busy = svp;
	} else if (tvp != NULL && tvp != svp &&
	    (error = vn_lock(tvp, LK_EXCLUSIVE | LK_NOWAIT)) != 0) {
		VOP_UNLOCK(svp);
		busy = tvp;
	}
	if (busy != NULL) {
		VOP_UNLOCK(sdvp);
		if (tdvp != sdvp)
			VOP_UNLOCK(tdvp);
		if (error == EBUSY &&
		    (error = vn_lock(busy, LK_EXCLUSIVE)) == 0)
			VOP_UNLOCK(busy);
		vrele(*svpp);
		*svpp = svp;
		if (*tvpp != NULL)
			vrele(*tvpp);
		*tvpp = tvp;
		if (error != 0)
			return (error);
		goto relock;
	}

	vrele(*svpp);
	*svpp = svp;
	if (*tvpp != NULL)
		vrele(*tvpp);
	*tvpp = tvp;
	return (0);

unlock_dirs:
	VOP_UNLOCK(sdvp);
	if (tdvp != sdvp)
		VOP_UNLOCK(tdvp);
	return (error);
}

/* Let go of what zfs_rename_relock() locked, each vnode once. */
static void
zfs_rename_unlock(struct vnode *sdvp, struct vnode *svp, struct vnode *tdvp,
    struct vnode *tvp)
{

	VOP_UNLOCK(sdvp);
	if (tdvp != sdvp)
		VOP_UNLOCK(tdvp);
	VOP_UNLOCK(svp);
	if (tvp != NULL && tvp != svp)
		VOP_UNLOCK(tvp);
}

/*
 * Note that we must use VRELE_ASYNC in this function as it walks
 * up the directory tree and vrele may need to acquire an exclusive
 * lock if a last reference to a vnode is dropped.
 */
static int
zfs_rename_check(znode_t *szp, znode_t *sdzp, znode_t *tdzp)
{
	zfsvfs_t	*zfsvfs;
	znode_t		*zp, *zp1;
	uint64_t	parent;
	int		error;

	zfsvfs = tdzp->z_zfsvfs;
	if (tdzp == szp)
		return (SET_ERROR(EINVAL));
	if (tdzp == sdzp)
		return (0);
	if (tdzp->z_id == zfsvfs->z_root)
		return (0);
	zp = tdzp;
	for (;;) {
		ASSERT(!zp->z_unlinked);
		if ((error = sa_lookup(zp->z_sa_hdl,
		    SA_ZPL_PARENT(zfsvfs), &parent, sizeof (parent))) != 0)
			break;

		if (parent == szp->z_id) {
			error = SET_ERROR(EINVAL);
			break;
		}
		if (parent == zfsvfs->z_root)
			break;
		if (parent == sdzp->z_id)
			break;

		error = zfs_zget(zfsvfs, parent, &zp1);
		if (error != 0)
			break;

		if (zp != tdzp)
			vrele_async(ZTOV(zp));
		zp = zp1;
	}

	if (error == ENOTDIR)
		panic("checkpath: .. not a directory\n");
	if (zp != tdzp)
		vrele_async(ZTOV(zp));
	return (error);
}

static int
zfs_do_rename_impl(vnode_t *sdvp, vnode_t **svpp, struct componentname *scnp,
    vnode_t *tdvp, vnode_t **tvpp, struct componentname *tcnp,
    cred_t *cr);

/*
 * Move an entry from the provided source directory to the target
 * directory.  Change the entry name as indicated.
 *
 *	IN:	sdvp	- Source directory containing the "old entry".
 *		scnp	- Old entry name.
 *		tdvp	- Target directory to contain the "new entry".
 *		tcnp	- New entry name.
 *		cr	- credentials of caller.
 *	INOUT:	svpp	- Source file
 *		tvpp	- Target file, may point to NULL initially
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	sdvp,tdvp - ctime|mtime updated
 */
static int
zfs_do_rename(vnode_t *sdvp, vnode_t **svpp, struct componentname *scnp,
    vnode_t *tdvp, vnode_t **tvpp, struct componentname *tcnp,
    cred_t *cr)
{
	int	error;

	ASSERT_VOP_ELOCKED(tdvp, __func__);
	if (*tvpp != NULL)
		ASSERT_VOP_ELOCKED(*tvpp, __func__);

	/* Reject renames across filesystems. */
	if ((*svpp)->v_mount != tdvp->v_mount ||
	    ((*tvpp) != NULL && (*svpp)->v_mount != (*tvpp)->v_mount)) {
		error = SET_ERROR(EXDEV);
		goto out;
	}

	if (zfsctl_is_node(tdvp)) {
		error = SET_ERROR(EXDEV);
		goto out;
	}

	/*
	 * Lock all four vnodes to ensure safety and semantics of renaming.
	 */
	error = zfs_rename_relock(sdvp, svpp, tdvp, tvpp, scnp, tcnp);
	if (error != 0) {
		/* no vnodes are locked in the case of error here */
		return (error);
	}

	error = zfs_do_rename_impl(sdvp, svpp, scnp, tdvp, tvpp, tcnp, cr);
	zfs_rename_unlock(sdvp, *svpp, tdvp, *tvpp);
	return (error);
out:
	if (*tvpp != NULL)
		VOP_UNLOCK(*tvpp);
	VOP_UNLOCK(tdvp);

	return (error);
}

static int
zfs_do_rename_impl(vnode_t *sdvp, vnode_t **svpp, struct componentname *scnp,
    vnode_t *tdvp, vnode_t **tvpp, struct componentname *tcnp,
    cred_t *cr)
{
	dmu_tx_t	*tx;
	zfsvfs_t	*zfsvfs;
	zilog_t		*zilog;
	znode_t		*tdzp, *sdzp, *tzp, *szp;
	const char	*snm = scnp->cn_nameptr;
	const char	*tnm = tcnp->cn_nameptr;
	int		error;

	tdzp = VTOZ(tdvp);
	sdzp = VTOZ(sdvp);
	zfsvfs = tdzp->z_zfsvfs;

	if ((error = zfs_enter_verify_zp(zfsvfs, tdzp, FTAG)) != 0)
		return (error);
	if ((error = zfs_verify_zp(sdzp)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}
	zilog = zfsvfs->z_log;

	if (zfsvfs->z_utf8 && u8_validate(tnm,
	    strlen(tnm), NULL, U8_VALIDATE_ENTIRE, &error) < 0) {
		error = SET_ERROR(EILSEQ);
		goto out;
	}

	/* If source and target are the same file, there is nothing to do. */
	if ((*svpp) == (*tvpp)) {
		error = 0;
		goto out;
	}

	if (((*svpp)->v_type == VDIR && (*svpp)->v_mountedhere != NULL) ||
	    ((*tvpp) != NULL && (*tvpp)->v_type == VDIR &&
	    (*tvpp)->v_mountedhere != NULL)) {
		error = SET_ERROR(EXDEV);
		goto out;
	}

	szp = VTOZ(*svpp);
	if ((error = zfs_verify_zp(szp)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}
	tzp = *tvpp == NULL ? NULL : VTOZ(*tvpp);
	if (tzp != NULL) {
		if ((error = zfs_verify_zp(tzp)) != 0) {
			zfs_exit(zfsvfs, FTAG);
			return (error);
		}
	}

	/*
	 * This is to prevent the creation of links into attribute space
	 * by renaming a linked file into/outof an attribute directory.
	 * See the comment in zfs_link() for why this is considered bad.
	 */
	if ((tdzp->z_pflags & ZFS_XATTR) != (sdzp->z_pflags & ZFS_XATTR)) {
		error = SET_ERROR(EINVAL);
		goto out;
	}

	/*
	 * If we are using project inheritance, means if the directory has
	 * ZFS_PROJINHERIT set, then its descendant directories will inherit
	 * not only the project ID, but also the ZFS_PROJINHERIT flag. Under
	 * such case, we only allow renames into our tree when the project
	 * IDs are the same.
	 *
	 * A rename within a single directory leaves the object exactly where
	 * it already is, so it cannot move it between projects and is always
	 * allowed.  Objects created before symlinks and other non-regular
	 * files began inheriting a project ID carry none of their own, and
	 * would otherwise not be renameable within the very directory that
	 * holds them -- which breaks "ln -sfn", implemented as
	 * create-under-a-temporary-name-then-rename.
	 */
	if (sdzp != tdzp && tdzp->z_pflags & ZFS_PROJINHERIT &&
	    tdzp->z_projid != szp->z_projid) {
		error = SET_ERROR(EXDEV);
		goto out;
	}

	/*
	 * Must have write access at the source to remove the old entry
	 * and write access at the target to create the new entry.
	 * Note that if target and source are the same, this can be
	 * done in a single check.
	 */
	if ((error = zfs_zaccess_rename(sdzp, szp, tdzp, tzp, cr)))
		goto out;

	if ((*svpp)->v_type == VDIR) {
		/*
		 * Avoid ".", "..", and aliases of "." for obvious reasons.
		 */
		if ((scnp->cn_namelen == 1 && scnp->cn_nameptr[0] == '.') ||
		    sdzp == szp ||
		    (scnp->cn_flags | tcnp->cn_flags) & ISDOTDOT) {
			error = EINVAL;
			goto out;
		}

		/*
		 * Check to make sure rename is valid.
		 * Can't do a move like this: /usr/a/b to /usr/a/b/c/d
		 */
		if ((error = zfs_rename_check(szp, sdzp, tdzp)))
			goto out;
	}

	/*
	 * Does target exist?
	 */
	if (tzp) {
		/*
		 * Source and target must be the same type.
		 */
		if ((*svpp)->v_type == VDIR) {
			if ((*tvpp)->v_type != VDIR) {
				error = SET_ERROR(ENOTDIR);
				goto out;
			} else {
				cache_purge(tdvp);
				if (sdvp != tdvp)
					cache_purge(sdvp);
			}
		} else {
			if ((*tvpp)->v_type == VDIR) {
				error = SET_ERROR(EISDIR);
				goto out;
			}
		}
	}

	if (*tvpp != NULL)
	if (tdvp != *tvpp)

	vnevent_rename_src(*svpp, sdvp, scnp->cn_nameptr, ct);
	if (tzp)
		vnevent_rename_dest(*tvpp, tdvp, tnm, ct);

	/*
	 * notify the target directory if it is not the same
	 * as source directory.
	 */
	if (tdvp != sdvp) {
		vnevent_rename_dest_dir(tdvp, ct);
	}

	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_sa(tx, szp->z_sa_hdl, ZFS_SEQ_MAY_GROW(szp));
	dmu_tx_hold_sa(tx, sdzp->z_sa_hdl, ZFS_SEQ_MAY_GROW(sdzp));
	dmu_tx_hold_zap(tx, sdzp->z_id, FALSE, snm);
	dmu_tx_hold_zap(tx, tdzp->z_id, TRUE, tnm);
	if (sdzp != tdzp) {
		dmu_tx_hold_sa(tx, tdzp->z_sa_hdl, ZFS_SEQ_MAY_GROW(tdzp));
		zfs_sa_upgrade_txholds(tx, tdzp);
	}
	if (tzp) {
		dmu_tx_hold_sa(tx, tzp->z_sa_hdl, ZFS_SEQ_MAY_GROW(tzp));
		zfs_sa_upgrade_txholds(tx, tzp);
	}

	zfs_sa_upgrade_txholds(tx, szp);
	dmu_tx_hold_zap(tx, zfsvfs->z_unlinkedobj, FALSE, NULL);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error) {
		dmu_tx_abort(tx);
		goto out_seq;
	}

	if (tzp)	/* Attempt to remove the existing target */
		error = zfs_link_destroy(tdzp, tnm, tzp, tx, 0, NULL);

	if (error == 0) {
		error = zfs_link_create(tdzp, tnm, szp, tx, ZRENAMING);
		if (error == 0) {
			szp->z_pflags |= ZFS_AV_MODIFIED;

			error = sa_update(szp->z_sa_hdl, SA_ZPL_FLAGS(zfsvfs),
			    (void *)&szp->z_pflags, sizeof (uint64_t), tx);
			ASSERT0(error);

			error = zfs_link_destroy(sdzp, snm, szp, tx, ZRENAMING,
			    NULL);
			if (error == 0) {
				zfs_log_rename(zilog, tx, TX_RENAME, sdzp,
				    snm, tdzp, tnm, szp);
			} else {
				/*
				 * At this point, we have successfully created
				 * the target name, but have failed to remove
				 * the source name.  Since the create was done
				 * with the ZRENAMING flag, there are
				 * complications; for one, the link count is
				 * wrong.  The easiest way to deal with this
				 * is to remove the newly created target, and
				 * return the original error.  This must
				 * succeed; fortunately, it is very unlikely to
				 * fail, since we just created it.
				 */
				VERIFY0(zfs_link_destroy(tdzp, tnm, szp, tx,
				    ZRENAMING, NULL));
			}
		}
		if (error == 0 && zfsvfs->z_use_namecache) {
			cache_purge(sdvp);
			cache_purge(*svpp);
			if (tdvp != sdvp)
				cache_purge(tdvp);
			if (*tvpp != NULL)
				cache_purge(*tvpp);
		}
	}

	dmu_tx_commit(tx);

out_seq:
	if (*tvpp != NULL)
	if (tdvp != *tvpp)

out:
	if (error == 0 && zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		error = zil_commit(zilog, 0);
	zfs_exit(zfsvfs, FTAG);

	return (error);
}

int
zfs_rename(znode_t *sdzp, const char *sname, znode_t *tdzp, const char *tname,
    cred_t *cr, int flags, uint64_t rflags, vattr_t *wo_vap)
{
	struct componentname scn, tcn;
	vnode_t *sdvp, *tdvp;
	vnode_t *svp, *tvp;
	int error;
	svp = tvp = NULL;

	if (is_nametoolong(tdzp->z_zfsvfs, tname))
		return (SET_ERROR(ENAMETOOLONG));

	if (rflags != 0 || wo_vap != NULL)
		return (SET_ERROR(EINVAL));

	sdvp = ZTOV(sdzp);
	tdvp = ZTOV(tdzp);
	error = zfs_lookup_internal(sdzp, sname, &svp, &scn, DELETE);
	if (sdzp->z_zfsvfs->z_replay == B_FALSE)
		VOP_UNLOCK(sdvp);
	if (error != 0)
		goto fail;
	VOP_UNLOCK(svp);

	vn_lock(tdvp, LK_EXCLUSIVE | LK_RETRY);
	error = zfs_lookup_internal(tdzp, tname, &tvp, &tcn, RENAME);
	if (error == EJUSTRETURN)
		tvp = NULL;
	else if (error != 0) {
		VOP_UNLOCK(tdvp);
		goto fail;
	}

	error = zfs_do_rename(sdvp, &svp, &scn, tdvp, &tvp, &tcn, cr);
fail:
	if (svp != NULL)
		vrele(svp);
	if (tvp != NULL)
		vrele(tvp);

	return (error);
}

/*
 * Insert the indicated symbolic reference entry into the directory.
 *
 *	IN:	dvp	- Directory to contain new symbolic link.
 *		link	- Name for new symlink entry.
 *		vap	- Attributes of new entry.
 *		cr	- credentials of caller.
 *		ct	- caller context
 *		flags	- case flags
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	dvp - ctime|mtime updated
 */
int
zfs_symlink(znode_t *dzp, const char *name, vattr_t *vap,
    const char *link, znode_t **zpp, cred_t *cr, int flags)
{
	(void) flags;
	znode_t		*zp;
	dmu_tx_t	*tx;
	zfsvfs_t	*zfsvfs = dzp->z_zfsvfs;
	zilog_t		*zilog;
	uint64_t	len = strlen(link);
	int		error;
	zfs_acl_ids_t	acl_ids;
	boolean_t	fuid_dirtied;
	uint64_t	txtype = TX_SYMLINK;

	ASSERT3S(vap->va_type, ==, VLNK);

	if (is_nametoolong(zfsvfs, name))
		return (SET_ERROR(ENAMETOOLONG));

	if ((error = zfs_enter_verify_zp(zfsvfs, dzp, FTAG)) != 0)
		return (error);
	zilog = zfsvfs->z_log;

	if (zfsvfs->z_utf8 && u8_validate(name, strlen(name),
	    NULL, U8_VALIDATE_ENTIRE, &error) < 0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EILSEQ));
	}

	if (len > MAXPATHLEN) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(ENAMETOOLONG));
	}

	if ((error = zfs_acl_ids_create(dzp, 0,
	    vap, cr, NULL, &acl_ids)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * Attempt to lock directory; fail if entry already exists.
	 */
	error = zfs_dirent_lookup(dzp, name, &zp, ZNEW);
	if (error) {
		zfs_acl_ids_free(&acl_ids);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	if ((error = zfs_zaccess(dzp, ACE_ADD_FILE, 0, B_FALSE, cr))) {
		zfs_acl_ids_free(&acl_ids);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	if (zfs_acl_ids_overquota(zfsvfs, &acl_ids, zfs_inherit_projid(dzp))) {
		zfs_acl_ids_free(&acl_ids);
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EDQUOT));
	}

	getnewvnode_reserve();
	tx = dmu_tx_create(zfsvfs->z_os);
	fuid_dirtied = zfsvfs->z_fuid_dirty;
	dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0, MAX(1, len));
	dmu_tx_hold_zap(tx, dzp->z_id, TRUE, name);
	dmu_tx_hold_sa_create(tx, acl_ids.z_aclp->z_acl_bytes +
	    ZFS_SA_BASE_ATTR_SIZE + len);
	dmu_tx_hold_sa(tx, dzp->z_sa_hdl, ZFS_SEQ_MAY_GROW(dzp));
	if (!zfsvfs->z_use_sa && acl_ids.z_aclp->z_acl_bytes > ZFS_ACE_SPACE) {
		dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0,
		    acl_ids.z_aclp->z_acl_bytes);
	}
	if (fuid_dirtied)
		zfs_fuid_txhold(zfsvfs, tx);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error) {
		zfs_acl_ids_free(&acl_ids);
		dmu_tx_abort(tx);
		getnewvnode_drop_reserve();
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * Create a new object for the symlink.
	 * for version 4 ZPL datasets the symlink will be an SA attribute
	 */
	zfs_mknode(dzp, vap, tx, cr, 0, &zp, &acl_ids);

	if (fuid_dirtied)
		zfs_fuid_sync(zfsvfs, tx);

	if (zp->z_is_sa)
		error = sa_update(zp->z_sa_hdl, SA_ZPL_SYMLINK(zfsvfs),
		    (void *)__UNCONST(link), len, tx);
	else
		zfs_sa_symlink(zp, (char *)__UNCONST(link), len, tx);

	zp->z_size = len;
	(void) sa_update(zp->z_sa_hdl, SA_ZPL_SIZE(zfsvfs),
	    &zp->z_size, sizeof (zp->z_size), tx);
	/*
	 * Insert the new object into the directory.
	 */
	error = zfs_link_create(dzp, name, zp, tx, ZNEW);
	if (error != 0) {
		zfs_znode_delete(zp, tx);
		VOP_UNLOCK(ZTOV(zp));
		zrele(zp);
	} else {
		zfs_log_symlink(zilog, tx, txtype, dzp, zp, name, link);
	}

	zfs_acl_ids_free(&acl_ids);

	dmu_tx_commit(tx);

	getnewvnode_drop_reserve();

	if (error == 0) {
		*zpp = zp;

		if (zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
			error = zil_commit(zilog, 0);
	}

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

/*
 * Return, in the buffer contained in the provided uio structure,
 * the symbolic path referred to by vp.
 *
 *	IN:	vp	- vnode of symbolic link.
 *		uio	- structure to contain the link path.
 *		cr	- credentials of caller.
 *		ct	- caller context
 *
 *	OUT:	uio	- structure containing the link path.
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	vp - atime updated
 */
static int
zfs_readlink(vnode_t *vp, zfs_uio_t *uio, cred_t *cr, caller_context_t *ct)
{
	(void) cr, (void) ct;
	znode_t		*zp = VTOZ(vp);
	zfsvfs_t	*zfsvfs = zp->z_zfsvfs;
	int		error;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	if (zp->z_is_sa)
		error = sa_lookup_uio(zp->z_sa_hdl,
		    SA_ZPL_SYMLINK(zfsvfs), uio);
	else
		error = zfs_sa_readlink(zp, uio);

	ZFS_ACCESSTIME_STAMP(zfsvfs, zp);

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

/*
 * Insert a new entry into directory tdvp referencing svp.
 *
 *	IN:	tdvp	- Directory to contain new entry.
 *		svp	- vnode of new entry.
 *		name	- name of new entry.
 *		cr	- credentials of caller.
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	tdvp - ctime|mtime updated
 *	 svp - ctime updated
 */
int
zfs_link(znode_t *tdzp, znode_t *szp, const char *name, cred_t *cr,
    int flags)
{
	(void) flags;
	znode_t		*tzp;
	zfsvfs_t	*zfsvfs = tdzp->z_zfsvfs;
	zilog_t		*zilog;
	dmu_tx_t	*tx;
	int		error;
	uint64_t	parent;
	uid_t		owner;

	ASSERT3S(ZTOV(tdzp)->v_type, ==, VDIR);

	if (is_nametoolong(zfsvfs, name))
		return (SET_ERROR(ENAMETOOLONG));

	if ((error = zfs_enter_verify_zp(zfsvfs, tdzp, FTAG)) != 0)
		return (error);
	zilog = zfsvfs->z_log;

	/*
	 * POSIX dictates that we return EPERM here.
	 * Better choices include ENOTSUP or EISDIR.
	 */
	if (ZTOV(szp)->v_type == VDIR) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EPERM));
	}

	if ((error = zfs_verify_zp(szp)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * If we are using project inheritance, means if the directory has
	 * ZFS_PROJINHERIT set, then its descendant directories will inherit
	 * not only the project ID, but also the ZFS_PROJINHERIT flag. Under
	 * such case, we only allow hard link creation in our tree when the
	 * project IDs are the same.
	 */
	if (tdzp->z_pflags & ZFS_PROJINHERIT &&
	    tdzp->z_projid != szp->z_projid) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EXDEV));
	}

	if (szp->z_pflags & (ZFS_APPENDONLY |
	    ZFS_IMMUTABLE | ZFS_READONLY)) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EPERM));
	}

	/* Prevent links to .zfs/shares files */

	if ((error = sa_lookup(szp->z_sa_hdl, SA_ZPL_PARENT(zfsvfs),
	    &parent, sizeof (uint64_t))) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}
	if (parent == zfsvfs->z_shares_dir) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EPERM));
	}

	if (zfsvfs->z_utf8 && u8_validate(name,
	    strlen(name), NULL, U8_VALIDATE_ENTIRE, &error) < 0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EILSEQ));
	}

	/*
	 * We do not support links between attributes and non-attributes
	 * because of the potential security risk of creating links
	 * into "normal" file space in order to circumvent restrictions
	 * imposed in attribute space.
	 */
	if ((szp->z_pflags & ZFS_XATTR) != (tdzp->z_pflags & ZFS_XATTR)) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EINVAL));
	}


	owner = zfs_fuid_map_id(zfsvfs, szp->z_uid, cr, ZFS_OWNER);
	if (owner != crgetuid(cr) && secpolicy_basic_link(ZTOV(szp), cr) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EPERM));
	}

	if ((error = zfs_zaccess(tdzp, ACE_ADD_FILE, 0, B_FALSE, cr))) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * Attempt to lock directory; fail if entry already exists.
	 */
	error = zfs_dirent_lookup(tdzp, name, &tzp, ZNEW);
	if (error) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_sa(tx, szp->z_sa_hdl, ZFS_SEQ_MAY_GROW(szp));
	dmu_tx_hold_sa(tx, tdzp->z_sa_hdl, ZFS_SEQ_MAY_GROW(tdzp));
	dmu_tx_hold_zap(tx, tdzp->z_id, TRUE, name);
	zfs_sa_upgrade_txholds(tx, szp);
	zfs_sa_upgrade_txholds(tx, tdzp);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error) {
		dmu_tx_abort(tx);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	error = zfs_link_create(tdzp, name, szp, tx, 0);

	if (error == 0) {
		uint64_t txtype = TX_LINK;
		zfs_log_link(zilog, tx, txtype, tdzp, szp, name);
	}

	dmu_tx_commit(tx);

	if (error == 0) {
		vnevent_link(ZTOV(szp), ct);
	}

	if (error == 0 && zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		error = zil_commit(zilog, 0);

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

/*
 * Free or allocate space in a file.  Currently, this function only
 * supports the `F_FREESP' command.  However, this command is somewhat
 * misnamed, as its functionality includes the ability to allocate as
 * well as free space.
 *
 *	IN:	ip	- inode of file to free data in.
 *		cmd	- action to take (only F_FREESP supported).
 *		bfp	- section of file to free/alloc.
 *		flag	- current file open mode flags.
 *		offset	- current file offset.
 *		cr	- credentials of caller.
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	ip - ctime|mtime updated
 */
int
zfs_space(znode_t *zp, int cmd, flock64_t *bfp, int flag,
    offset_t offset, cred_t *cr)
{
	(void) offset;
	zfsvfs_t	*zfsvfs = ZTOZSB(zp);
	uint64_t	off, len;
	int		error;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	if (cmd != F_FREESP) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EINVAL));
	}

	/*
	 * Callers might not be able to detect properly that we are read-only,
	 * so check it explicitly here.
	 */
	if (zfs_is_readonly(zfsvfs)) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EROFS));
	}

	if (bfp->l_len < 0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EINVAL));
	}

	/*
	 * Permissions aren't checked on Solaris because on this OS
	 * zfs_space() can only be called with an opened file handle.
	 * On Linux we can get here through truncate_range() which
	 * operates directly on inodes, so we need to check access rights.
	 */
	if ((error = zfs_zaccess(zp, ACE_WRITE_DATA, 0, B_FALSE, cr))) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	off = bfp->l_start;
	len = bfp->l_len; /* 0 means from off to end of file */

	error = zfs_freesp(zp, off, len, flag, TRUE);

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

static void
zfs_inactive(vnode_t *vp, cred_t *cr, caller_context_t *ct)
{
	(void) cr, (void) ct;
	znode_t	*zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	int error;

	ZFS_TEARDOWN_INACTIVE_ENTER_READ(zfsvfs);
	if (zp->z_sa_hdl == NULL) {
		/*
		 * The fs has been unmounted, or we did a
		 * suspend/resume and this file no longer exists.
		 */
		ZFS_TEARDOWN_INACTIVE_EXIT_READ(zfsvfs);
		return;
	}

	/* A removed file is recycled; zfs_netbsd_inactive() says so. */
	if (zp->z_unlinked) {
		ZFS_TEARDOWN_INACTIVE_EXIT_READ(zfsvfs);
		return;
	}

	if (zp->z_atime_dirty && zp->z_unlinked == 0) {
		dmu_tx_t *tx = dmu_tx_create(zfsvfs->z_os);

		dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
		zfs_sa_upgrade_txholds(tx, zp);
		error = dmu_tx_assign(tx, DMU_TX_WAIT);
		if (error) {
			dmu_tx_abort(tx);
		} else {
			(void) sa_update(zp->z_sa_hdl, SA_ZPL_ATIME(zfsvfs),
			    (void *)&zp->z_atime, sizeof (zp->z_atime), tx);
			zp->z_atime_dirty = 0;
			dmu_tx_commit(tx);
		}
	}
	ZFS_TEARDOWN_INACTIVE_EXIT_READ(zfsvfs);
}


static int
zfs_pathconf(vnode_t *vp, int cmd, ulong_t *valp, cred_t *cr,
    caller_context_t *ct)
{
	znode_t *zp;
	zfsvfs_t *zfsvfs;
	uint_t blksize, iosize;
	int error;

	switch (cmd) {
	case _PC_LINK_MAX:
		*valp = MIN(LONG_MAX, ZFS_LINK_MAX);
		return (0);

	case _PC_FILESIZEBITS:
		*valp = 64;
		return (0);
	case _PC_MIN_HOLE_SIZE:
		iosize = vp->v_mount->mnt_stat.f_iosize;
		if (vp->v_type == VREG) {
			zp = VTOZ(vp);
			blksize = zp->z_blksz;
			if (zp->z_size <= blksize)
				blksize = MAX(blksize, iosize);
			*valp = (int)blksize;
			return (0);
		}
		if (vp->v_type == VDIR) {
			*valp = (int)iosize;
			return (0);
		}
		return (EINVAL);
	case _PC_ACL_EXTENDED:
#if 0		/* POSIX ACLs are not implemented for ZFS on FreeBSD yet. */
		zp = VTOZ(vp);
		zfsvfs = zp->z_zfsvfs;
		if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
			return (error);
		*valp = zfsvfs->z_acl_type == ZFSACLTYPE_POSIX ? 1 : 0;
		zfs_exit(zfsvfs, FTAG);
#else
		*valp = 0;
#endif
		return (0);

	case _PC_ACL_NFS4:
		zp = VTOZ(vp);
		zfsvfs = zp->z_zfsvfs;
		if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
			return (error);
		*valp = zfsvfs->z_acl_type == ZFS_ACLTYPE_NFSV4 ? 1 : 0;
		zfs_exit(zfsvfs, FTAG);
		return (0);

	case _PC_ACL_PATH_MAX:
		*valp = ACL_MAX_ENTRIES;
		return (0);

	default:
		return (EOPNOTSUPP);
	}
}


/* The open(2) flags the common code takes, from a VOP's IO_ flags. */
static int
ioflags(int ioflags)
{
	int flags = 0;

	if (ioflags & IO_APPEND)
		flags |= O_APPEND;
	if (ioflags & IO_NDELAY)
		flags |= O_NONBLOCK;
	if (ioflags & IO_DIRECT)
		flags |= O_DIRECT;
	if (ioflags & IO_SYNC)
		flags |= O_SYNC;
	if (ioflags & IO_DSYNC)
		flags |= O_DSYNC;
	return (flags);
}


/*
 * Extended attributes: NetBSD's extattr(2), over ZFS's own, which are
 * kept in the znode's system attributes (xattr=sa) or as files of a
 * hidden directory (xattr=dir).  This is FreeBSD's, whose extattr
 * interface NetBSD shares -- namespaces, names, the length-prefixed
 * list -- with the names ZFS stores encoded as FreeBSD encodes them:
 *
 *	NAMESPACE	XATTR_COMPAT	PREFIX
 *	system		*		freebsd:system:
 *	user		1		(none, as attributes made on Solaris)
 *	user		0		user.
 *
 * NetBSD's own ZFS had no extended attributes, so there are none on its
 * pools to stay compatible with; FreeBSD's encoding lets a pool move
 * between the two.  Its setxattr(2) and listxattr(2) are this same
 * interface: a "user.x" set by setxattr(2) is an attribute "user.x" of
 * the user namespace, as on FFS.
 *
 * The directory of an attribute is worked on through the znodes, not by
 * namei() relative to it as FreeBSD does, which NetBSD has no
 * kernel-side form of.  Who may read and write is decided as for the
 * attributes in the system attributes, by extattr_check_cred() against
 * the file, not by the mode of the attribute's own file.
 */

int zfs_xattr_compat = 1;

static int
zfs_check_attrname(const char *name)
{

	/* We don't allow '/' character in attribute name. */
	if (strchr(name, '/') != NULL)
		return (SET_ERROR(EINVAL));
	/* We don't allow attribute names that start with a namespace prefix. */
	if (ZFS_XA_NS_PREFIX_FORBIDDEN(name))
		return (SET_ERROR(EINVAL));
	return (0);
}

static int
zfs_create_attrname(int attrnamespace, const char *name, char *attrname,
    size_t size, boolean_t compat)
{
	const char *namespace, *prefix, *suffix;

	memset(attrname, 0, size);

	switch (attrnamespace) {
	case EXTATTR_NAMESPACE_USER:
		if (compat) {
			/*
			 * This is the default namespace by which we can access
			 * all attributes created on Solaris.
			 */
			prefix = namespace = suffix = "";
		} else {
			/*
			 * This is compatible with the user namespace encoding
			 * on Linux prior to xattr_compat, but nothing
			 * else.
			 */
			prefix = "";
			namespace = "user";
			suffix = ".";
		}
		break;
	case EXTATTR_NAMESPACE_SYSTEM:
		prefix = "freebsd:";
		namespace = EXTATTR_NAMESPACE_SYSTEM_STRING;
		suffix = ":";
		break;
	case EXTATTR_NAMESPACE_EMPTY:
	default:
		return (SET_ERROR(EINVAL));
	}
	if (snprintf(attrname, size, "%s%s%s%s", prefix, namespace, suffix,
	    name) >= size) {
		return (SET_ERROR(ENAMETOOLONG));
	}
	return (0);
}

static int
zfs_ensure_xattr_cached(znode_t *zp)
{
	int error = 0;

	ASSERT(RW_LOCK_HELD(&zp->z_xattr_lock));

	if (zp->z_xattr_cached != NULL)
		return (0);

	if (rw_write_held(&zp->z_xattr_lock))
		return (zfs_sa_get_xattr(zp));

	if (!rw_tryupgrade(&zp->z_xattr_lock)) {
		rw_exit(&zp->z_xattr_lock);
		rw_enter(&zp->z_xattr_lock, RW_WRITER);
	}
	if (zp->z_xattr_cached == NULL)
		error = zfs_sa_get_xattr(zp);
	rw_downgrade(&zp->z_xattr_lock);
	return (error);
}

/*
 * "." and ".." name the directory of the attributes and the file they
 * are of, not attributes, though zfs_dirlook() finds them: ".." is a
 * regular file, the one already locked by the VOP.
 */
static boolean_t
zfs_xattr_dotname(const char *attrname)
{

	return (attrname[0] == '\0' || strcmp(attrname, ".") == 0 ||
	    strcmp(attrname, "..") == 0);
}

/*
 * The file of an attribute in the directory of a file's attributes,
 * which the caller has locked, as a lookup has it: referenced and
 * unlocked.  Anything that is not a file is not an attribute.
 */
static int
zfs_xattr_file(vnode_t *xvp, const char *attrname, vnode_t **vpp)
{
	znode_t *xdzp = VTOZ(xvp), *zp;
	zfsvfs_t *zfsvfs = xdzp->z_zfsvfs;
	int error;

	if (zfs_xattr_dotname(attrname))
		return (SET_ERROR(ENOENT));
	if ((error = zfs_enter_verify_zp(zfsvfs, xdzp, FTAG)) != 0)
		return (error);
	error = zfs_dirlook(xdzp, attrname, &zp);
	zfs_exit(zfsvfs, FTAG);
	if (error != 0)
		return (error);
	if (ZTOV(zp)->v_type != VREG) {
		vrele(ZTOV(zp));
		return (SET_ERROR(ENOENT));
	}
	*vpp = ZTOV(zp);
	return (0);
}

static int
zfs_getextattr_dir(struct vop_getextattr_args *ap, const char *attrname)
{
	struct vattr va;
	vnode_t *xvp = NULL, *vp;
	int error;

	error = zfs_lookup(ap->a_vp, NULL, &xvp, NULL, 0, ap->a_cred,
	    LOOKUP_XATTR);
	if (error != 0)
		return (error);
	vn_lock(xvp, LK_SHARED | LK_RETRY);
	error = zfs_xattr_file(xvp, attrname, &vp);
	vput(xvp);
	if (error != 0)
		return (error);

	vn_lock(vp, LK_SHARED | LK_RETRY);
	if (ap->a_size != NULL) {
		error = VOP_GETATTR(vp, &va, ap->a_cred);
		if (error == 0)
			*ap->a_size = (size_t)va.va_size;
	} else if (ap->a_uio != NULL)
		error = VOP_READ(vp, ap->a_uio, IO_UNIT, ap->a_cred);
	vput(vp);
	return (error);
}

static int
zfs_getextattr_sa(struct vop_getextattr_args *ap, const char *attrname)
{
	znode_t *zp = VTOZ(ap->a_vp);
	uchar_t *nv_value;
	uint_t nv_size;
	int error;

	error = zfs_ensure_xattr_cached(zp);
	if (error != 0)
		return (error);

	ASSERT(RW_LOCK_HELD(&zp->z_xattr_lock));
	ASSERT3P(zp->z_xattr_cached, !=, NULL);

	error = nvlist_lookup_byte_array(zp->z_xattr_cached, attrname,
	    &nv_value, &nv_size);
	if (error != 0)
		return (SET_ERROR(error));

	if (ap->a_size != NULL)
		*ap->a_size = nv_size;
	else if (ap->a_uio != NULL)
		error = uiomove(nv_value, nv_size, ap->a_uio);
	if (error != 0)
		return (SET_ERROR(error));

	return (0);
}

static int
zfs_getextattr_impl(struct vop_getextattr_args *ap, boolean_t compat)
{
	znode_t *zp = VTOZ(ap->a_vp);
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	char attrname[EXTATTR_MAXNAMELEN+1];
	int error;

	error = zfs_create_attrname(ap->a_attrnamespace, ap->a_name, attrname,
	    sizeof (attrname), compat);
	if (error != 0)
		return (error);

	error = ENOENT;
	if (zfsvfs->z_use_sa && zp->z_is_sa)
		error = zfs_getextattr_sa(ap, attrname);
	if (error == ENOENT && !zp->z_xattr_dir_absent)
		error = zfs_getextattr_dir(ap, attrname);
	return (error);
}

/*
 * Vnode operation to retrieve a named extended attribute.
 */
static int
zfs_netbsd_getextattr(void *v)
{
	struct vop_getextattr_args *ap = v;
	znode_t *zp = VTOZ(ap->a_vp);
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	int error;

	/*
	 * If the xattr property is off, refuse the request.
	 */
	if (!(zfsvfs->z_flags & ZSB_XATTR))
		return (SET_ERROR(EOPNOTSUPP));

	error = extattr_check_cred(ap->a_vp, ap->a_attrnamespace,
	    ap->a_cred, VREAD);
	if (error != 0)
		return (SET_ERROR(error));

	error = zfs_check_attrname(ap->a_name);
	if (error != 0)
		return (error);

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);
	error = ENOENT;
	rw_enter(&zp->z_xattr_lock, RW_READER);

	error = zfs_getextattr_impl(ap, zfs_xattr_compat);
	if ((error == ENOENT || error == ENOATTR) &&
	    ap->a_attrnamespace == EXTATTR_NAMESPACE_USER) {
		/*
		 * Fall back to the alternate namespace format if we failed to
		 * find a user xattr.
		 */
		error = zfs_getextattr_impl(ap, !zfs_xattr_compat);
	}

	rw_exit(&zp->z_xattr_lock);
	zfs_exit(zfsvfs, FTAG);
	if (error == ENOENT)
		error = SET_ERROR(ENOATTR);
	return (error);
}

static int
zfs_deleteextattr_dir(struct vop_deleteextattr_args *ap, const char *attrname)
{
	vnode_t *xvp = NULL;
	int error;

	error = zfs_lookup(ap->a_vp, NULL, &xvp, NULL, 0, ap->a_cred,
	    LOOKUP_XATTR);
	if (error != 0)
		return (error);
	if (zfs_xattr_dotname(attrname)) {
		vrele(xvp);
		return (SET_ERROR(ENOENT));
	}
	vn_lock(xvp, LK_EXCLUSIVE | LK_RETRY);
	error = zfs_remove(VTOZ(xvp), attrname, ap->a_cred, 0);
	vput(xvp);
	return (error);
}

static int
zfs_deleteextattr_sa(struct vop_deleteextattr_args *ap, const char *attrname)
{
	znode_t *zp = VTOZ(ap->a_vp);
	nvlist_t *nvl;
	int error;

	error = zfs_ensure_xattr_cached(zp);
	if (error != 0)
		return (error);

	ASSERT(RW_WRITE_HELD(&zp->z_xattr_lock));
	ASSERT3P(zp->z_xattr_cached, !=, NULL);

	nvl = zp->z_xattr_cached;
	error = nvlist_remove(nvl, attrname, DATA_TYPE_BYTE_ARRAY);
	if (error != 0)
		error = SET_ERROR(error);
	else
		error = zfs_sa_set_xattr(zp, attrname, NULL, 0);
	if (error != 0) {
		zp->z_xattr_cached = NULL;
		nvlist_free(nvl);
	}
	return (error);
}

static int
zfs_deleteextattr_impl(struct vop_deleteextattr_args *ap, boolean_t compat)
{
	znode_t *zp = VTOZ(ap->a_vp);
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	char attrname[EXTATTR_MAXNAMELEN+1];
	int error;

	error = zfs_create_attrname(ap->a_attrnamespace, ap->a_name, attrname,
	    sizeof (attrname), compat);
	if (error != 0)
		return (error);

	error = ENOENT;
	if (zfsvfs->z_use_sa && zp->z_is_sa)
		error = zfs_deleteextattr_sa(ap, attrname);
	if (error == ENOENT)
		error = zfs_deleteextattr_dir(ap, attrname);
	return (error);
}

/*
 * Vnode operation to remove a named attribute.
 */
static int
zfs_netbsd_deleteextattr(void *v)
{
	struct vop_deleteextattr_args *ap = v;
	znode_t *zp = VTOZ(ap->a_vp);
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	int error;

	/*
	 * If the xattr property is off, refuse the request.
	 */
	if (!(zfsvfs->z_flags & ZSB_XATTR))
		return (SET_ERROR(EOPNOTSUPP));

	error = extattr_check_cred(ap->a_vp, ap->a_attrnamespace,
	    ap->a_cred, VWRITE);
	if (error != 0)
		return (SET_ERROR(error));

	error = zfs_check_attrname(ap->a_name);
	if (error != 0)
		return (error);

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);
	rw_enter(&zp->z_xattr_lock, RW_WRITER);

	error = zfs_deleteextattr_impl(ap, zfs_xattr_compat);
	if ((error == ENOENT || error == ENOATTR) &&
	    ap->a_attrnamespace == EXTATTR_NAMESPACE_USER) {
		/*
		 * Fall back to the alternate namespace format if we failed to
		 * find a user xattr.
		 */
		error = zfs_deleteextattr_impl(ap, !zfs_xattr_compat);
	}

	rw_exit(&zp->z_xattr_lock);
	zfs_exit(zfsvfs, FTAG);
	if (error == ENOENT)
		error = SET_ERROR(ENOATTR);
	return (error);
}

/*
 * The file is looked for before it is made, under the file's
 * z_xattr_lock held for writing, so that what zfs_create() hands back is
 * always a new vnode, and locked: zfs_create() makes only new names.
 */
static int
zfs_setextattr_dir(struct vop_setextattr_args *ap, const char *attrname)
{
	struct vattr va;
	vnode_t *xvp = NULL, *vp;
	znode_t *zp;
	int error;

	error = zfs_lookup(ap->a_vp, NULL, &xvp, NULL, 0, ap->a_cred,
	    LOOKUP_XATTR | CREATE_XATTR_DIR);
	if (error != 0)
		return (error);

	if (zfs_xattr_dotname(attrname)) {
		vrele(xvp);
		return (SET_ERROR(EINVAL));
	}
	vn_lock(xvp, LK_EXCLUSIVE | LK_RETRY);
	error = zfs_xattr_file(xvp, attrname, &vp);
	if (error == 0) {
		vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
	} else if (error == ENOENT) {
		vattr_null(&va);
		va.va_type = VREG;
		va.va_mode = 0600;
		vattr_init_mask(&va);
		error = zfs_create(VTOZ(xvp), attrname, &va, 0, 0600, &zp,
		    ap->a_cred, 0, NULL);
		if (error == 0)
			vp = ZTOV(zp);
	}
	vput(xvp);
	if (error != 0)
		return (error);

	vattr_null(&va);
	va.va_size = 0;
	error = VOP_SETATTR(vp, &va, ap->a_cred);
	if (error == 0)
		error = VOP_WRITE(vp, ap->a_uio, IO_UNIT, ap->a_cred);
	vput(vp);
	return (error);
}

static int
zfs_setextattr_sa(struct vop_setextattr_args *ap, const char *attrname)
{
	znode_t *zp = VTOZ(ap->a_vp);
	nvlist_t *nvl;
	size_t sa_size;
	int error;

	error = zfs_ensure_xattr_cached(zp);
	if (error != 0)
		return (error);

	ASSERT(RW_WRITE_HELD(&zp->z_xattr_lock));
	ASSERT3P(zp->z_xattr_cached, !=, NULL);

	nvl = zp->z_xattr_cached;
	size_t entry_size = ap->a_uio->uio_resid;
	if (entry_size > DXATTR_MAX_ENTRY_SIZE)
		return (SET_ERROR(EFBIG));
	error = nvlist_size(nvl, &sa_size, NV_ENCODE_XDR);
	if (error != 0)
		return (SET_ERROR(error));
	if (sa_size > DXATTR_MAX_SA_SIZE)
		return (SET_ERROR(EFBIG));
	uchar_t *buf = kmem_alloc(entry_size, KM_SLEEP);
	error = uiomove(buf, entry_size, ap->a_uio);
	if (error != 0) {
		error = SET_ERROR(error);
	} else {
		error = nvlist_add_byte_array(nvl, attrname, buf, entry_size);
		if (error != 0)
			error = SET_ERROR(error);
	}
	if (error == 0)
		error = zfs_sa_set_xattr(zp, attrname, buf, entry_size);
	kmem_free(buf, entry_size);
	if (error != 0) {
		zp->z_xattr_cached = NULL;
		nvlist_free(nvl);
	}
	return (error);
}

static int
zfs_setextattr_impl(struct vop_setextattr_args *ap, boolean_t compat)
{
	znode_t *zp = VTOZ(ap->a_vp);
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	char attrname[EXTATTR_MAXNAMELEN+1];
	int error;

	error = zfs_create_attrname(ap->a_attrnamespace, ap->a_name, attrname,
	    sizeof (attrname), compat);
	if (error != 0)
		return (error);

	struct vop_deleteextattr_args vda = {
		.a_vp = ap->a_vp,
		.a_attrnamespace = ap->a_attrnamespace,
		.a_name = ap->a_name,
		.a_cred = ap->a_cred,
	};
	error = ENOENT;
	if (zfsvfs->z_use_sa && zp->z_is_sa && zfsvfs->z_xattr_sa) {
		error = zfs_setextattr_sa(ap, attrname);
		if (error == 0) {
			/*
			 * Successfully put into SA, we need to clear the one
			 * in dir if present.
			 */
			zfs_deleteextattr_dir(&vda, attrname);
		}
	}
	if (error != 0) {
		error = zfs_setextattr_dir(ap, attrname);
		if (error == 0 && zp->z_is_sa) {
			/*
			 * Successfully put into dir, we need to clear the one
			 * in SA if present.
			 */
			zfs_deleteextattr_sa(&vda, attrname);
		}
	}
	if (error == 0 && ap->a_attrnamespace == EXTATTR_NAMESPACE_USER) {
		/*
		 * Also clear all versions of the alternate compat name.
		 */
		zfs_deleteextattr_impl(&vda, !compat);
	}
	return (error);
}

/*
 * Vnode operation to set a named attribute.
 */
static int
zfs_netbsd_setextattr(void *v)
{
	struct vop_setextattr_args *ap = v;
	znode_t *zp = VTOZ(ap->a_vp);
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	int error;

	/*
	 * If the xattr property is off, refuse the request.
	 */
	if (!(zfsvfs->z_flags & ZSB_XATTR))
		return (SET_ERROR(EOPNOTSUPP));

	error = extattr_check_cred(ap->a_vp, ap->a_attrnamespace,
	    ap->a_cred, VWRITE);
	if (error != 0)
		return (SET_ERROR(error));

	error = zfs_check_attrname(ap->a_name);
	if (error != 0)
		return (error);

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);
	rw_enter(&zp->z_xattr_lock, RW_WRITER);

	error = zfs_setextattr_impl(ap, zfs_xattr_compat);

	rw_exit(&zp->z_xattr_lock);
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

/*
 * One name of a list, in the form the caller asked for: a length byte
 * and the name, as extattr_list_file(2) returns them, or the name and a
 * NUL, as listxattr(2) does; NetBSD's FFS answers both the same way.
 */
static int
zfs_listextattr_name(struct vop_listextattr_args *ap, const char *name,
    uint8_t nlen)
{
	int error;

	if (ap->a_size != NULL) {
		*ap->a_size += 1 + nlen;
		return (0);
	}
	if (ap->a_uio == NULL)
		return (0);
	if (ap->a_flag & EXTATTR_LIST_LENPREFIX) {
		error = uiomove(&nlen, 1, ap->a_uio);
		if (error == 0)
			error = uiomove(__UNCONST(name), nlen, ap->a_uio);
	} else {
		error = uiomove(__UNCONST(name), nlen, ap->a_uio);
		if (error == 0)
			error = uiomove(__UNCONST(""), 1, ap->a_uio);
	}
	return (error != 0 ? SET_ERROR(error) : 0);
}

static int
zfs_listextattr_dir(struct vop_listextattr_args *ap, const char *attrprefix)
{
	uint8_t *dirbuf;
	struct iovec aiov;
	struct uio auio;
	vnode_t *xvp = NULL;
	int error, eof;

	error = zfs_lookup(ap->a_vp, NULL, &xvp, NULL, 0, ap->a_cred,
	    LOOKUP_XATTR);
	if (error != 0) {
		/*
		 * ENOATTR means that the EA directory does not yet exist,
		 * i.e. there are no extended attributes there.
		 */
		if (error == ENOATTR)
			error = 0;
		return (error);
	}

	dirbuf = kmem_alloc(sizeof (struct dirent), KM_SLEEP);
	auio.uio_iov = &aiov;
	auio.uio_iovcnt = 1;
	auio.uio_rw = UIO_READ;
	auio.uio_offset = 0;
	UIO_SETUP_SYSSPACE(&auio);

	size_t plen = strlen(attrprefix);

	vn_lock(xvp, LK_SHARED | LK_RETRY);
	do {
		aiov.iov_base = (void *)dirbuf;
		aiov.iov_len = sizeof (struct dirent);
		auio.uio_resid = sizeof (struct dirent);
		error = VOP_READDIR(xvp, &auio, ap->a_cred, &eof, NULL, NULL);
		if (error != 0)
			break;
		int done = sizeof (struct dirent) - auio.uio_resid;
		if (done == 0)
			break;
		for (int pos = 0; pos < done; ) {
			struct dirent *dp = (struct dirent *)(dirbuf + pos);
			pos += dp->d_reclen;
			/*
			 * XXX: Temporarily we also accept DT_UNKNOWN, as this
			 * is what we get when attribute was created on Solaris.
			 */
			if (dp->d_type != DT_REG && dp->d_type != DT_UNKNOWN)
				continue;
			else if (plen == 0 &&
			    ZFS_XA_NS_PREFIX_FORBIDDEN(dp->d_name))
				continue;
			else if (strncmp(dp->d_name, attrprefix, plen) != 0)
				continue;
			error = zfs_listextattr_name(ap, dp->d_name + plen,
			    dp->d_namlen - plen);
			if (error != 0)
				break;
		}
	} while (!eof && error == 0);
	vput(xvp);
	kmem_free(dirbuf, sizeof (struct dirent));
	return (error);
}

static int
zfs_listextattr_sa(struct vop_listextattr_args *ap, const char *attrprefix)
{
	znode_t *zp = VTOZ(ap->a_vp);
	int error;

	error = zfs_ensure_xattr_cached(zp);
	if (error != 0)
		return (error);

	ASSERT(RW_LOCK_HELD(&zp->z_xattr_lock));
	ASSERT3P(zp->z_xattr_cached, !=, NULL);

	size_t plen = strlen(attrprefix);
	nvpair_t *nvp = NULL;
	while ((nvp = nvlist_next_nvpair(zp->z_xattr_cached, nvp)) != NULL) {
		ASSERT3U(nvpair_type(nvp), ==, DATA_TYPE_BYTE_ARRAY);

		const char *name = nvpair_name(nvp);
		if (plen == 0 && ZFS_XA_NS_PREFIX_FORBIDDEN(name))
			continue;
		else if (strncmp(name, attrprefix, plen) != 0)
			continue;
		error = zfs_listextattr_name(ap, name + plen,
		    strlen(name) - plen);
		if (error != 0)
			break;
	}

	return (error);
}

static int
zfs_listextattr_impl(struct vop_listextattr_args *ap, boolean_t compat)
{
	znode_t *zp = VTOZ(ap->a_vp);
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	char attrprefix[16];
	int error;

	error = zfs_create_attrname(ap->a_attrnamespace, "", attrprefix,
	    sizeof (attrprefix), compat);
	if (error != 0)
		return (error);

	if (zfsvfs->z_use_sa && zp->z_is_sa)
		error = zfs_listextattr_sa(ap, attrprefix);
	if (error == 0)
		error = zfs_listextattr_dir(ap, attrprefix);
	return (error);
}

/*
 * Vnode operation to retrieve extended attributes on a vnode.
 */
static int
zfs_netbsd_listextattr(void *v)
{
	struct vop_listextattr_args *ap = v;
	znode_t *zp = VTOZ(ap->a_vp);
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	int error;

	if (ap->a_size != NULL)
		*ap->a_size = 0;

	/*
	 * If the xattr property is off, refuse the request.
	 */
	if (!(zfsvfs->z_flags & ZSB_XATTR))
		return (SET_ERROR(EOPNOTSUPP));

	error = extattr_check_cred(ap->a_vp, ap->a_attrnamespace,
	    ap->a_cred, VREAD);
	if (error != 0)
		return (SET_ERROR(error));

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);
	rw_enter(&zp->z_xattr_lock, RW_READER);

	error = zfs_listextattr_impl(ap, zfs_xattr_compat);
	if (error == 0 && ap->a_attrnamespace == EXTATTR_NAMESPACE_USER) {
		/* Also list user xattrs with the alternate format. */
		error = zfs_listextattr_impl(ap, !zfs_xattr_compat);
	}

	rw_exit(&zp->z_xattr_lock);
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

ZFS_MODULE_PARAM(zfs, zfs_, xattr_compat, INT, ZMOD_RW,
	"Use legacy ZFS xattr naming for writing new user namespace xattrs");
/*
 * The VOP entry points.
 */

static int
zfs_netbsd_lookup(void *v)
{
	struct vop_lookup_v2_args *ap = v;
	struct componentname *cnp = ap->a_cnp;
	vnode_t *dvp = ap->a_dvp;
	char nm[NAME_MAX + 1];
	int error;

	*ap->a_vpp = NULL;
	if (cnp->cn_namelen >= sizeof (nm))
		return (SET_ERROR(ENAMETOOLONG));
	/*
	 * A last component that is to be removed or renamed must be in a
	 * directory that can be written.
	 */
	if ((cnp->cn_flags & ISLASTCN) &&
	    (dvp->v_mount->mnt_flag & MNT_RDONLY) &&
	    (cnp->cn_nameiop == DELETE || cnp->cn_nameiop == RENAME))
		return (SET_ERROR(EROFS));
	/* The name cache answers first, as for NetBSD's own. */
	if (cache_lookup(dvp, cnp->cn_nameptr, cnp->cn_namelen,
	    cnp->cn_nameiop, cnp->cn_flags, NULL, ap->a_vpp)) {
		return (*ap->a_vpp == NULL ? SET_ERROR(ENOENT) : 0);
	}
	(void) strlcpy(nm, cnp->cn_nameptr, cnp->cn_namelen + 1);
	error = zfs_lookup(dvp, nm, ap->a_vpp, cnp, cnp->cn_nameiop,
	    cnp->cn_cred, 0);
	return (error);
}

/*
 * The name as a string: a componentname's runs on to the rest of the
 * path.
 */
static char *
zfs_cnp_name(struct componentname *cnp)
{
	char *nm;

	nm = kmem_alloc(cnp->cn_namelen + 1, KM_SLEEP);
	(void) strlcpy(nm, cnp->cn_nameptr, cnp->cn_namelen + 1);
	return (nm);
}

static void
zfs_cnp_name_free(struct componentname *cnp, char *nm)
{

	kmem_free(nm, cnp->cn_namelen + 1);
}

/*
 * Create, mknod, mkdir and symlink return the new vnode referenced and
 * unlocked; zfs_mknode() gave it to them locked.
 */
static int
zfs_netbsd_create(void *v)
{
	struct vop_create_v3_args *ap = v;
	struct componentname *cnp = ap->a_cnp;
	vnode_t *dvp = ap->a_dvp;
	vattr_t *vap = ap->a_vap;
	zfsvfs_t *zfsvfs = dvp->v_mount->mnt_data;
	znode_t *zp = NULL;
	char *nm;
	int error;

	vattr_init_mask(vap);
	*ap->a_vpp = NULL;
	nm = zfs_cnp_name(cnp);
	error = zfs_create(VTOZ(dvp), nm, vap, 0, vap->va_mode & ALLPERMS,
	    &zp, cnp->cn_cred, 0, NULL);
	if (error == 0) {
		*ap->a_vpp = ZTOV(zp);
		VOP_UNLOCK(*ap->a_vpp);
		if (zfsvfs->z_use_namecache)
			cache_enter(dvp, *ap->a_vpp, cnp->cn_nameptr,
			    cnp->cn_namelen, cnp->cn_flags);
	}
	zfs_cnp_name_free(cnp, nm);
	return (error);
}

static int
zfs_netbsd_mknod(void *v)
{
	struct vop_mknod_v3_args *ap = v;
	vattr_t *vap = ap->a_vap;
	enum vtype vt = vap->va_type;

	if (vt != VBLK && vt != VCHR && vt != VFIFO && vt != VSOCK) {
		*ap->a_vpp = NULL;
		return (SET_ERROR(EINVAL));
	}
	return (zfs_netbsd_create(v));
}

static int
zfs_netbsd_mkdir(void *v)
{
	struct vop_mkdir_v3_args *ap = v;
	struct componentname *cnp = ap->a_cnp;
	vattr_t *vap = ap->a_vap;
	znode_t *zp = NULL;
	char *nm;
	int error;

	vattr_init_mask(vap);
	*ap->a_vpp = NULL;
	nm = zfs_cnp_name(cnp);
	error = zfs_mkdir(VTOZ(ap->a_dvp), nm, vap, &zp, cnp->cn_cred, 0,
	    NULL);
	if (error == 0) {
		*ap->a_vpp = ZTOV(zp);
		VOP_UNLOCK(*ap->a_vpp);
	}
	zfs_cnp_name_free(cnp, nm);
	return (error);
}

static int
zfs_netbsd_symlink(void *v)
{
	struct vop_symlink_v3_args *ap = v;
	struct componentname *cnp = ap->a_cnp;
	vattr_t *vap = ap->a_vap;
	znode_t *zp = NULL;
	char *nm;
	int error;

	vattr_init_mask(vap);
	vap->va_type = VLNK;
	*ap->a_vpp = NULL;
	nm = zfs_cnp_name(cnp);
	error = zfs_symlink(VTOZ(ap->a_dvp), nm, vap, ap->a_target, &zp,
	    cnp->cn_cred, 0);
	if (error == 0) {
		*ap->a_vpp = ZTOV(zp);
		VOP_UNLOCK(*ap->a_vpp);
	}
	zfs_cnp_name_free(cnp, nm);
	return (error);
}

/* Remove and rmdir: dvp and vp come locked; vp goes back put. */
static int
zfs_netbsd_remove(void *v)
{
	struct vop_remove_v3_args *ap = v;
	struct componentname *cnp = ap->a_cnp;
	vnode_t *vp = ap->a_vp;
	char *nm;
	int error;

	if (vp->v_type == VDIR) {
		error = SET_ERROR(EPERM);
	} else {
		nm = zfs_cnp_name(cnp);
		error = zfs_remove_(ap->a_dvp, vp, nm, cnp->cn_cred);
		zfs_cnp_name_free(cnp, nm);
		if (error == 0)
			cache_purge(vp);
	}
	ap->ctx_vp_new_nlink = (error == 0) ? VTOZ(vp)->z_links : 0;
	vput(vp);
	return (error);
}

static int
zfs_netbsd_rmdir(void *v)
{
	struct vop_rmdir_v2_args *ap = v;
	struct componentname *cnp = ap->a_cnp;
	vnode_t *vp = ap->a_vp;
	char *nm;
	int error;

	nm = zfs_cnp_name(cnp);
	error = zfs_rmdir_(ap->a_dvp, vp, nm, cnp->cn_cred);
	zfs_cnp_name_free(cnp, nm);
	vput(vp);
	return (error);
}

static int
zfs_netbsd_link(void *v)
{
	struct vop_link_v2_args *ap = v;
	struct componentname *cnp = ap->a_cnp;
	vnode_t *vp = ap->a_vp, *tdvp = ap->a_dvp;
	char *nm;
	int error;

	if (tdvp->v_mount != vp->v_mount)
		return (SET_ERROR(EXDEV));
	if ((error = vn_lock(vp, LK_EXCLUSIVE)) != 0)
		return (error);
	nm = zfs_cnp_name(cnp);
	error = zfs_link(VTOZ(tdvp), VTOZ(vp), nm, cnp->cn_cred, 0);
	zfs_cnp_name_free(cnp, nm);
	VOP_UNLOCK(vp);
	return (error);
}

/*
 * The source directory and file come referenced and unlocked, the
 * target directory and file (if any) referenced and locked, as on
 * FreeBSD, whose zfs_do_rename() locks all four itself.
 */
static int
zfs_netbsd_rename(void *v)
{
	struct vop_rename_args *ap = v;
	vnode_t *fdvp = ap->a_fdvp, *fvp = ap->a_fvp;
	vnode_t *tdvp = ap->a_tdvp, *tvp = ap->a_tvp;
	struct componentname *fcnp = ap->a_fcnp, *tcnp = ap->a_tcnp;
	char *fnm, *tnm;
	int error;

	/* fcnp and tcnp name their components; ZFS wants strings. */
	fnm = zfs_cnp_name(fcnp);
	tnm = zfs_cnp_name(tcnp);
	{
		struct componentname fcn = *fcnp, tcn = *tcnp;

		fcn.cn_nameptr = fnm;
		tcn.cn_nameptr = tnm;
		error = zfs_do_rename(fdvp, &fvp, &fcn, tdvp, &tvp, &tcn,
		    fcnp->cn_cred);
	}
	zfs_cnp_name_free(fcnp, fnm);
	zfs_cnp_name_free(tcnp, tnm);
	vrele(fdvp);
	vrele(fvp);
	vrele(tdvp);
	if (tvp != NULL)
		vrele(tvp);
	return (error);
}

static int
zfs_netbsd_open(void *v)
{
	struct vop_open_args *ap = v;
	vnode_t *vp = ap->a_vp;

	return (zfs_open(&vp, ap->a_mode, ap->a_cred));
}

static int
zfs_netbsd_close(void *v)
{
	struct vop_close_args *ap = v;

	return (zfs_close(ap->a_vp, ap->a_fflag, 1, 0, ap->a_cred));
}

/*
 * Read, write, execute and append are the ACL's to answer; the rest,
 * which is administering the file, is its owner's, as genfs decides.
 * A file that is not a directory can be executed only if some execute
 * bit is set, even by the superuser.
 */
static int
zfs_netbsd_access(void *v)
{
	struct vop_access_args *ap = v;
	vnode_t *vp = ap->a_vp;
	znode_t *zp = VTOZ(vp);
	accmode_t accmode = ap->a_accmode;
	int error = 0;

	if (accmode & (VREAD | VWRITE | VEXEC | VAPPEND))
		error = zfs_access(zp, accmode & (VREAD | VWRITE | VEXEC |
		    VAPPEND), 0, ap->a_cred);
	if (error == 0 && (accmode & ~(VREAD | VWRITE | VEXEC | VAPPEND))) {
		accmode_t rest = accmode & ~(VREAD | VWRITE | VEXEC | VAPPEND);

		error = kauth_authorize_vnode(ap->a_cred,
		    KAUTH_ACCESS_ACTION(rest, vp->v_type, zp->z_mode), vp,
		    NULL, genfs_can_access(vp, ap->a_cred, zp->z_uid,
		    zp->z_gid, zp->z_mode, NULL, rest));
	}
	if (error == 0 && (accmode & VEXEC) && vp->v_type != VDIR &&
	    (zp->z_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0)
		error = SET_ERROR(EACCES);
	return (error);
}

static int
zfs_netbsd_read(void *v)
{
	struct vop_read_args *ap = v;
	vnode_t *vp = ap->a_vp;
	zfs_uio_t uio;

	switch (vp->v_type) {
	case VREG:
		break;
	case VDIR:
		return (SET_ERROR(EISDIR));
	default:
		return (SET_ERROR(EINVAL));
	}
	zfs_uio_init(&uio, ap->a_uio);
	return (zfs_read(VTOZ(vp), &uio, ioflags(ap->a_ioflag), ap->a_cred));
}

static int
zfs_netbsd_write(void *v)
{
	struct vop_write_args *ap = v;
	vnode_t *vp = ap->a_vp;
	zfs_uio_t uio;

	if (vp->v_type != VREG)
		return (SET_ERROR(EINVAL));
	zfs_uio_init(&uio, ap->a_uio);
	return (zfs_write(VTOZ(vp), &uio, ioflags(ap->a_ioflag),
	    ap->a_cred));
}

/*
 * A mapping's dirty pages go into the DMU first, and then the intent
 * log is committed, unless the vnode is being reclaimed or the syncer
 * is asking: the data then only has to reach a txg, and committing the
 * log from a reclaim can look the vnode up in the cache, which waits
 * for this reclaim to finish.
 */
static int
zfs_netbsd_fsync(void *v)
{
	struct vop_fsync_args *ap = v;
	vnode_t *vp = ap->a_vp;
	int flags = ap->a_flags;
	int error;

	rw_enter(vp->v_uobj.vmobjlock, RW_WRITER);
	error = VOP_PUTPAGES(vp, trunc_page(ap->a_offlo),
	    round_page(ap->a_offhi), PGO_CLEANIT |
	    ((flags & FSYNC_WAIT) ? PGO_SYNCIO : 0));
	if (error != 0)
		return (error);
	if (flags & (FSYNC_RECLAIM | FSYNC_LAZY))
		return (0);
	return (zfs_fsync(VTOZ(vp), 0, ap->a_cred));
}

static int
zfs_netbsd_getattr(void *v)
{
	struct vop_getattr_args *ap = v;
	vattr_t *vap = ap->a_vap;
	xvattr_t xvap;
	u_long fflags = 0;
	int error;

	xva_init(&xvap);
	xvap.xva_vattr = *vap;
	xvap.xva_vattr.va_mask |= AT_XVATTR;
	XVA_SET_REQ(&xvap, XAT_IMMUTABLE);
	XVA_SET_REQ(&xvap, XAT_APPENDONLY);
	XVA_SET_REQ(&xvap, XAT_NODUMP);
	XVA_SET_REQ(&xvap, XAT_ARCHIVE);
	error = zfs_getattr(ap->a_vp, (vattr_t *)&xvap, 0, ap->a_cred);
	if (error != 0)
		return (error);

	/* The ZFS attributes NetBSD has chflags(2) flags for. */
#define	FLAG_CHECK(fflag, xflag, xfield)	do {			\
	if (XVA_ISSET_RTN(&xvap, (xflag)) && (xfield) != 0)		\
		fflags |= (fflag);					\
} while (0)
	FLAG_CHECK(SF_IMMUTABLE, XAT_IMMUTABLE,
	    xvap.xva_xoptattrs.xoa_immutable);
	FLAG_CHECK(SF_APPEND, XAT_APPENDONLY,
	    xvap.xva_xoptattrs.xoa_appendonly);
	FLAG_CHECK(SF_ARCHIVED, XAT_ARCHIVE,
	    xvap.xva_xoptattrs.xoa_archive);
	FLAG_CHECK(UF_NODUMP, XAT_NODUMP,
	    xvap.xva_xoptattrs.xoa_nodump);
#undef	FLAG_CHECK
	*vap = xvap.xva_vattr;
	vap->va_flags = fflags;
	return (0);
}

/* The chflags(2) flags that have ZFS attributes to keep them in. */
#define	ZFS_NETBSD_FLAGS	\
	(SF_IMMUTABLE | SF_APPEND | SF_ARCHIVED | UF_NODUMP)

static int
zfs_netbsd_setattr(void *v)
{
	struct vop_setattr_args *ap = v;
	vnode_t *vp = ap->a_vp;
	vattr_t *vap = ap->a_vap;
	cred_t *cred = ap->a_cred;
	znode_t *zp = VTOZ(vp);
	xvattr_t xvap;
	uint64_t zflags = zp->z_pflags;
	int error;

	/* What cannot be set. */
	if (vap->va_type != VNON || vap->va_nlink != (nlink_t)VNOVAL ||
	    vap->va_fsid != VNOVAL || vap->va_fileid != VNOVAL ||
	    vap->va_blocksize != VNOVAL || vap->va_rdev != (dev_t)VNOVAL ||
	    vap->va_bytes != (u_quad_t)VNOVAL || vap->va_gen != VNOVAL)
		return (SET_ERROR(EINVAL));

	vattr_init_mask(vap);
	vap->va_mask &= ~AT_NOSET;
	/*
	 * vn_open() sets the size to 0 for O_TRUNC whatever the vnode
	 * is, and zfs_setattr() refuses a size on a device with EINVAL,
	 * so "> /dev/null" failed on a root on ZFS.  ufs_setattr() leaves
	 * a device's size alone; so does this.
	 */
	if (vp->v_type == VCHR || vp->v_type == VBLK)
		vap->va_mask &= ~AT_SIZE;
	xva_init(&xvap);
	xvap.xva_vattr = *vap;

	if (vap->va_flags != VNOVAL) {
		u_long fflags = vap->va_flags, cur = 0;
		kauth_action_t action = KAUTH_VNODE_WRITE_FLAGS;

		if (zp->z_zfsvfs->z_use_fuids == B_FALSE)
			return (SET_ERROR(EOPNOTSUPP));
		if ((fflags & ~ZFS_NETBSD_FLAGS) != 0)
			return (SET_ERROR(EOPNOTSUPP));
		if (zflags & ZFS_IMMUTABLE)
			cur |= SF_IMMUTABLE;
		if (zflags & ZFS_APPENDONLY)
			cur |= SF_APPEND;
		if (zflags & ZFS_ARCHIVE)
			cur |= SF_ARCHIVED;
		/* As ufs_setattr() asks it. */
		if ((fflags & SF_SETTABLE) != (cur & SF_SETTABLE))
			action |= KAUTH_VNODE_WRITE_SYSFLAGS;
		if (cur & (SF_IMMUTABLE | SF_APPEND))
			action |= KAUTH_VNODE_HAS_SYSFLAGS;
		error = kauth_authorize_vnode(cred, action, vp, NULL,
		    genfs_can_chflags(vp, cred, zp->z_uid,
		    (action & KAUTH_VNODE_WRITE_SYSFLAGS) != 0));
		if (error != 0)
			return (error);

#define	FLAG_CHANGE(fflag, zflag, xflag, xfield)	do {		\
	if (((fflags & (fflag)) && !(zflags & (zflag))) ||		\
	    ((zflags & (zflag)) && !(fflags & (fflag)))) {		\
		XVA_SET_REQ(&xvap, (xflag));				\
		(xfield) = ((fflags & (fflag)) != 0);			\
	}								\
} while (0)
		FLAG_CHANGE(SF_IMMUTABLE, ZFS_IMMUTABLE, XAT_IMMUTABLE,
		    xvap.xva_xoptattrs.xoa_immutable);
		FLAG_CHANGE(SF_APPEND, ZFS_APPENDONLY, XAT_APPENDONLY,
		    xvap.xva_xoptattrs.xoa_appendonly);
		FLAG_CHANGE(SF_ARCHIVED, ZFS_ARCHIVE, XAT_ARCHIVE,
		    xvap.xva_xoptattrs.xoa_archive);
		FLAG_CHANGE(UF_NODUMP, ZFS_NODUMP, XAT_NODUMP,
		    xvap.xva_xoptattrs.xoa_nodump);
#undef	FLAG_CHANGE
		xvap.xva_vattr.va_mask |= AT_XVATTR;
	}
	if (vap->va_birthtime.tv_sec != VNOVAL) {
		xvap.xva_vattr.va_mask |= AT_XVATTR;
		XVA_SET_REQ(&xvap, XAT_CREATETIME);
	}
	return (zfs_setattr(zp, (vattr_t *)&xvap, 0, cred));
}

static int
zfs_netbsd_readdir(void *v)
{
	struct vop_readdir_args *ap = v;
	zfs_uio_t uio;

	zfs_uio_init(&uio, ap->a_uio);
	return (zfs_readdir(ap->a_vp, &uio, ap->a_cred, ap->a_eofflag,
	    ap->a_cookies, ap->a_ncookies));
}

static int
zfs_netbsd_readlink(void *v)
{
	struct vop_readlink_args *ap = v;
	zfs_uio_t uio;

	zfs_uio_init(&uio, ap->a_uio);
	return (zfs_readlink(ap->a_vp, &uio, ap->a_cred, NULL));
}

/*
 * The last reference is going: the access time is written, and a file
 * that has no links, or no longer has an object, is recycled now
 * rather than kept in the cache.
 */
static int
zfs_netbsd_inactive(void *v)
{
	struct vop_inactive_v2_args *ap = v;
	vnode_t *vp = ap->a_vp;
	znode_t *zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;

	zfs_inactive(vp, kcred, NULL);
	ZFS_TEARDOWN_INACTIVE_ENTER_READ(zfsvfs);
	*ap->a_recycle = (zp->z_sa_hdl == NULL || zp->z_unlinked);
	ZFS_TEARDOWN_INACTIVE_EXIT_READ(zfsvfs);
	return (0);
}

/*
 * vcache has flushed the pages; the znode goes, and with it the file if
 * it was unlinked.  z_teardown_inactive_lock keeps zfsvfs_teardown()
 * from finishing the znode under it.  The vnode comes locked and goes
 * back unlocked, as NetBSD's reclaim does; it stays locked until the
 * znode is gone, since removing a file expects the lock (zfs_rmnode()).
 * The genfs node lives in the znode, so it goes first.
 */
static int
zfs_netbsd_reclaim(void *v)
{
	struct vop_reclaim_v2_args *ap = v;
	vnode_t *vp = ap->a_vp;
	znode_t *zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;

	genfs_node_destroy(vp);
	ZFS_TEARDOWN_INACTIVE_ENTER_READ(zfsvfs);
	if (zp->z_sa_hdl == NULL)
		zfs_znode_free(zp);
	else
		zfs_zinactive(zp);
	ZFS_TEARDOWN_INACTIVE_EXIT_READ(zfsvfs);
	vp->v_data = NULL;
	VOP_UNLOCK(vp);
	return (0);
}

static int
zfs_netbsd_pathconf(void *v)
{
	struct vop_pathconf_args *ap = v;
	ulong_t val;
	int error;

	switch (ap->a_name) {
	case _PC_NAME_MAX:
		*ap->a_retval = VTOZ(ap->a_vp)->z_zfsvfs->z_longname ?
		    ZAP_MAXNAMELEN_NEW - 1 : NAME_MAX;
		return (0);
	case _PC_PATH_MAX:
		*ap->a_retval = PATH_MAX;
		return (0);
	case _PC_PIPE_BUF:
		*ap->a_retval = PIPE_BUF;
		return (0);
	case _PC_CHOWN_RESTRICTED:
		*ap->a_retval = 1;
		return (0);
	case _PC_NO_TRUNC:
		*ap->a_retval = 1;
		return (0);
	case _PC_SYNC_IO:
		*ap->a_retval = 1;
		return (0);
	}
	error = zfs_pathconf(ap->a_vp, ap->a_name, &val, curlwp->l_cred,
	    NULL);
	if (error == 0)
		*ap->a_retval = val;
	else if (error == EOPNOTSUPP)
		error = genfs_pathconf(v);
	return (error);
}

static int
zfs_netbsd_advlock(void *v)
{
	struct vop_advlock_args *ap = v;
	znode_t *zp = VTOZ(ap->a_vp);

	return (lf_advlock(ap, &zp->z_lockf, zp->z_size));
}

static int
zfs_netbsd_ioctl(void *v)
{
	struct vop_ioctl_args *ap = v;

	return (zfs_ioctl(ap->a_vp, ap->a_command, (intptr_t)ap->a_data,
	    ap->a_fflag, ap->a_cred, NULL));
}

/*
 * NFSv4 ACLs, which NetBSD's struct acl carries as FreeBSD's does; the
 * POSIX.1e kind ZFS does not keep.
 */
static int
zfs_netbsd_getacl(void *v)
{
	struct vop_getacl_args *ap = v;
	vsecattr_t vsecattr;
	int error;

	if (ap->a_type != ACL_TYPE_NFS4)
		return (SET_ERROR(EINVAL));
	vsecattr.vsa_mask = VSA_ACE | VSA_ACECNT;
	if ((error = zfs_getsecattr(VTOZ(ap->a_vp), &vsecattr, 0,
	    ap->a_cred)) != 0)
		return (error);
	error = acl_from_aces(ap->a_aclp, vsecattr.vsa_aclentp,
	    vsecattr.vsa_aclcnt);
	if (vsecattr.vsa_aclentp != NULL)
		kmem_free(vsecattr.vsa_aclentp, vsecattr.vsa_aclentsz);
	return (error);
}

static int
zfs_netbsd_setacl(void *v)
{
	struct vop_setacl_args *ap = v;
	vsecattr_t vsecattr;
	int aclbsize, error;
	void *aaclp;

	if (ap->a_type != ACL_TYPE_NFS4)
		return (SET_ERROR(EINVAL));
	if (ap->a_aclp == NULL)
		return (SET_ERROR(EINVAL));
	if (ap->a_aclp->acl_cnt < 1 || ap->a_aclp->acl_cnt > MAX_ACL_ENTRIES)
		return (SET_ERROR(EINVAL));
	/*
	 * chmod(2) may split each entry in two and add six, which must
	 * still fit.
	 */
	if (ap->a_aclp->acl_cnt * 2 + 6 > ACL_MAX_ENTRIES)
		return (SET_ERROR(ENOSPC));
	if ((error = acl_nfs4_check(ap->a_aclp,
	    ap->a_vp->v_type == VDIR)) != 0)
		return (error);

	vsecattr.vsa_mask = VSA_ACE;
	aclbsize = ap->a_aclp->acl_cnt * sizeof (ace_t);
	aaclp = vsecattr.vsa_aclentp = kmem_alloc(aclbsize, KM_SLEEP);
	vsecattr.vsa_aclentsz = aclbsize;
	aces_from_acl(vsecattr.vsa_aclentp, &vsecattr.vsa_aclcnt, ap->a_aclp);
	error = zfs_setsecattr(VTOZ(ap->a_vp), &vsecattr, 0, ap->a_cred);
	kmem_free(aaclp, aclbsize);
	return (error);
}

static int
zfs_netbsd_aclcheck(void *v)
{

	return (SET_ERROR(EOPNOTSUPP));
}

static int
zfs_netbsd_print(void *v)
{
	struct vop_print_args *ap = v;
	znode_t *zp = VTOZ(ap->a_vp);

	printf("\tobject %llu size %llu links %llu%s\n",
	    (u_longlong_t)zp->z_id, (u_longlong_t)zp->z_size,
	    (u_longlong_t)zp->z_links, zp->z_unlinked ? " unlinked" : "");
	return (0);
}

/*
 * Device and FIFO nodes: their own I/O, and the ZFS attributes, the
 * life of the znode and an fsync that commits the log.
 */
static int
zfs_spec_fsync(void *v)
{
	struct vop_fsync_args *ap = v;
	int error;

	if ((error = spec_fsync(v)) != 0)
		return (error);
	if (ap->a_flags & (FSYNC_RECLAIM | FSYNC_LAZY))
		return (0);
	return (zfs_fsync(VTOZ(ap->a_vp), 0, ap->a_cred));
}

const struct vnodeopv_entry_desc zfs_vnodeop_entries[] = {
	{ &vop_default_desc,		vn_default_error },
	{ &vop_parsepath_desc,		genfs_parsepath },
	{ &vop_lookup_desc,		zfs_netbsd_lookup },
	{ &vop_create_desc,		zfs_netbsd_create },
	{ &vop_mknod_desc,		zfs_netbsd_mknod },
	{ &vop_open_desc,		zfs_netbsd_open },
	{ &vop_close_desc,		zfs_netbsd_close },
	{ &vop_access_desc,		zfs_netbsd_access },
	{ &vop_accessx_desc,		genfs_accessx },
	{ &vop_getattr_desc,		zfs_netbsd_getattr },
	{ &vop_setattr_desc,		zfs_netbsd_setattr },
	{ &vop_read_desc,		zfs_netbsd_read },
	{ &vop_write_desc,		zfs_netbsd_write },
	{ &vop_fallocate_desc,		genfs_eopnotsupp },
	{ &vop_fdiscard_desc,		genfs_eopnotsupp },
	{ &vop_ioctl_desc,		zfs_netbsd_ioctl },
	{ &vop_fcntl_desc,		genfs_fcntl },
	{ &vop_poll_desc,		genfs_poll },
	{ &vop_kqfilter_desc,		genfs_kqfilter },
	{ &vop_revoke_desc,		genfs_revoke },
	{ &vop_mmap_desc,		genfs_mmap },
	{ &vop_fsync_desc,		zfs_netbsd_fsync },
	{ &vop_seek_desc,		genfs_seek },
	{ &vop_remove_desc,		zfs_netbsd_remove },
	{ &vop_link_desc,		zfs_netbsd_link },
	{ &vop_rename_desc,		zfs_netbsd_rename },
	{ &vop_mkdir_desc,		zfs_netbsd_mkdir },
	{ &vop_rmdir_desc,		zfs_netbsd_rmdir },
	{ &vop_symlink_desc,		zfs_netbsd_symlink },
	{ &vop_readdir_desc,		zfs_netbsd_readdir },
	{ &vop_readlink_desc,		zfs_netbsd_readlink },
	{ &vop_abortop_desc,		genfs_abortop },
	{ &vop_inactive_desc,		zfs_netbsd_inactive },
	{ &vop_reclaim_desc,		zfs_netbsd_reclaim },
	{ &vop_lock_desc,		genfs_lock },
	{ &vop_unlock_desc,		genfs_unlock },
	{ &vop_bmap_desc,		genfs_eopnotsupp },
	{ &vop_strategy_desc,		genfs_eopnotsupp },
	{ &vop_print_desc,		zfs_netbsd_print },
	{ &vop_pathconf_desc,		zfs_netbsd_pathconf },
	{ &vop_islocked_desc,		genfs_islocked },
	{ &vop_advlock_desc,		zfs_netbsd_advlock },
	{ &vop_bwrite_desc,		genfs_nullop },
	{ &vop_getpages_desc,		zfs_getpages },
	{ &vop_putpages_desc,		genfs_putpages },
	{ &vop_getacl_desc,		zfs_netbsd_getacl },
	{ &vop_setacl_desc,		zfs_netbsd_setacl },
	{ &vop_aclcheck_desc,		zfs_netbsd_aclcheck },
	{ &vop_getextattr_desc,		zfs_netbsd_getextattr },
	{ &vop_setextattr_desc,		zfs_netbsd_setextattr },
	{ &vop_listextattr_desc,		zfs_netbsd_listextattr },
	{ &vop_deleteextattr_desc,	zfs_netbsd_deleteextattr },
	{ NULL, NULL }
};

const struct vnodeopv_desc zfs_vnodeop_opv_desc =
	{ &zfs_vnodeop_p, zfs_vnodeop_entries };

const struct vnodeopv_entry_desc zfs_specop_entries[] = {
	{ &vop_default_desc,		vn_default_error },
	GENFS_SPECOP_ENTRIES,
	{ &vop_close_desc,		spec_close },
	{ &vop_access_desc,		zfs_netbsd_access },
	{ &vop_accessx_desc,		genfs_accessx },
	{ &vop_getattr_desc,		zfs_netbsd_getattr },
	{ &vop_setattr_desc,		zfs_netbsd_setattr },
	{ &vop_read_desc,		spec_read },
	{ &vop_write_desc,		spec_write },
	{ &vop_fcntl_desc,		genfs_fcntl },
	{ &vop_fsync_desc,		zfs_spec_fsync },
	{ &vop_inactive_desc,		zfs_netbsd_inactive },
	{ &vop_reclaim_desc,		zfs_netbsd_reclaim },
	{ &vop_lock_desc,		genfs_lock },
	{ &vop_unlock_desc,		genfs_unlock },
	{ &vop_print_desc,		zfs_netbsd_print },
	{ &vop_islocked_desc,		genfs_islocked },
	{ &vop_bwrite_desc,		vn_bwrite },
	{ NULL, NULL }
};

const struct vnodeopv_desc zfs_specop_opv_desc =
	{ &zfs_specop_p, zfs_specop_entries };

const struct vnodeopv_entry_desc zfs_fifoop_entries[] = {
	{ &vop_default_desc,		vn_default_error },
	GENFS_FIFOOP_ENTRIES,
	{ &vop_close_desc,		vn_fifo_bypass },
	{ &vop_access_desc,		zfs_netbsd_access },
	{ &vop_accessx_desc,		genfs_accessx },
	{ &vop_getattr_desc,		zfs_netbsd_getattr },
	{ &vop_setattr_desc,		zfs_netbsd_setattr },
	{ &vop_read_desc,		vn_fifo_bypass },
	{ &vop_write_desc,		vn_fifo_bypass },
	{ &vop_fcntl_desc,		genfs_fcntl },
	{ &vop_fsync_desc,		zfs_netbsd_fsync },
	{ &vop_inactive_desc,		zfs_netbsd_inactive },
	{ &vop_reclaim_desc,		zfs_netbsd_reclaim },
	{ &vop_lock_desc,		genfs_lock },
	{ &vop_unlock_desc,		genfs_unlock },
	{ &vop_strategy_desc,		vn_fifo_bypass },
	{ &vop_print_desc,		zfs_netbsd_print },
	{ &vop_islocked_desc,		genfs_islocked },
	{ &vop_bwrite_desc,		genfs_nullop },
	{ NULL, NULL }
};

const struct vnodeopv_desc zfs_fifoop_opv_desc =
	{ &zfs_fifoop_p, zfs_fifoop_entries };
