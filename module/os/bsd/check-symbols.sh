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
#
# Nor will it load one that uses a symbol none of them defines ("linker
# error: symbol `x' not found"), which the link of a module does not
# catch, a module being linked with -r.  Those are printed too.
set -eu
kmod=$1
shift
# nm in a pipeline fails without failing the pipeline.
[ -f "$kmod" ] || { echo "$kmod: no such file" >&2; exit 1; }
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
nm -u "$kmod" | awk '{print $2}' | sort -u > "$tmp/undef"
comm -12 "$tmp/mod" "$tmp/other" > "$tmp/both"
comm -23 "$tmp/undef" "$tmp/other" > "$tmp/missing"
rc=0
if [ -s "$tmp/both" ]; then
	echo "globals zfs.kmod defines that are already defined:"
	cat "$tmp/both"
	rc=1
fi
if [ -s "$tmp/missing" ]; then
	echo "symbols zfs.kmod uses that nothing defines:"
	cat "$tmp/missing"
	rc=1
fi
[ $rc -eq 0 ] &&
    echo "no global of zfs.kmod is defined elsewhere, and none is missing"
exit $rc
