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

#include <limits.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include "../../libspl_impl.h"

__attribute__((visibility("hidden"))) ssize_t
getexecname_impl(char *execname)
{
	static const int name[] = {
		CTL_KERN, KERN_PROC_ARGS, -1, KERN_PROC_PATHNAME,
	};
	size_t len = PATH_MAX;

	if (sysctl(name, __arraycount(name), execname, &len, NULL, 0) == -1)
		return (-1);

	return ((ssize_t)len);
}
