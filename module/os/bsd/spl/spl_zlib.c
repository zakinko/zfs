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
 * gzip compression for the common code's gzip.c, over the zlib NetBSD's
 * kernel carries (sys/net/zlib.c), a built-in module of its own in
 * GENERIC.  That zlib is 1.1.4, which exports only the explicit
 * deflateInit2_() and inflateInit2_(), so the window is named here: a
 * zlib stream, header and all, as compress2() and uncompress() make.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/kmem.h>
#include <net/zlib.h>
#include <sys/zmod.h>

/*
 * zlib asks for its state as items and sizes, and gives it back by
 * address only, so the size is kept in front of what it is given.  The
 * state is a few hundred kilobytes, and failing to allocate it would
 * fail the read of a block that is there, so the allocation waits.
 */
static void *
spl_zalloc(void *opaque, uInt items, uInt size)
{
	size_t len = (size_t)items * size + sizeof (uint64_t);
	uint64_t *p;

	(void) opaque;
	p = kmem_alloc(len, KM_SLEEP);
	*p = len;
	return (p + 1);
}

static void
spl_zfree(void *opaque, void *ptr)
{
	uint64_t *p = (uint64_t *)ptr - 1;

	(void) opaque;
	kmem_free(p, (size_t)*p);
}

int
z_compress_level(void *dest, size_t *destLen, const void *source,
    size_t sourceLen, int level)
{
	z_stream stream;
	int err;

	memset(&stream, 0, sizeof (stream));
	stream.next_in = (Bytef *)(uintptr_t)source;
	stream.avail_in = (uInt)sourceLen;
	stream.next_out = dest;
	stream.avail_out = (uInt)*destLen;
	if ((size_t)stream.avail_out != *destLen ||
	    (size_t)stream.avail_in != sourceLen)
		return (Z_BUF_ERROR);
	stream.zalloc = spl_zalloc;
	stream.zfree = spl_zfree;

	err = deflateInit2(&stream, level, Z_DEFLATED, MAX_WBITS, 8,
	    Z_DEFAULT_STRATEGY);
	if (err != Z_OK)
		return (err);
	err = deflate(&stream, Z_FINISH);
	if (err != Z_STREAM_END) {
		(void) deflateEnd(&stream);
		return (err == Z_OK ? Z_BUF_ERROR : err);
	}
	*destLen = stream.total_out;
	return (deflateEnd(&stream));
}

int
z_uncompress(void *dest, size_t *destLen, const void *source,
    size_t sourceLen)
{
	z_stream stream;
	int err;

	memset(&stream, 0, sizeof (stream));
	stream.next_in = (Bytef *)(uintptr_t)source;
	stream.avail_in = (uInt)sourceLen;
	stream.next_out = dest;
	stream.avail_out = (uInt)*destLen;
	if ((size_t)stream.avail_out != *destLen ||
	    (size_t)stream.avail_in != sourceLen)
		return (Z_BUF_ERROR);
	stream.zalloc = spl_zalloc;
	stream.zfree = spl_zfree;

	err = inflateInit2(&stream, MAX_WBITS);
	if (err != Z_OK)
		return (err);
	err = inflate(&stream, Z_FINISH);
	if (err != Z_STREAM_END) {
		(void) inflateEnd(&stream);
		return (err == Z_NEED_DICT || (err == Z_BUF_ERROR &&
		    stream.avail_in == 0) ? Z_DATA_ERROR : err);
	}
	*destLen = stream.total_out;
	return (inflateEnd(&stream));
}
