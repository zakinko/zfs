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
 * illumos's kernel statistics as sysctl nodes, under
 * kstat.<module>.<class>.<name>, the tree FreeBSD gives them.  A named
 * kstat is a node per field, each read through the kstat's update
 * function; a raw or I/O kstat is one string node holding the text its
 * functions print.
 *
 * A sysctl node's name is letters, digits, '-' and '_', not starting
 * with a digit, and shorter than SYSCTL_NAMELEN.  A kstat's module and
 * class are split into nodes at '/' and '.', and other characters
 * become '_'; a name longer than a node takes is cut short, so two
 * pools whose names differ only past that point share a node.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kmem.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <sys/kstat.h>
#include <sys/debug.h>
#include <sys/time.h>
#include <sys/zfs_context.h>

/*
 * sysctl_createv() is called by its own name, past the macro that
 * checks the data pointer against a CTLTYPE_ spelled out at the call:
 * the data here is a kstat or a field of one, read by a handler.
 */

/* A named kstat's field, as its node knows it. */
struct kstat_field {
	kstat_t		*kf_ks;
	uint_t		kf_index;
};

static void
kstat_node_name(char *dst, const char *src, size_t len)
{
	size_t i = 0;

	if (*src >= '0' && *src <= '9')
		dst[i++] = '_';
	for (; *src != '\0' && i < SYSCTL_NAMELEN - 1; src++, i++) {
		char c = *src;

		dst[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '-' || c == '_') ? c : '_';
	}
	dst[MIN(i, len - 1)] = '\0';
}

/* Create kstat.<a>.<b>... from path, split at '/' and '.'. */
static const struct sysctlnode *
kstat_node_path(struct sysctllog **clog, const struct sysctlnode *node,
    const char *path)
{
	char comp[KSTAT_STRLEN], name[SYSCTL_NAMELEN];
	const char *p, *q;

	for (p = path; *p != '\0'; p = (*q != '\0') ? q + 1 : q) {
		size_t len;

		for (q = p; *q != '\0' && *q != '/' && *q != '.'; q++)
			continue;
		if (q == p)
			continue;
		len = MIN((size_t)(q - p), sizeof (comp) - 1);
		memcpy(comp, p, len);
		comp[len] = '\0';
		kstat_node_name(name, comp, sizeof (name));
		if ((sysctl_createv)(clog, 0, &node, &node, 0, CTLTYPE_NODE,
		    name, NULL, NULL, 0, NULL, 0, CTL_CREATE, CTL_EOL) != 0)
			return (NULL);
	}
	return (node);
}

static int
kstat_default_update(kstat_t *ksp, int rw)
{

	return (rw == KSTAT_WRITE ? EACCES : 0);
}

static void *
kstat_raw_default_addr(kstat_t *ksp, loff_t n)
{

	return (n == 0 ? ksp->ks_data : NULL);
}

void
__kstat_set_raw_ops(kstat_t *ksp, int (*headers)(char *, size_t),
    int (*data)(char *, size_t, void *), void *(*addr)(kstat_t *, loff_t))
{

	ksp->ks_raw_ops.headers = headers;
	ksp->ks_raw_ops.data = data;
	ksp->ks_raw_ops.addr = addr;
}

void
__kstat_set_seq_raw_ops(kstat_t *ksp, int (*headers)(struct seq_file *),
    int (*data)(char *, size_t, void *), void *(*addr)(kstat_t *, loff_t))
{

	ksp->ks_raw_ops.seq_headers = headers;
	ksp->ks_raw_ops.data = data;
	ksp->ks_raw_ops.addr = addr;
}

void
seq_printf(struct seq_file *m, const char *fmt, ...)
{
	size_t len = strlen(m->sf_buf);
	va_list ap;

	if (len >= m->sf_size)
		return;
	va_start(ap, fmt);
	(void) vsnprintf(m->sf_buf + len, m->sf_size - len, fmt, ap);
	va_end(ap);
}

