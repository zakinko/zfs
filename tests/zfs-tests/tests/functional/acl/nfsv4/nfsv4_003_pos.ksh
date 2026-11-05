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
#	chmod keeps a named ACE under aclmode=passthrough and drops it under
#	aclmode=discard.
#

verify_runnable "both"

typeset f=$TESTDIR/file.3

function cleanup
{
	rm -f $f
	zfs set aclmode=passthrough $TESTPOOL/$TESTFS
}

log_assert "aclmode decides what chmod does to a named ACE"
log_onexit cleanup

typeset uid=$(id -u $ZFS_ACL_STAFF1)
log_must touch $f
log_must nfs4_setfacl -a A::$uid:r $f

log_must chmod 640 $f
log_must eval "nfs4_getfacl $f | grep -q ':$uid:'"

log_must zfs set aclmode=discard $TESTPOOL/$TESTFS
log_must chmod 600 $f
log_mustnot eval "nfs4_getfacl $f | grep -q ':$uid:'"

log_pass "aclmode decides what chmod does to a named ACE"
