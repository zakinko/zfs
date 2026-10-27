// SPDX-License-Identifier: CDDL-1.0
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * https://opensource.org/license/CDDL-1.0.
 */

/*
 * illumos's privilege checks, through kauth(9).
 *
 * Administering pools and datasets, and injecting faults, are what
 * Linux asks CAP_SYS_ADMIN for and FreeBSD a privilege of their own;
 * NetBSD has no finer action for them than the superuser's, which is
 * also what its own ZFS asked.  A check on a vnode goes to
 * kauth_authorize_vnode() with the file system's own answer as "no",
 * so that the secmodel can grant it, as NetBSD's file systems do; the
 * answer is the errno a file system of its own gives, from genfs.
 *
 * Each of these is asked only once the file's ACL or mode has refused,
 * so it decides whether privilege overrides the refusal.  The mount's
 * owner is granted what the superuser is when vfs.zfs.super_owner is
 * set, as on FreeBSD.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/kauth.h>
#include <sys/vnode.h>
#include <sys/policy.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/zfs_znode.h>
#include <sys/zfs_vfsops.h>

int
secpolicy_zfs(cred_t *cr)
{

	return (kauth_authorize_generic(cr, KAUTH_GENERIC_ISSUSER, NULL));
}

int
secpolicy_sys_config(cred_t *cr, int checkonly)
{

	(void) checkonly;
	return (kauth_authorize_generic(cr, KAUTH_GENERIC_ISSUSER, NULL));
}

int
secpolicy_zinject(cred_t *cr)
{

	return (kauth_authorize_generic(cr, KAUTH_GENERIC_ISSUSER, NULL));
}

int
secpolicy_fs_owner(struct mount *mp, cred_t *cr)
{

	if (zfs_super_owner &&
	    kauth_cred_geteuid(cr) == mp->mnt_stat.f_owner)
		return (0);
	return (SET_ERROR(EPERM));
}

int
secpolicy_fs_mount(cred_t *cr, vnode_t *mvp, struct mount *vfsp)
{

	return (kauth_authorize_system(cr, KAUTH_SYSTEM_MOUNT,
	    KAUTH_REQ_SYSTEM_MOUNT_NEW, mvp, NULL, NULL));
}

int
secpolicy_fs_unmount(cred_t *cr, struct mount *vfsp)
{

	return (kauth_authorize_system(cr, KAUTH_SYSTEM_MOUNT,
	    KAUTH_REQ_SYSTEM_MOUNT_UNMOUNT, vfsp, NULL, NULL));
}

/*
 * A mount by a user who may not mount set-id programs and devices is
 * made nosuid and nodev, as mount_domount() makes a usermount.
 */
void
secpolicy_fs_mount_clearopts(cred_t *cr, struct mount *vfsp)
{

	if (kauth_authorize_generic(cr, KAUTH_GENERIC_ISSUSER, NULL) != 0)
		vfsp->mnt_flag |= MNT_NOSUID | MNT_NODEV;
}

/* sys_link() has asked the secmodel already. */
int
secpolicy_basic_link(vnode_t *vp, cred_t *cr)
{

	(void) vp, (void) cr;
	return (0);
}

/* Removing or renaming another's file from a sticky directory. */
int
secpolicy_vnode_remove(vnode_t *vp, cred_t *cr)
{

	if (secpolicy_fs_owner(vp->v_mount, cr) == 0)
		return (0);
	return (kauth_authorize_vnode(cr, KAUTH_VNODE_DELETE, vp, NULL,
	    EPERM));
}

/* The same, where illumos has no override: ZFS's own answer stands. */
int
secpolicy_vnode_stky_modify(cred_t *cr)
{

	(void) cr;
	return (SET_ERROR(EPERM));
}

/*
 * The access the file's ACL or mode refused.  The superuser may not
 * execute a file with no execute bit; KAUTH_VNODE_IS_EXEC says whether
 * this one has one.
 */
