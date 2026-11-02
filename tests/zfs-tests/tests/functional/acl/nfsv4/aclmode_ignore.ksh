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

#
# Copyright (c) 2026 Showta Ishizaki <zakinko@snowrabbit.org>
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/acl/acl_common.kshlib

#
# DESCRIPTION:
#	With aclmode=ignore, chmod(2) of a file with a non-trivial ACL
#	succeeds and leaves the ACL and the permission bits alone, while
#	the set-id and sticky bits still change as asked.
#
# STRATEGY:
#	1. Give a file an ACL that lets another user read it, then chmod 600
#	2. Check the ACL, the mode and that the user can still read it
#	3. Set and clear the set-id bits with chmod
#	4. Check that a user who may not change the mode still gets EPERM
#	5. Check that chgrp by the owner still clears the set-gid bit
#	6. Check that a file with a trivial ACL is changed as before
#	7. Check that cp -R keeps an inherited ACL
#

verify_runnable "both"

function cleanup
{
	log_must zfs inherit aclmode $TESTPOOL/$TESTFS
	rm -rf $TESTDIR/file $TESTDIR/gid $TESTDIR/trivial $TESTDIR/top
}

log_assert "aclmode=ignore leaves a non-trivial ACL alone on chmod"
log_onexit cleanup

reader=$ZFS_ACL_STAFF1
other=$ZFS_ACL_STAFF2
group=$ZFS_ACL_STAFF_GROUP

log_must zfs set aclmode=ignore $TESTPOOL/$TESTFS

file=$TESTDIR/file
log_must touch $file
log_must chmod 644 $file
log_must setfacl -m u:$reader:rwxp::allow $file
acl=$(getfacl -q $file)

log_must chmod 600 $file
log_must test "$(getfacl -q $file)" = "$acl"
log_must test "$(stat -f %Lp $file)" = "644"
log_must user_run $reader cat $file

log_must chmod u+s,g+s $file
log_must test "$(stat -f %Lp $file)" = "6644"
log_must chmod u-s,g-s $file
log_must test "$(stat -f %Lp $file)" = "644"
log_must test "$(getfacl -q $file)" = "$acl"

log_mustnot user_run $other chmod 777 $file
log_must test "$(getfacl -q $file)" = "$acl"

gid=$TESTDIR/gid
log_must touch $gid
log_must chown $reader:wheel $gid
log_must chmod 2755 $gid
log_must setfacl -m u:$other:r::allow $gid
log_must user_run $reader chgrp $group $gid
log_must test "$(stat -f %Lp $gid)" = "755"

trivial=$TESTDIR/trivial
log_must touch $trivial
log_must chmod 644 $trivial
log_must chmod 600 $trivial
log_must test "$(stat -f %Lp $trivial)" = "600"

top=$TESTDIR/top
log_must mkdir $top
log_must setfacl -m u:$reader:rwxpdDaARWcCos:fd:allow $top
log_must mkdir $top/a
log_must touch $top/a/file
log_must cp -R $top/a $top/b
log_must test "$(getfacl -q $top/b)" = "$(getfacl -q $top/a)"
log_must test "$(getfacl -q $top/b/file)" = "$(getfacl -q $top/a/file)"

log_pass "aclmode=ignore leaves a non-trivial ACL alone on chmod"
