#!/bin/bash
# acltype=nfsv4 and aclmode=decoupled on Linux.  Run as root with the module
# loaded and zpool and zfs on PATH.  Each failure prints FAIL: and the run ends
# non-zero.
set -u
fails=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }
# ok DESC CMD...: CMD must succeed.  no DESC CMD...: it must fail.
ok() { d=$1; shift; if "$@"; then pass "$d"; else fail "$d"; fi; }
no() { d=$1; shift; if "$@" 2>/dev/null; then fail "$d"; else pass "$d"; fi; }
as() { u=$1; shift; setpriv --reuid "$u" --regid "$u" --clear-groups "$@"; }
mode() { stat -c %a "$1"; }

V=/var/tmp/nfs4acl.img
truncate -s 1G $V
zpool create -f tank $V
zfs create -o acltype=nfsv4 -o aclmode=decoupled -o xattr=sa tank/d
zfs create -o acltype=nfsv4 -o aclmode=discard tank/c
zfs create tank/off
chmod 755 /tank/d /tank/c /tank/off

cd /tank/d || exit 1
echo data > f
chmod 600 f
ok "getfacl works on a trivial file" nfs4_getfacl f
no "uid 1234 refused before the ACL" as 1234 cat f
ok "setfacl numeric user" nfs4_setfacl -a A::1234:r f
ok "ACL shows the entry" sh -c "nfs4_getfacl f | grep -q ':1234:'"
ok "listxattr shows it" python3 -c "import os;assert 'system.nfs4_acl' in os.listxattr('f')"
ok "uid 1234 reads through the ACL" as 1234 cat f
no "uid 1235 still refused" as 1235 cat f
no "uid 1234 cannot write" as 1234 sh -c 'echo x >> f'

a=$(nfs4_getfacl f)
chmod 000 f
[ "$(mode f)" = 0 ] && pass "chmod 000 sets the mode" || fail "chmod 000 mode $(mode f)"
[ "$(nfs4_getfacl f)" = "$a" ] && pass "chmod leaves the ACL" || fail "chmod changed the ACL"
ok "uid 1234 reads after chmod 000" as 1234 cat f
chmod 777 f
no "uid 1235 refused after chmod 777" as 1235 cat f
[ "$(mode f)" = 777 ] && pass "chmod 777 sets the mode" || fail "chmod 777 mode $(mode f)"

# Control: without decoupled the chmod has to rewrite the ACL.
cd /tank/c || exit 1
echo data > f
chmod 600 f
nfs4_setfacl -a A::1234:r f
chmod 600 f
no "control: aclmode=discard drops the ACE" as 1234 cat f
cd /tank/d || exit 1

# A trivial ACL stays coupled.
echo data > t
chmod 644 t
ok "trivial 644 readable" as 1235 cat t
chmod 600 t
no "trivial 600 refused" as 1235 cat t

# Inheritance and a group area.
mkdir share
chmod 700 share
nfs4_setfacl -a A:fd:1234:rwaDdxtTnNcCy share
ok "uid 1234 creates in a 700 directory" as 1234 sh -c 'echo x > share/n'
ok "uid 1234 mkdirs there" as 1234 mkdir share/sub
ok "uid 1234 lists" as 1234 ls share
no "uid 1235 cannot enter" as 1235 ls share
ok "child inherited" sh -c "nfs4_getfacl share/n | grep -q ':1234:'"
ok "uid 1234 removes" as 1234 rm share/n

# Deny beats the mode.
echo data > dn
chmod 644 dn
nfs4_setfacl -a D::1235:r dn
no "deny ACE refuses a mode-readable file" as 1235 cat dn
ok "others still read" as 1236 cat dn

# Capabilities.  A root without them gets what the ACL gives.
echo data > cap
chown 1235 cap
chmod 600 cap
nfs4_setfacl -a A::1234:r cap
ok "root with capabilities reads" cat cap
no "root without capabilities refused" \
    setpriv --bounding-set=-all --inh-caps=-all --ambient-caps=-all cat cap

