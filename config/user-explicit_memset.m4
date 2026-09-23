dnl #
dnl # explicit_memset() is a BSD extension, and the one the BSDs want
dnl # used for clearing sensitive data.
dnl #
AC_DEFUN([ZFS_AC_CONFIG_USER_EXPLICIT_MEMSET], [
	AC_CHECK_DECL([explicit_memset], [
		AC_DEFINE([HAVE_EXPLICIT_MEMSET], 1,
		    [explicit_memset() is available])
	], [], [[#include <string.h>]])
])
