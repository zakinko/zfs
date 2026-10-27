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
 * The odds and ends of illumos's kernel interface that the common code
 * calls and NetBSD has under other names, or not at all.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/proc.h>
#include <sys/file.h>
#include <sys/kmem.h>
#include <sys/misc.h>
#include <sys/sunddi.h>
#include <sys/cmn_err.h>
#include <sys/debug.h>
#include <sys/zone.h>
#include <sys/zfs_context.h>
#include <uvm/uvm.h>

static struct opensolaris_utsname spl_utsname = {
	.sysname = ostype,
	.nodename = hostname,
	.release = osrelease,
	.machine = machine,
};

utsname_t *
utsname(void)
{

	strlcpy(spl_utsname.version, osrelease,
	    sizeof (spl_utsname.version));
	return (&spl_utsname);
}

/* NetBSD's kern.hostid, which zpool records against the pool. */
uint32_t
zone_get_hostid(void *zone)
{

	(void) zone;
	return ((uint32_t)hostid);
}

int
current_is_reclaim_thread(void)
{

	return (curlwp == uvm.pagedaemon_lwp);
}

/* An ioctl issued from inside the kernel has kernel addresses. */
int
ddi_copyin(const void *from, void *to, size_t len, int flags)
{

	if (flags & FKIOCTL) {
		memcpy(to, from, len);
		return (0);
	}
	return (copyin(from, to, len));
}

int
ddi_copyout(const void *from, void *to, size_t len, int flags)
{

	if (flags & FKIOCTL) {
		memcpy(to, from, len);
		return (0);
	}
	return (copyout(from, to, len));
}

/*
 * illumos's ddi_strto*() return EINVAL when there are no digits and
 * ERANGE when the value does not fit, and leave anything after the
 * digits to the caller through nptr.  NetBSD's strtoi() and strtou()
 * report the same, with ECANCELED for no digits and ENOTSUP for what
 * follows them.
 */
static int
spl_strto_status(int rstatus)
{

	switch (rstatus) {
	case 0:
	case ENOTSUP:
		return (0);
	case ECANCELED:
		return (EINVAL);
	default:
		return (rstatus);
	}
}

int
ddi_strtol(const char *str, char **nptr, int base, long *result)
{
	int rstatus;

	*result = (long)strtoi(str, nptr, base, LONG_MIN, LONG_MAX, &rstatus);
	return (spl_strto_status(rstatus));
}

int
ddi_strtoul(const char *str, char **nptr, int base, unsigned long *result)
{
	int rstatus;

	*result = (unsigned long)strtou(str, nptr, base, 0, ULONG_MAX,
	    &rstatus);
	return (spl_strto_status(rstatus));
}

int
ddi_strtoll(const char *str, char **nptr, int base, longlong_t *result)
{
	int rstatus;

	*result = (longlong_t)strtoi(str, nptr, base, INT64_MIN, INT64_MAX,
	    &rstatus);
	return (spl_strto_status(rstatus));
}

int
ddi_strtoull(const char *str, char **nptr, int base,
    unsigned long long *result)
{
	int rstatus;

	*result = (unsigned long long)strtou(str, nptr, base, 0, UINT64_MAX,
	    &rstatus);
	return (spl_strto_status(rstatus));
}

void
spl_panic(const char *file, const char *func, int line, const char *fmt, ...)
{
	va_list ap;

	(void) file, (void) func, (void) line;
	va_start(ap, fmt);
	vpanic(fmt, ap);
	va_end(ap);
}

/*
 * illumos prefixes a notice and a warning and ends them with a newline;
 * a continuation is printed as it is.
 */
void
vcmn_err(int ce, const char *fmt, va_list ap)
{

	switch (ce) {
	case CE_CONT:
		vprintf(fmt, ap);
		break;
	case CE_NOTE:
		printf("NOTICE: ");
		vprintf(fmt, ap);
		printf("\n");
		break;
	case CE_WARN:
		printf("WARNING: ");
		vprintf(fmt, ap);
		printf("\n");
		break;
	case CE_PANIC:
		vpanic(fmt, ap);
		break;
	case CE_IGNORE:
	default:
		break;
	}
}