# RFC 8881 6.2.1.5: no group flag on GROUP@, and ignored if a client sends it.
ok "GROUP@ encoded without the group flag" python3 -c "
import os,struct
os.setxattr('dn','system.nfs4_acl',struct.pack('>5I',1,0,0x40,1,6)+b'GROUP@\\0\\0')
b=os.getxattr('dn','system.nfs4_acl')
assert struct.unpack('>I',b[8:12])[0]==0 and b[20:26]==b'GROUP@'"

# Malformed values must fail cleanly.
cat > /var/tmp/nfs4acl-bad.py <<'PY'
import os, struct
f = 'f'
ace = struct.pack('>4I', 0, 0, 1, 6) + b'OWNER@\0\0'
good = struct.pack('>I', 1) + ace
keep = os.getxattr(f, 'system.nfs4_acl')
os.setxattr(f, 'system.nfs4_acl', good)
bad = [b'', b'\0', struct.pack('>I', 0), struct.pack('>I', 2) + ace,
    struct.pack('>I', 1), good + b'\0', struct.pack('>I', 0xffffffff),
    struct.pack('>I', 1025) + ace * 1025,
    struct.pack('>5I', 1, 9, 0, 1, 6) + b'OWNER@\0\0',
    struct.pack('>5I', 1, 0, 0x1000, 1, 6) + b'OWNER@\0\0',
    struct.pack('>5I', 1, 0, 0, 0x80000000, 6) + b'OWNER@\0\0',
    struct.pack('>5I', 1, 0, 0, 1, 0xffffffff) + b'OWNER@\0\0',
    struct.pack('>5I', 1, 0, 0, 1, 12) + b'alice@domain',
    struct.pack('>5I', 1, 0, 0, 1, 10) + b'4294967295\0\0']
for i, b in enumerate(bad):
    try:
        os.setxattr(f, 'system.nfs4_acl', b)
    except OSError:
        continue
    raise SystemExit(f'bad value {i} accepted')
os.setxattr(f, 'system.nfs4_acl', keep)
PY
ok "malformed ACLs refused" python3 /var/tmp/nfs4acl-bad.py
rm -f /var/tmp/nfs4acl-bad.py
ok "group entry round trips" sh -c "nfs4_setfacl -a A:g:1500:r cap && nfs4_getfacl cap | grep -q ':1500:'"

# Idmapped mount: access follows the mapping, setting is refused.  Which id
# of the option is which is taken from what the mount shows.
mkdir -p /mnt/id
: > idown
chown 1234:1234 idown
M=
for spec in b:1234:5000:1 b:5000:1234:1; do
	mount --bind -o X-mount.idmap=$spec /tank/d /mnt/id || { umount /mnt/id 2>/dev/null; continue; }
	if [ "$(stat -c %u /mnt/id/idown)" = 5000 ]; then
		M=$spec
		break
	fi
	umount /mnt/id
done
if [ -n "$M" ]; then
	echo "idmap $M"
	ok "idmapped: mapped uid reads" as 5000 cat /mnt/id/cap
	no "idmapped: unmapped uid refused" as 1234 cat /mnt/id/cap
	no "idmapped: setting refused" nfs4_setfacl -a A::1:r /mnt/id/cap
	ok "idmapped: raw read still works" nfs4_getfacl /mnt/id/cap
	umount /mnt/id
else
	fail "no idmapped mount showed uid 5000"
fi

# off is what it was.
cd /tank/off || exit 1
echo data > f
no "acltype=off has no nfs4_acl" python3 -c "import os;os.getxattr('f','system.nfs4_acl')"
chmod 600 f
no "acltype=off goes by the mode" as 1234 cat f

# The ACL is on disk and not only cached.
cd /
zfs unmount tank/d && zfs mount tank/d
ok "ACL after remount" as 1234 cat /tank/d/f
zpool export tank && zpool import -d /var/tmp tank
ok "ACL after import" as 1234 cat /tank/d/f
zpool destroy tank
rm -f $V
echo "fails=$fails"
[ "$fails" = 0 ]
