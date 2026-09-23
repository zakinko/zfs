dnl #
dnl # struct statfs.  NetBSD has statvfs(2) only, and its struct has no
dnl # f_type, so callers that key on the file system magic need a guard.
dnl #
AC_DEFUN([ZFS_AC_CONFIG_USER_STRUCT_STATFS], [
	AC_CHECK_MEMBER([struct statfs.f_type], [
		AC_DEFINE([HAVE_STRUCT_STATFS], 1,
		    [struct statfs with an f_type member is available])
	], [], [[
		#include <sys/param.h>
		#include <sys/mount.h>
	]])
])
