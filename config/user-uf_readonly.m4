dnl #
dnl # UF_READONLY, the DOS READONLY attribute as a chflags(2) flag.  Linux
dnl # reaches the same attribute through a ZFS ioctl, and NetBSD has neither.
dnl #
AC_DEFUN([ZFS_AC_CONFIG_USER_UF_READONLY], [
	AC_CHECK_DECL([UF_READONLY], [
		have_uf_readonly=yes
	], [
		have_uf_readonly=no
	], [[#include <sys/stat.h>]])
	AM_CONDITIONAL([HAVE_UF_READONLY], [test "x$have_uf_readonly" = xyes])
])
