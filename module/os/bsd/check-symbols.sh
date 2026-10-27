#!/bin/sh
#
# check-symbols.sh zfs.kmod [kernel [module ...]]
#
# NetBSD will not load a module that defines a global symbol the kernel
# or a loaded module already defines (kobj_checksyms() in subr_kobj.c),
# and says only "global symbol `x' redefined" when it is tried.  This
# finds such a symbol before then: it prints every global zfs.kmod
# defines that the kernel (by default /netbsd) or any module named
# also defines, and fails if there is one.  The names that clash are
# renamed in include/os/bsd/spl/sys/namespace.h.
set -eu
kmod=$1
shift
kernel=${1:-/netbsd}
[ $# -gt 0 ] && shift
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
nm -g --defined-only "$kmod" | awk '{print $3}' | sort -u > "$tmp/mod"
{
	nm -g --defined-only "$kernel"
	for m in "$@"; do
		nm -g --defined-only "$m"
	done
} | awk '{print $3}' | sort -u > "$tmp/other"
comm -12 "$tmp/mod" "$tmp/other" > "$tmp/both"
if [ -s "$tmp/both" ]; then
	echo "globals zfs.kmod defines that are already defined:"
	cat "$tmp/both"
	exit 1
fi
echo "no global of zfs.kmod is defined elsewhere"
