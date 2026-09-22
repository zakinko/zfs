dnl # SPDX-License-Identifier: CDDL-1.0
dnl #
dnl # Set the target system
dnl #
AC_DEFUN([ZFS_AC_CONFIG_ALWAYS_SYSTEM], [
	AC_MSG_CHECKING([for system type ($host_os)])
	case $host_os in
		*linux*)
			AC_DEFINE([SYSTEM_LINUX], [1],
				[True if ZFS is to be compiled for a Linux system])
			ac_system="Linux"
			ac_system_l="linux"
			;;
		*freebsd*)
			AC_DEFINE([SYSTEM_FREEBSD], [1],
				[True if ZFS is to be compiled for a FreeBSD system])
			ac_system="FreeBSD"
			ac_system_l="freebsd"
			;;
		dnl #
		dnl # The systems below share the os/bsd platform layer.  Each
		dnl # one also gets a define of its own, as the layer still has
		dnl # to tell them apart in a few places.
		dnl #
		*netbsd*)
			AC_DEFINE([SYSTEM_BSD], [1],
				[True if ZFS is to be compiled for a BSD system])
			AC_DEFINE([SYSTEM_NETBSD], [1],
				[True if ZFS is to be compiled for a NetBSD system])
			ac_system="NetBSD"
			ac_system_l="bsd"
			;;
		*openbsd*)
			AC_DEFINE([SYSTEM_BSD], [1],
				[True if ZFS is to be compiled for a BSD system])
			AC_DEFINE([SYSTEM_OPENBSD], [1],
				[True if ZFS is to be compiled for an OpenBSD system])
			ac_system="OpenBSD"
			ac_system_l="bsd"
			;;
		*dragonfly*)
			AC_DEFINE([SYSTEM_BSD], [1],
				[True if ZFS is to be compiled for a BSD system])
			AC_DEFINE([SYSTEM_DRAGONFLY], [1],
				[True if ZFS is to be compiled for a DragonFly system])
			ac_system="DragonFly"
			ac_system_l="bsd"
			;;
		*)
			ac_system="unknown"
			ac_system_l="unknown"
			;;
	esac
	AC_MSG_RESULT([$ac_system])
	AC_SUBST([ac_system])
	AC_SUBST([ac_system_l])

	AM_CONDITIONAL([BUILD_LINUX], [test "x$ac_system" = "xLinux"])
	AM_CONDITIONAL([BUILD_FREEBSD], [test "x$ac_system" = "xFreeBSD"])
	AM_CONDITIONAL([BUILD_BSD], [test "x$ac_system_l" = "xbsd"])
	AM_CONDITIONAL([BUILD_NETBSD], [test "x$ac_system" = "xNetBSD"])
	AM_CONDITIONAL([BUILD_OPENBSD], [test "x$ac_system" = "xOpenBSD"])
	AM_CONDITIONAL([BUILD_DRAGONFLY], [test "x$ac_system" = "xDragonFly"])
])