void
cmn_err(int ce, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vcmn_err(ce, fmt, ap);
	va_end(ap);
}

/*
 * A kernel thread, MP-safe, with the priority illumos gave; see
 * <sys/proc.h> for how the priorities map.
 */
kthread_t *
spl_thread_create(void (*func)(void *), void *arg, pri_t pri,
    const char *name)
{
	lwp_t *l;
	int error;

	error = kthread_create(pri, KTHREAD_MPSAFE, NULL, func, arg, &l,
	    "%s", name);
	if (error != 0)
		return (NULL);
	return (l);
}

/*
 * qsort() over kheapsort(), which wants room for one element; the
 * common code sorts small arrays of pointers, so the room is on the
 * stack when it fits there.  The two are not stable, which the common
 * code does not ask for.
 */
void
qsort(void *base, size_t nmemb, size_t size,
    int (*cmp)(const void *, const void *))
{
	uint64_t small[4];
	void *tmp;

	if (nmemb < 2 || size == 0)
		return;
	tmp = size <= sizeof (small) ? small : kmem_alloc(size, KM_SLEEP);
	(void) kheapsort(base, nmemb, size, cmp, tmp);
	if (tmp != small)
		kmem_free(tmp, size);
}

/*
 * sscanf() for integers: %d, %i, %u and %x, with the h, l and ll
 * length modifiers, %n, and literal characters, with white space in
 * the format matching any amount.  It returns the number of
 * conversions made, as sscanf() does, and stops at the first that
 * fails.
 */
int
sscanf(const char *str, const char *fmt, ...)
{
	const char *s = str;
	va_list ap;
	int count = 0;

	va_start(ap, fmt);
	while (*fmt != '\0') {
		int lmod = 0, base;
		char *end;
		int rstatus;

		if (*fmt == ' ' || *fmt == '\t' || *fmt == '\n') {
			while (isspace((unsigned char)*s))
				s++;
			fmt++;
			continue;
		}
		if (*fmt != '%') {
			if (*s != *fmt)
				break;
			s++;
			fmt++;
			continue;
		}
		fmt++;
		while (*fmt == 'h' || *fmt == 'l') {
			lmod += (*fmt == 'l') ? 1 : -1;
			fmt++;
		}
		if (*fmt == 'n') {
			*va_arg(ap, int *) = (int)(s - str);
			fmt++;
			continue;
		}
		switch (*fmt) {
		case 'd':
		case 'u':
			base = 10;
			break;
		case 'i':
			base = 0;
			break;
		case 'x':
			base = 16;
			break;
		default:
			goto out;
		}
		while (isspace((unsigned char)*s))
			s++;
		if (*fmt == 'u' || *fmt == 'x') {
			uintmax_t v = strtou(s, &end, base, 0, UINTMAX_MAX,
			    &rstatus);
			if (rstatus == ECANCELED || rstatus == EINVAL)
				goto out;
			if (lmod >= 2)
				*va_arg(ap, unsigned long long *) = v;
			else if (lmod == 1)
				*va_arg(ap, unsigned long *) = v;
			else if (lmod < 0)
				*va_arg(ap, unsigned short *) = v;
			else
				*va_arg(ap, unsigned int *) = v;
		} else {
			intmax_t v = strtoi(s, &end, base, INTMAX_MIN,
			    INTMAX_MAX, &rstatus);
			if (rstatus == ECANCELED || rstatus == EINVAL)
				goto out;
			if (lmod >= 2)
				*va_arg(ap, long long *) = v;
			else if (lmod == 1)
				*va_arg(ap, long *) = v;
			else if (lmod < 0)
				*va_arg(ap, short *) = v;
			else
				*va_arg(ap, int *) = v;
		}
		s = end;
		count++;
		fmt++;
	}
out:
	va_end(ap);
	return (count);
}
