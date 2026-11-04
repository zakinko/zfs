#!/bin/sh
# Run inside the VM: NFSv4 ACLs from nfsd down the typed operations into ZFS.
set -u
Z=$1
uname -r
PATH=$Z:$PATH
insmod $Z/module/spl.ko || { echo "FAIL: spl"; exit 1; }
insmod $Z/module/zfs.ko || { echo "FAIL: zfs"; exit 1; }
B=/run/zn
mkdir -p $B
mount -t tmpfs tmpfs $B
mkdir $B/a
truncate -s 1G $B/img
zpool create -f -m $B/tank tank $B/img || { echo "FAIL: pool"; exit 1; }
zfs create -o acltype=nfsv4 -o xattr=sa tank/d
: > $B/tank/d/f
chmod 640 $B/tank/d/f

mount -t tmpfs tmpfs /var/lib/nfs
mkdir -p /var/lib/nfs/rpc_pipefs /var/lib/nfs/v4recovery
touch /var/lib/nfs/etab /var/lib/nfs/rmtab
mount -t rpc_pipefs sunrpc /var/lib/nfs/rpc_pipefs
mount -t nfsd nfsd /proc/fs/nfsd
rpcbind -w 2>/dev/null || true
opts=rw,sync,no_root_squash,no_subtree_check
exportfs -o $opts,fsid=0,crossmnt 127.0.0.1:$B/tank
exportfs -o $opts,fsid=1 127.0.0.1:$B/tank/d
rpc.mountd
rpc.nfsd 4
mount -t nfs -o vers=4.2,nolock 127.0.0.1:/d $B/a || { echo "FAIL: mount"; exit 1; }

nfs4_setfacl -a A::1234:r $B/a/f
echo "rc=$?"
nfs4_getfacl $B/a/f
nfs4_getfacl $B/tank/d/f
if nfs4_getfacl $B/tank/d/f | grep -q ':1234:'; then
	echo "PASS: ACE set over NFS is in ZFS"
else
	echo "FAIL: ACE set over NFS is not in ZFS"
fi
if nfs4_getfacl $B/a/f | grep -q ':1234:'; then
	echo "PASS: ACE reads back over NFS"
else
	echo "FAIL: ACE does not read back over NFS"
fi
if setpriv --reuid 1234 --regid 1234 --clear-groups cat $B/tank/d/f; then
	echo "PASS: uid 1234 reads through the ACL"
else
	echo "FAIL: uid 1234 refused"
fi

# The xattr goes through the kernel's own handling; ZFS has no handler for it.
L=$B/tank/d/l
: > $L
as() { u=$1; shift; setpriv --reuid "$u" --regid "$u" --clear-groups "$@"; }
ck() { w=$1; d=$2; shift 2; if "$@"; then r=ok; else r=no; fi
	if [ $r = $w ]; then echo "PASS: $d"; else echo "FAIL: $d (got $r)"; fi; }
ck ok "local nfs4_setfacl through the VFS" nfs4_setfacl -a A::1234:r $L
ck ok "local nfs4_getfacl through the VFS" sh -c "nfs4_getfacl $L | grep -q ':1234:'"
ck no "unset: chmod refused without write_acl" as 1234 chmod 666 $L
nfs4_setfacl -a A::1234:C $L
ck ok "write_acl: a non-owner may chmod" as 1234 chmod 664 $L
echo "aclmode=$(zfs get -H -o value aclmode tank/d), ACL after chmod 664:"; nfs4_getfacl $L
# The default aclmode discards the ACE that gave write_acl.
ck no "aclmode=discard: chmod took write_acl away" as 1234 chmod 755 $L
zfs set aclmode=passthrough tank/d
nfs4_setfacl -a A::1234:C $L
ck ok "aclmode=passthrough: first chmod" as 1234 chmod 664 $L
echo "ACL after passthrough chmod:"; nfs4_getfacl $L
ck ok "aclmode=passthrough: write_acl survives chmod" as 1234 chmod 4755 $L
[ "$(stat -c %a $L)" = 755 ] && echo "PASS: S_ISUID not given by write_acl" || echo "FAIL: mode $(stat -c %a $L)"
ck no "chown to self refused without write_owner" as 1235 chown 1235 $L
nfs4_setfacl -a A::1235:o $L
ck no "write_owner gives no chown to a third uid" as 1235 chown 1236 $L
ck ok "write_owner takes ownership" as 1235 chown 1235 $L
[ "$(stat -c %u $L)" = 1235 ] && echo "PASS: owner is 1235" || echo "FAIL: owner $(stat -c %u $L)"
umount $B/a