kstat_t *
__kstat_create(const char *module, int instance, const char *name,
    const char *class, uchar_t type, uint_t ndata, uchar_t flags)
{
	kstat_t *ksp;

	ksp = kmem_zalloc(sizeof (*ksp), KM_SLEEP);
	ksp->ks_magic = KS_MAGIC;
	ksp->ks_crtime = gethrtime();
	ksp->ks_snaptime = ksp->ks_crtime;
	strlcpy(ksp->ks_module, module, sizeof (ksp->ks_module));
	ksp->ks_instance = instance;
	strlcpy(ksp->ks_name, name, sizeof (ksp->ks_name));
	strlcpy(ksp->ks_class, class, sizeof (ksp->ks_class));
	ksp->ks_type = type;
	ksp->ks_flags = flags;
	ksp->ks_update = kstat_default_update;
	mutex_init(&ksp->ks_private_lock, NULL, MUTEX_DEFAULT, NULL);
	ksp->ks_lock = &ksp->ks_private_lock;

	switch (type) {
	case KSTAT_TYPE_RAW:
		ksp->ks_ndata = 1;
		ksp->ks_data_size = ndata;
		break;
	case KSTAT_TYPE_NAMED:
		ksp->ks_ndata = ndata;
		ksp->ks_data_size = ndata * sizeof (kstat_named_t);
		break;
	case KSTAT_TYPE_INTR:
		ksp->ks_ndata = ndata;
		ksp->ks_data_size = ndata * sizeof (kstat_intr_t);
		break;
	case KSTAT_TYPE_IO:
		ksp->ks_ndata = ndata;
		ksp->ks_data_size = ndata * sizeof (kstat_io_t);
		break;
	case KSTAT_TYPE_TIMER:
		ksp->ks_ndata = ndata;
		ksp->ks_data_size = ndata * sizeof (kstat_timer_t);
		break;
	default:
		panic("kstat %s: type %d", name, type);
	}

	if (!(flags & KSTAT_FLAG_VIRTUAL) && ksp->ks_data_size != 0)
		ksp->ks_data = kmem_zalloc(ksp->ks_data_size, KM_SLEEP);
	return (ksp);
}

static int
kstat_named_handler(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	struct kstat_field *kf = rnode->sysctl_data;
	kstat_t *ksp = kf->kf_ks;
	kstat_named_t *kn;
	uint64_t q;
	int32_t i;
	char *s = NULL;
	size_t slen = 0;
	int error;

	if (newp != NULL)
		return (EPERM);
	mutex_enter(ksp->ks_lock);
	(void) ksp->ks_update(ksp, KSTAT_READ);
	kn = &((kstat_named_t *)ksp->ks_data)[kf->kf_index];
	switch (kn->data_type) {
	case KSTAT_DATA_INT32:
		i = kn->value.i32;
		node.sysctl_data = &i;
		break;
	case KSTAT_DATA_UINT32:
		i = (int32_t)kn->value.ui32;
		node.sysctl_data = &i;
		break;
	case KSTAT_DATA_INT64:
	case KSTAT_DATA_UINT64:
		q = kn->value.ui64;
		node.sysctl_data = &q;
		break;
	case KSTAT_DATA_LONG:
		q = (uint64_t)kn->value.l;
		node.sysctl_data = &q;
		break;
	case KSTAT_DATA_ULONG:
		q = (uint64_t)kn->value.ul;
		node.sysctl_data = &q;
		break;
	case KSTAT_DATA_STRING:
		slen = KSTAT_NAMED_STR_BUFLEN(kn);
		s = kmem_zalloc(slen + 1, KM_SLEEP);
		if (KSTAT_NAMED_STR_PTR(kn) != NULL)
			strlcpy(s, KSTAT_NAMED_STR_PTR(kn), slen + 1);
		node.sysctl_data = s;
		node.sysctl_size = slen + 1;
		break;
	case KSTAT_DATA_CHAR:
	default:
		slen = sizeof (kn->value.c);
		s = kmem_zalloc(slen + 1, KM_SLEEP);
		memcpy(s, kn->value.c, slen);
		node.sysctl_data = s;
		node.sysctl_size = slen + 1;
		break;
	}
	mutex_exit(ksp->ks_lock);
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (s != NULL)
		kmem_free(s, slen + 1);
	return (error);
}

/* Text that grows as it is appended to. */
struct kstat_text {
	char	*kt_buf;
	size_t	kt_len;
	size_t	kt_size;
};

