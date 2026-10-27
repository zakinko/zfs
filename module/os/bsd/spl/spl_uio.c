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
 * Direct I/O wires the user's pages for the I/O to go straight to and
 * from them.  That is not done here yet, so a read or write that would
 * be direct -- O_DIRECT, or direct=always, with aligned buffers -- fails
 * with EOPNOTSUPP: zfs_setup_direct() returns the error as it is.
 */
int
zfs_uio_get_dio_pages_alloc(zfs_uio_t *uio, zfs_uio_rw_t rw)
{

	(void) uio, (void) rw;
	return (SET_ERROR(EOPNOTSUPP));
}

void
zfs_uio_free_dio_pages(zfs_uio_t *uio, zfs_uio_rw_t rw)
{

	(void) uio, (void) rw;
	panic("zfs_uio_free_dio_pages: no Direct I/O pages were taken");
}
