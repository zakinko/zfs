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
#	An inheritable ACE on a directory reaches a file created in it.
#

verify_runnable "both"

typeset d=$TESTDIR/dir.4

function cleanup
{
	rm -rf $d
}

log_assert "An inheritable ACE reaches new files"
log_onexit cleanup

typeset uid=$(id -u $ZFS_ACL_STAFF1)
log_must mkdir $d
log_must chmod 700 $d
log_must nfs4_setfacl -a A:fd:$uid:rx $d
log_must eval "echo data > $d/f"
log_must chmod 600 $d/f
log_must eval "nfs4_getfacl $d/f | grep -q ':$uid:'"
log_must user_run $ZFS_ACL_STAFF1 cat $d/f

log_pass "An inheritable ACE reaches new files"
