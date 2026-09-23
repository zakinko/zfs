dnl # SPDX-License-Identifier: CDDL-1.0
dnl #
dnl # Check if libintl and possibly libiconv are needed for gettext() functionality
dnl #
AC_DEFUN([ZFS_AC_CONFIG_USER_GETTEXT], [
    AM_GNU_GETTEXT([external])
    dnl #
    dnl # NetBSD's libintl is not GNU's, so the check above finds no GNU
    dnl # gettext and leaves LTLIBINTL empty.  The catalogues cannot be
    dnl # built there, but gettext(3) itself is present and the code calls
    dnl # it unconditionally, so the library still has to be linked.
    dnl #
    AC_SEARCH_LIBS([gettext], [intl])
])
