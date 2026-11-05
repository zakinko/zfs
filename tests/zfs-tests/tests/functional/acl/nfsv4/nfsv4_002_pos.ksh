#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# https://opensource.org/license/CDDL-1.0.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/acl/acl_common.kshlib

#
# DESCRIPTION:
#	A deny ACE refuses access the mode gives, without affecting others.
#

verify_runnable "both"

typeset f=$TESTDIR/file.2

function cleanup
{
	rm -f $f
}

log_assert "A deny ACE refuses access the mode gives"
log_onexit cleanup

log_must eval "echo data > $f"
log_must chmod 644 $f
log_must user_run $ZFS_ACL_STAFF1 cat $f

log_must nfs4_setfacl -a D::$(id -u $ZFS_ACL_STAFF1):r $f
log_mustnot user_run $ZFS_ACL_STAFF1 cat $f
log_must user_run $ZFS_ACL_STAFF2 cat $f

log_pass "A deny ACE refuses access the mode gives"