static void
kstat_text_cat(struct kstat_text *kt, const char *s)
{
	size_t n = strlen(s);

	if (kt->kt_len + n + 1 > kt->kt_size) {
		size_t size = MAX(kt->kt_size * 2, kt->kt_len + n + 1);
		char *buf = kmem_alloc(size, KM_SLEEP);

		if (kt->kt_buf != NULL) {
			memcpy(buf, kt->kt_buf, kt->kt_len);
			kmem_free(kt->kt_buf, kt->kt_size);
		}
		kt->kt_buf = buf;
		kt->kt_size = size;
	}
	memcpy(kt->kt_buf + kt->kt_len, s, n + 1);
	kt->kt_len += n;
}

/* Double the scratch buffer a print function said was too small. */
static int
kstat_raw_grow(kstat_t *ksp)
{

	if (ksp->ks_raw_bufsize >= KSTAT_RAW_MAX)
		return (ENOMEM);
	kmem_free(ksp->ks_raw_buf, ksp->ks_raw_bufsize);
	ksp->ks_raw_bufsize = MIN(ksp->ks_raw_bufsize * 2, KSTAT_RAW_MAX);
	ksp->ks_raw_buf = kmem_zalloc(ksp->ks_raw_bufsize, KM_SLEEP);
	return (0);
}

static void
kstat_raw_text(kstat_t *ksp, struct kstat_text *kt)
{
	void *(*addr)(kstat_t *, loff_t);
	void *data;
	loff_t n;
	int rc = 0;

	addr = ksp->ks_raw_ops.addr != NULL ? ksp->ks_raw_ops.addr :
	    kstat_raw_default_addr;
	ksp->ks_raw_bufsize = PAGE_SIZE;
	ksp->ks_raw_buf = kmem_zalloc(PAGE_SIZE, KM_SLEEP);

	if (ksp->ks_raw_ops.headers != NULL ||
	    ksp->ks_raw_ops.seq_headers != NULL) {
		for (;;) {
			memset(ksp->ks_raw_buf, 0, ksp->ks_raw_bufsize);
			if (ksp->ks_raw_ops.headers != NULL) {
				rc = ksp->ks_raw_ops.headers(ksp->ks_raw_buf,
				    ksp->ks_raw_bufsize);
			} else {
				struct seq_file f = {
					.sf_buf = ksp->ks_raw_buf,
					.sf_size = ksp->ks_raw_bufsize,
				};
				rc = ksp->ks_raw_ops.seq_headers(&f);
			}
			if (rc != ENOMEM || kstat_raw_grow(ksp) != 0)
				break;
		}
		if (rc == 0)
			kstat_text_cat(kt, ksp->ks_raw_buf);
	}

	for (n = 0; (data = addr(ksp, n)) != NULL; n++) {
		if (ksp->ks_raw_ops.data == NULL)
			break;
		for (;;) {
			memset(ksp->ks_raw_buf, 0, ksp->ks_raw_bufsize);
			rc = ksp->ks_raw_ops.data(ksp->ks_raw_buf,
			    ksp->ks_raw_bufsize, data);
			if (rc != ENOMEM || kstat_raw_grow(ksp) != 0)
				break;
		}
		if (rc == 0)
			kstat_text_cat(kt, ksp->ks_raw_buf);
	}
	kmem_free(ksp->ks_raw_buf, ksp->ks_raw_bufsize);
	ksp->ks_raw_buf = NULL;
	ksp->ks_raw_bufsize = 0;
}

static void
kstat_io_text(kstat_t *ksp, struct kstat_text *kt)
{
	kstat_io_t *kio = ksp->ks_data;
	char line[256];

	snprintf(line, sizeof (line),
	    "nread nwritten reads writes wtime wlentime wupdate rtime "
	    "rlentime rupdate wcnt rcnt\n"
	    "%llu %llu %u %u %lld %lld %lld %lld %lld %lld %u %u\n",
	    (unsigned long long)kio->nread, (unsigned long long)kio->nwritten,
	    kio->reads, kio->writes, (long long)kio->wtime,
	    (long long)kio->wlentime, (long long)kio->wlastupdate,
	    (long long)kio->rtime, (long long)kio->rlentime,
	    (long long)kio->rlastupdate, kio->wcnt, kio->rcnt);
	kstat_text_cat(kt, line);
}

