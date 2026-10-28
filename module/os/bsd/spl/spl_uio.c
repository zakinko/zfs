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
 * Moving data through a zfs_uio_t, over NetBSD's uiomove(9), which
 * copies in or out of the address space the uio's vmspace names.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kmem.h>
#include <sys/uio.h>
#include <sys/uio_impl.h>
#include <sys/debug.h>
#include <sys/zfs_debug.h>
#include <uvm/uvm_extern.h>

void
zfs_uio_init(zfs_uio_t *uio, struct uio *uio_s)
{

	memset(uio, 0, sizeof (zfs_uio_t));
	if (uio_s != NULL) {
		uio->uio = uio_s;
		zfs_uio_soffset(uio) = uio_s->uio_offset;
		zfs_uio_segflg(uio) =
		    VMSPACE_IS_KERNEL_P(uio_s->uio_vmspace) ?
		    UIO_SYSSPACE : UIO_USERSPACE;
	}
}

int
zfs_uiomove(void *cp, size_t n, zfs_uio_rw_t dir, zfs_uio_t *uio)
{

	ASSERT3U(zfs_uio_rw(uio), ==, dir);
	return (uiomove(cp, n, GET_UIO_STRUCT(uio)));
}

/*
 * NetBSD has no way to keep a page fault from being taken with the
 * caller's locks held, as FreeBSD's vn_io_fault_uiomove() does; its own
 * file systems call uiomove() the same way.
 */
int
zfs_uio_fault_move(void *p, size_t n, zfs_uio_rw_t dir, zfs_uio_t *uio)
{

	return (zfs_uiomove(p, n, dir, uio));
}

/*
 * Copy as zfs_uiomove() would, without moving the uio on: the copy is
 * made through a uio of its own, over a copy of the iovecs.
 */
int
zfs_uiocopy(void *p, size_t n, zfs_uio_rw_t rw, zfs_uio_t *uio,
    size_t *cbytes)
{
	struct uio *src = GET_UIO_STRUCT(uio), clone;
	struct iovec small[1], *iov;
	size_t iovsz = sizeof (struct iovec) * src->uio_iovcnt;
	int error;

	ASSERT3U(zfs_uio_rw(uio), ==, rw);
	iov = src->uio_iovcnt == 1 ? small : kmem_alloc(iovsz, KM_SLEEP);
	memcpy(iov, src->uio_iov, iovsz);
	clone = *src;
	clone.uio_iov = iov;
	error = uiomove(p, n, &clone);
	*cbytes = src->uio_resid - clone.uio_resid;
	if (iov != small)
		kmem_free(iov, iovsz);
	return (error);
}

/*
 * Move the uio on by n bytes without copying.  illumos does nothing when
 * there are fewer than that left.
 */
void
zfs_uioskip(zfs_uio_t *uio, size_t n)
{
	struct uio *u = GET_UIO_STRUCT(uio);

	if (n > u->uio_resid)
		return;
	u->uio_resid -= n;
	u->uio_offset += n;
	while (n > 0 && u->uio_iovcnt > 0) {
		size_t cnt = MIN(n, u->uio_iov->iov_len);

		u->uio_iov->iov_base = (char *)u->uio_iov->iov_base + cnt;
		u->uio_iov->iov_len -= cnt;
		n -= cnt;
		if (u->uio_iov->iov_len == 0) {
			u->uio_iov++;
			u->uio_iovcnt--;
		}
	}
}

boolean_t
zfs_uio_page_aligned(zfs_uio_t *uio)
{
	const struct iovec *iov = zfs_uio_iov(uio);
	int i;

	for (i = zfs_uio_iovcnt(uio); i > 0; iov++, i--) {
		if (((uintptr_t)iov->iov_base & PAGE_MASK) != 0 ||
		    (iov->iov_len & PAGE_MASK) != 0)
			return (B_FALSE);
	}
	return (B_TRUE);
}

/*
 * Direct I/O wires the user's pages, as physio does with uvm_vslock(),
 * and maps them into kernel space, as vmapbuf() does, for the DMU to
 * reach them through abd_alloc_from_pages().  The direct map is not
 * used: a module is built without _KERNEL_OPT, so __HAVE_DIRECT_MAP is
 * not defined in it, and a KASAN or Xen PV kernel has none.  The
 * iovecs are copied, since the I/O advances the uio's own, and the
 * copy is what is unmapped and unwired.
 *
 * FreeBSD also write-protects the pages of a write until it is done,
 * so that the process cannot change a block between its checksum and
 * the disk.  UVM has no such hold on an anonymous page: a write fault
 * on a wired page goes through.  That is Linux's position too, and
 * vdev.c's zfs_vdev_direct_write_verify defaults on for everything but
 * FreeBSD for it, rechecking the checksum of each Direct I/O write.
 */
