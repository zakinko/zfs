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

#ifndef _SPL_SYS_KSTAT_H
#define	_SPL_SYS_KSTAT_H

/*
 * illumos's kernel statistics, whose structures are its ABI.  NetBSD
 * has no kstat framework; a kstat is published as sysctl nodes under
 * kstat.<module>.<class>.<name>, as FreeBSD publishes it, and the nodes
 * are taken down with the kstat.  See spl_kstat.c.
 */
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/mutex.h>

#define	KSTAT_STRLEN		255
#define	KSTAT_RAW_MAX		(128*1024)

#define	KSTAT_TYPE_RAW		0
#define	KSTAT_TYPE_NAMED	1
#define	KSTAT_TYPE_INTR		2
#define	KSTAT_TYPE_IO		3
#define	KSTAT_TYPE_TIMER	4
#define	KSTAT_NUM_TYPES		5

#define	KSTAT_DATA_CHAR		0
#define	KSTAT_DATA_INT32	1
#define	KSTAT_DATA_UINT32	2
#define	KSTAT_DATA_INT64	3
#define	KSTAT_DATA_UINT64	4
#define	KSTAT_DATA_LONG		5
#define	KSTAT_DATA_ULONG	6
#define	KSTAT_DATA_STRING	7
#define	KSTAT_NUM_DATAS		8

#define	KSTAT_INTR_HARD		0
#define	KSTAT_INTR_SOFT		1
#define	KSTAT_INTR_WATCHDOG	2
#define	KSTAT_INTR_SPURIOUS	3
#define	KSTAT_INTR_MULTSVC	4
#define	KSTAT_NUM_INTRS		5

#define	KSTAT_FLAG_VIRTUAL	0x01
#define	KSTAT_FLAG_VAR_SIZE	0x02
#define	KSTAT_FLAG_WRITABLE	0x04
#define	KSTAT_FLAG_PERSISTENT	0x08
#define	KSTAT_FLAG_DORMANT	0x10
#define	KSTAT_FLAG_INVALID	0x20
#define	KSTAT_FLAG_LONGSTRINGS	0x40
#define	KSTAT_FLAG_NO_HEADERS	0x80

#define	KS_MAGIC		0x9d9d9d9d

#define	KSTAT_READ		0
#define	KSTAT_WRITE		1

struct kstat_s;
typedef struct kstat_s kstat_t;
typedef int kid_t;
typedef int kstat_update_t(struct kstat_s *, int);

/* The common code's raw kstats print through this. */
struct seq_file {
	char	*sf_buf;
	size_t	sf_size;
};
void seq_printf(struct seq_file *, const char *, ...)
    __attribute__((format(__printf__, 2, 3)));

typedef struct kstat_raw_ops {
	int (*headers)(char *, size_t);
	int (*seq_headers)(struct seq_file *);
	int (*data)(char *, size_t, void *);
	void *(*addr)(kstat_t *, loff_t);
} kstat_raw_ops_t;

struct kstat_s {
	int		ks_magic;
	kid_t		ks_kid;
	hrtime_t	ks_crtime;
	hrtime_t	ks_snaptime;
	char		ks_module[KSTAT_STRLEN];
	int		ks_instance;
	char		ks_name[KSTAT_STRLEN];
	char		ks_class[KSTAT_STRLEN];
	uchar_t		ks_type;
	uchar_t		ks_flags;
	void		*ks_data;
	uint_t		ks_ndata;
	size_t		ks_data_size;
	kstat_update_t	*ks_update;
	void		*ks_private;
	void		*ks_private1;
	kmutex_t	ks_private_lock;
	kmutex_t	*ks_lock;
	kstat_raw_ops_t	ks_raw_ops;
	char		*ks_raw_buf;
	size_t		ks_raw_bufsize;
	struct sysctllog *ks_sysctllog;
};

typedef struct kstat_named_s {
	char	name[KSTAT_STRLEN];
	uchar_t	data_type;
	union {
		char c[16];
		int32_t	i32;
		uint32_t ui32;
		int64_t i64;
		uint64_t ui64;
		long l;
		ulong_t ul;
		struct {
			union {
				char *ptr;
				char __pad[8];
			} addr;
			uint32_t len;
		} string;
	} value;
} kstat_named_t;

#define	KSTAT_NAMED_STR_PTR(knptr)	((knptr)->value.string.addr.ptr)
#define	KSTAT_NAMED_STR_BUFLEN(knptr)	((knptr)->value.string.len)

typedef struct kstat_intr {
	uint_t intrs[KSTAT_NUM_INTRS];
} kstat_intr_t;

typedef struct kstat_io {
	u_longlong_t	nread;
	u_longlong_t	nwritten;
	uint_t		reads;
	uint_t		writes;
	hrtime_t	wtime;
	hrtime_t	wlentime;
	hrtime_t	wlastupdate;
	hrtime_t	rtime;
	hrtime_t	rlentime;
	hrtime_t	rlastupdate;
	uint_t		wcnt;
	uint_t		rcnt;
} kstat_io_t;

typedef struct kstat_timer {
	char		name[KSTAT_STRLEN];
	u_longlong_t	num_events;
	hrtime_t	elapsed_time;
	hrtime_t	min_time;
	hrtime_t	max_time;
	hrtime_t	start_time;
	hrtime_t	stop_time;
} kstat_timer_t;

extern int spl_kstat_init(void);
extern void spl_kstat_fini(void);

extern void __kstat_set_raw_ops(kstat_t *, int (*)(char *, size_t),
    int (*)(char *, size_t, void *), void *(*)(kstat_t *, loff_t));
extern void __kstat_set_seq_raw_ops(kstat_t *, int (*)(struct seq_file *),
    int (*)(char *, size_t, void *), void *(*)(kstat_t *, loff_t));
extern kstat_t *__kstat_create(const char *, int, const char *,
    const char *, uchar_t, uint_t, uchar_t);
extern void __kstat_install(kstat_t *);
extern void __kstat_delete(kstat_t *);

#define	kstat_set_seq_raw_ops(k, h, d, a)	__kstat_set_seq_raw_ops(k, h, d, a)
#define	kstat_set_raw_ops(k, h, d, a)		__kstat_set_raw_ops(k, h, d, a)
#define	kstat_create(m, i, n, c, t, s, f)	__kstat_create(m, i, n, c, t, s, f)
#define	kstat_install(k)			__kstat_install(k)
#define	kstat_delete(k)				__kstat_delete(k)

#endif	/* _SPL_SYS_KSTAT_H */