static int
kstat_text_handler(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	kstat_t *ksp = rnode->sysctl_data;
	struct kstat_text kt = { NULL, 0, 0 };
	int error;

	/*
	 * Writing anything asks the kstat to clear itself, as writing to
	 * the Linux file does; whether it can is its update function's
	 * answer.
	 */
	if (newp != NULL) {
		mutex_enter(ksp->ks_lock);
		error = ksp->ks_update(ksp, KSTAT_WRITE);
		mutex_exit(ksp->ks_lock);
		return (error);
	}
	kstat_text_cat(&kt, "");
	mutex_enter(ksp->ks_lock);
	(void) ksp->ks_update(ksp, KSTAT_READ);
	if (ksp->ks_type == KSTAT_TYPE_IO)
		kstat_io_text(ksp, &kt);
	else
		kstat_raw_text(ksp, &kt);
	mutex_exit(ksp->ks_lock);

	node.sysctl_data = kt.kt_buf;
	node.sysctl_size = kt.kt_len + 1;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	kmem_free(kt.kt_buf, kt.kt_size);
	return (error);
}

void
__kstat_install(kstat_t *ksp)
{
	const struct sysctlnode *node = NULL;
	char name[SYSCTL_NAMELEN];
	uint_t i;

	if ((sysctl_createv)(&ksp->ks_sysctllog, 0, NULL, &node, 0,
	    CTLTYPE_NODE, "kstat", NULL, NULL, 0, NULL, 0, CTL_CREATE,
	    CTL_EOL) != 0)
		return;
	if ((node = kstat_node_path(&ksp->ks_sysctllog, node,
	    ksp->ks_module)) == NULL)
		return;
	if ((node = kstat_node_path(&ksp->ks_sysctllog, node,
	    ksp->ks_class)) == NULL)
		return;
	kstat_node_name(name, ksp->ks_name, sizeof (name));

	if (ksp->ks_type != KSTAT_TYPE_NAMED) {
		(void) (sysctl_createv)(&ksp->ks_sysctllog, 0, &node, NULL,
		    CTLFLAG_READWRITE, CTLTYPE_STRING, name, NULL,
		    kstat_text_handler, 0, ksp, 0, CTL_CREATE, CTL_EOL);
		return;
	}

	if ((sysctl_createv)(&ksp->ks_sysctllog, 0, &node, &node, 0,
	    CTLTYPE_NODE, name, NULL, NULL, 0, NULL, 0, CTL_CREATE,
	    CTL_EOL) != 0)
		return;
	ksp->ks_fields = kmem_zalloc(sizeof (struct kstat_field) *
	    ksp->ks_ndata, KM_SLEEP);
	for (i = 0; i < ksp->ks_ndata; i++) {
		struct kstat_field *kf =
		    &((struct kstat_field *)ksp->ks_fields)[i];
		kstat_named_t *kn = &((kstat_named_t *)ksp->ks_data)[i];
		int type;

		kf->kf_ks = ksp;
		kf->kf_index = i;
		switch (kn->data_type) {
		case KSTAT_DATA_INT32:
		case KSTAT_DATA_UINT32:
			type = CTLTYPE_INT;
			break;
		case KSTAT_DATA_INT64:
		case KSTAT_DATA_UINT64:
		case KSTAT_DATA_LONG:
		case KSTAT_DATA_ULONG:
			type = CTLTYPE_QUAD;
			break;
		default:
			type = CTLTYPE_STRING;
			break;
		}
		kstat_node_name(name, kn->name, sizeof (name));
		(void) (sysctl_createv)(&ksp->ks_sysctllog, 0, &node, NULL, 0,
		    type, name, NULL, kstat_named_handler, 0, kf, 0,
		    CTL_CREATE, CTL_EOL);
	}
}

void
__kstat_delete(kstat_t *ksp)
{

	sysctl_teardown(&ksp->ks_sysctllog);
	if (ksp->ks_fields != NULL && ksp->ks_type == KSTAT_TYPE_NAMED)
		kmem_free(ksp->ks_fields,
		    sizeof (struct kstat_field) * ksp->ks_ndata);
	if (!(ksp->ks_flags & KSTAT_FLAG_VIRTUAL) && ksp->ks_data != NULL)
		kmem_free(ksp->ks_data, ksp->ks_data_size);
	mutex_destroy(&ksp->ks_private_lock);
	kmem_free(ksp, sizeof (*ksp));
}
