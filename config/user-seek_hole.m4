dnl #
dnl # SEEK_HOLE and SEEK_DATA as lseek(2) whence values.  NetBSD has the
dnl # same knowledge behind the FIOSEEKHOLE and FIOSEEKDATA ioctls
dnl # instead, so a test written against lseek(2) cannot run there.
dnl #
AC_DEFUN([ZFS_AC_CONFIG_USER_SEEK_HOLE], [
	AC_CHECK_DECL([SEEK_HOLE], [
		AC_DEFINE([HAVE_LSEEK_SEEK_HOLE], 1,
		    [lseek() understands SEEK_HOLE and SEEK_DATA])
		have_lseek_seek_hole=yes
	], [
		have_lseek_seek_hole=no
	], [[#include <unistd.h>]])
	AM_CONDITIONAL([HAVE_LSEEK_SEEK_HOLE],
	    [test "x$have_lseek_seek_hole" = xyes])
])