int
zfs_uio_get_dio_pages_alloc(zfs_uio_t *uio, zfs_uio_rw_t rw)
{
	struct uio *u = GET_UIO_STRUCT(uio);
	zfs_uio_dio_t *dio = &uio->uio_dio;
	struct vmspace *vs = u->uio_vmspace;
	bool user = !VMSPACE_IS_KERNEL_P(vs);
	vm_prot_t prot;
	int npages, i, error;

	ASSERT3U(zfs_uio_rw(uio), ==, rw);
	ASSERT(zfs_uio_page_aligned(uio));

	/* Reading the file writes the pages, and the other way round. */
	prot = (rw == UIO_READ) ? (VM_PROT_READ | VM_PROT_WRITE) :
	    VM_PROT_READ;
	npages = 0;
	for (i = 0; i < u->uio_iovcnt; i++)
		npages += u->uio_iov[i].iov_len >> PAGE_SHIFT;
	if (npages == 0)
		return (SET_ERROR(EINVAL));

	dio->pages = kmem_alloc(npages * sizeof (void *), KM_SLEEP);
	dio->iovalloc = u->uio_iovcnt;
	dio->iov = kmem_alloc(dio->iovalloc * sizeof (struct iovec), KM_SLEEP);
	dio->kva = kmem_zalloc(dio->iovalloc * sizeof (vaddr_t), KM_SLEEP);
	dio->vs = vs;
	dio->npages = 0;
	dio->iovcnt = 0;

	for (i = 0; i < u->uio_iovcnt; i++) {
		struct iovec *iov = &u->uio_iov[i];
		vaddr_t va = (vaddr_t)iov->iov_base;
		vaddr_t kva, off;
		paddr_t pa;

		if (iov->iov_len == 0)
			continue;
		if (!user) {
			/* Kernel memory is wired and mapped already. */
			for (off = 0; off < iov->iov_len; off += PAGE_SIZE)
				dio->pages[dio->npages++] = (void *)(va + off);
			continue;
		}
		error = uvm_vslock(vs, iov->iov_base, iov->iov_len, prot);
		if (error != 0)
			goto fail;
		kva = uvm_km_alloc(kernel_map, iov->iov_len, 0,
		    UVM_KMF_VAONLY | UVM_KMF_WAITVA);
		for (off = 0; off < iov->iov_len; off += PAGE_SIZE) {
			if (!pmap_extract(vm_map_pmap(&vs->vm_map), va + off,
			    &pa)) {
				/* Wired, so not expected; undo this iovec. */
				pmap_kremove(kva, off);
				pmap_update(pmap_kernel());
				uvm_km_free(kernel_map, kva, iov->iov_len,
				    UVM_KMF_VAONLY);
				uvm_vsunlock(vs, iov->iov_base, iov->iov_len);
				error = EFAULT;
				goto fail;
			}
			pmap_kenter_pa(kva + off, pa, prot, 0);
			dio->pages[dio->npages++] = (void *)(kva + off);
		}
		dio->iov[dio->iovcnt] = *iov;
		dio->kva[dio->iovcnt++] = kva;
	}
	pmap_update(pmap_kernel());
	ASSERT3S(dio->npages, ==, npages);
	uio->uio_extflg |= UIO_DIRECT;
	return (0);

fail:
	dio->npages = npages;
	zfs_uio_free_dio_pages(uio, rw);
	return (SET_ERROR(error));
}

void
zfs_uio_free_dio_pages(zfs_uio_t *uio, zfs_uio_rw_t rw)
{
	zfs_uio_dio_t *dio = &uio->uio_dio;
	int i;

	(void) rw;
	ASSERT3P(dio->pages, !=, NULL);
	for (i = 0; i < dio->iovcnt; i++) {
		vsize_t len = dio->iov[i].iov_len;

		pmap_kremove(dio->kva[i], len);
		pmap_update(pmap_kernel());
		uvm_km_free(kernel_map, dio->kva[i], len, UVM_KMF_VAONLY);
		uvm_vsunlock(dio->vs, dio->iov[i].iov_base, len);
	}
	kmem_free(dio->kva, dio->iovalloc * sizeof (vaddr_t));
	kmem_free(dio->iov, dio->iovalloc * sizeof (struct iovec));
	kmem_free(dio->pages, dio->npages * sizeof (void *));
	dio->pages = NULL;
	dio->iov = NULL;
	dio->kva = NULL;
	dio->npages = 0;
	dio->iovcnt = 0;
	dio->iovalloc = 0;
}