int
secpolicy_vnode_access(cred_t *cr, vnode_t *vp, uid_t owner,
    accmode_t accmode)
{
	znode_t *zp = VTOZ(vp);

	(void) owner;
	if (accmode == 0)
		return (0);
	if (secpolicy_fs_owner(vp->v_mount, cr) == 0)
		return (0);
	return (kauth_authorize_vnode(cr, KAUTH_ACCESS_ACTION(accmode,
	    vp->v_type, zp != NULL ? (mode_t)zp->z_mode : 0), vp, NULL,
	    EACCES));
}

int
secpolicy_vnode_access2(cred_t *cr, vnode_t *vp, uid_t owner,
    accmode_t curmode, accmode_t wantmode)
{

	return (secpolicy_vnode_access(cr, vp, owner, ~curmode & wantmode));
}

/* Whether the caller may have any access to the file at all. */
int
secpolicy_vnode_any_access(cred_t *cr, vnode_t *vp, uid_t owner)
{

	if (owner == kauth_cred_geteuid(cr))
		return (0);
	return (secpolicy_vnode_access(cr, vp, owner, VREAD));
}

/* Changing the mode or the ACL, which the owner may. */
int
secpolicy_vnode_setdac(vnode_t *vp, cred_t *cr, uid_t owner)
{

	if (owner == kauth_cred_geteuid(cr))
		return (0);
	if (secpolicy_fs_owner(vp->v_mount, cr) == 0)
		return (0);
	return (kauth_authorize_vnode(cr, KAUTH_VNODE_WRITE_SECURITY, vp, NULL,
	    EPERM));
}

int
secpolicy_vnode_owner(vnode_t *vp, cred_t *cr, uid_t owner)
{

	return (secpolicy_vnode_setdac(vp, cr, owner));
}

int
secpolicy_vnode_chown(vnode_t *vp, cred_t *cr, uid_t owner)
{

	(void) owner;
	if (secpolicy_fs_owner(vp->v_mount, cr) == 0)
		return (0);
	return (kauth_authorize_vnode(cr, KAUTH_VNODE_CHANGE_OWNERSHIP, vp,
	    NULL, EPERM));
}

/* Creating a file in a group the caller is not in; illumos's answer. */
int
secpolicy_vnode_create_gid(cred_t *cr)
{

	(void) cr;
	return (SET_ERROR(EPERM));
}

int
secpolicy_vnode_setids_setgids(vnode_t *vp, cred_t *cr, gid_t gid)
{

	if (groupmember(gid, cr))
		return (0);
	if (secpolicy_fs_owner(vp->v_mount, cr) == 0)
		return (0);
	return (kauth_authorize_vnode(cr, KAUTH_VNODE_WRITE_SECURITY, vp, NULL,
	    EPERM));
}

/* Clear the set-id bits of a file changing hands, unless privileged. */
void
secpolicy_setid_clear(struct vattr *vap, vnode_t *vp, cred_t *cr)
{

	if (secpolicy_fs_owner(vp->v_mount, cr) == 0)
		return;
	if ((vap->va_mode & (S_ISUID | S_ISGID)) == 0)
		return;
	if (kauth_authorize_vnode(cr, KAUTH_VNODE_RETAIN_SUID |
	    KAUTH_VNODE_RETAIN_SGID, vp, NULL, EPERM) != 0) {
		vap->va_mask |= AT_MODE;
		vap->va_mode &= ~(S_ISUID | S_ISGID);
	}
}

/*
 * Setting the sticky bit on a file that is not a directory, the set-gid
 * bit in a group the caller is not in, or the set-uid bit on a file the
 * caller does not own: genfs_can_chmod()'s rules.
 */
