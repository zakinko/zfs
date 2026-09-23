dnl #
dnl # Some C libraries declare struct xdr_bytesrec and XDR_GET_BYTES_AVAIL
dnl # themselves, in which case libspl must not declare them again.
dnl #
AC_DEFUN([ZFS_AC_CONFIG_USER_XDR_BYTESREC], [
	AC_CHECK_TYPE([struct xdr_bytesrec], [
		AC_DEFINE([HAVE_XDR_BYTESREC], 1,
		    [struct xdr_bytesrec is declared in rpc/xdr.h])
	], [], [[
		#include <rpc/types.h>
		#include <rpc/xdr.h>
	]])
])
