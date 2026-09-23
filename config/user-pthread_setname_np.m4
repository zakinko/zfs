dnl #
dnl # pthread_setname_np() takes a printf(3) format and one argument for
dnl # it on NetBSD, and a plain string everywhere else.
dnl #
AC_DEFUN([ZFS_AC_CONFIG_USER_PTHREAD_SETNAME_NP], [
	AC_MSG_CHECKING([whether pthread_setname_np() takes a format])
	saved_CFLAGS="$CFLAGS"
	CFLAGS="$CFLAGS -Werror"
	AC_LINK_IFELSE([AC_LANG_PROGRAM([
		#include <pthread.h>
	], [
		(void) pthread_setname_np(pthread_self(), "%s", (void *)"x");
	])], [
		AC_MSG_RESULT([yes])
		AC_DEFINE([HAVE_PTHREAD_SETNAME_NP_FORMAT], 1,
		    [pthread_setname_np() takes a format and an argument])
	], [
		AC_MSG_RESULT([no])
	])
	CFLAGS="$saved_CFLAGS"
])