int
secpolicy_setid_setsticky_clear(vnode_t *vp, struct vattr *vap,
    const struct vattr *ovap, cred_t *cr)
{
	int error;

	if (secpolicy_fs_owner(vp->v_mount, cr) == 0)
		return (0);
	if (vp->v_type != VDIR && (vap->va_mode & S_ISTXT) &&
	    (error = kauth_authorize_vnode(cr, KAUTH_VNODE_WRITE_SECURITY, vp,
	    NULL, EFTYPE)) != 0)
		return (error);
	if ((vap->va_mode & S_ISGID) != 0 &&
	    (error = secpolicy_vnode_setids_setgids(vp, cr, ovap->va_gid)) != 0)
		return (error);
	if ((vap->va_mode & S_ISUID) != 0 &&
	    ovap->va_uid != kauth_cred_geteuid(cr) &&
	    (error = kauth_authorize_vnode(cr, KAUTH_VNODE_WRITE_SECURITY, vp,
	    NULL, EPERM)) != 0)
		return (error);
	return (0);
}

int
secpolicy_vnode_setattr(cred_t *cr, vnode_t *vp, struct vattr *vap,
    const struct vattr *ovap, int flags,
    int unlocked_access(void *, int, cred_t *), void *node)
{
	int mask = vap->va_mask;
	int error;

	(void) flags;
	if (mask & AT_SIZE) {
		if (vp->v_type == VDIR)
			return (SET_ERROR(EISDIR));
		if ((error = unlocked_access(node, VWRITE, cr)) != 0)
			return (error);
	}
	if (mask & AT_MODE) {
		if ((error = secpolicy_vnode_setdac(vp, cr, ovap->va_uid)) != 0)
			return (error);
		error = secpolicy_setid_setsticky_clear(vp, vap, ovap, cr);
		if (error != 0)
			return (error);
	} else {
		vap->va_mode = ovap->va_mode;
	}
	if (mask & (AT_UID | AT_GID)) {
		if ((error = secpolicy_vnode_setdac(vp, cr, ovap->va_uid)) != 0)
			return (error);
		/*
		 * Giving the file away, or to a group the caller is not
		 * in, takes privilege.
		 */
		if (((mask & AT_UID) && vap->va_uid != ovap->va_uid) ||
		    ((mask & AT_GID) && vap->va_gid != ovap->va_gid &&
		    !groupmember(vap->va_gid, cr))) {
			error = secpolicy_vnode_chown(vp, cr, ovap->va_uid);
			if (error != 0)
				return (error);
		}
		if (((mask & AT_UID) && vap->va_uid != ovap->va_uid) ||
		    ((mask & AT_GID) && vap->va_gid != ovap->va_gid))
			secpolicy_setid_clear(vap, vp, cr);
	}
	if (mask & (AT_ATIME | AT_MTIME)) {
		/*
		 * utimes(2): the owner may set the times; with no times
		 * given, so may anyone who may write the file.
		 */
		error = secpolicy_vnode_setdac(vp, cr, ovap->va_uid);
		if (error != 0 && (vap->va_vaflags & VA_UTIMES_NULL))
			error = unlocked_access(node, VWRITE, cr);
		if (error != 0)
			return (error);
	}
	return (0);
}

/* The immutable, append-only and the other flags a user may not set. */
int
secpolicy_xvattr(vnode_t *vp, xvattr_t *xvap, uid_t owner, cred_t *cr,
    vtype_t vtype)
{

	(void) xvap, (void) owner, (void) vtype;
	if (secpolicy_fs_owner(vp->v_mount, cr) == 0)
		return (0);
	return (kauth_authorize_vnode(cr, KAUTH_VNODE_WRITE_SYSFLAGS, vp, NULL,
	    EPERM));
}

/* There is no SMB sharing to administer. */
int
secpolicy_smb(cred_t *cr)
{

	(void) cr;
	return (SET_ERROR(EPERM));
}

/* Whether a write may leave the set-id bits on the file it writes. */
int
secpolicy_vnode_setid_retain(struct znode *zp, cred_t *cr,
    boolean_t issuidroot)
{

	(void) issuidroot;
	if (secpolicy_fs_owner(ZTOV(zp)->v_mount, cr) == 0)
		return (0);
	return (kauth_authorize_vnode(cr, KAUTH_VNODE_RETAIN_SUID |
	    KAUTH_VNODE_RETAIN_SGID, ZTOV(zp), NULL, EPERM));
}
