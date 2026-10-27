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

#ifndef _SPL_PROP_PROP_OBJECT_H
#define	_SPL_PROP_PROP_OBJECT_H

/*
 * NetBSD's proplib names two of its object types PROP_TYPE_NUMBER and
 * PROP_TYPE_STRING, which are also the common code's zprop_type_t.
 * <sys/kauth.h> reaches proplib through <secmodel/secmodel.h>, so every
 * file that sees credentials sees both.  The two are renamed here while
 * proplib declares them; prop_object.h is the only place proplib uses
 * these names, and an enumerator's name is not part of any ABI.
 */
#define	PROP_TYPE_NUMBER	NETBSD_PROP_TYPE_NUMBER
#define	PROP_TYPE_STRING	NETBSD_PROP_TYPE_STRING
#include_next <prop/prop_object.h>
#undef	PROP_TYPE_NUMBER
#undef	PROP_TYPE_STRING

#endif	/* _SPL_PROP_PROP_OBJECT_H */
