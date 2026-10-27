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
 * The module's tunables as sysctl nodes; see <sys/mod.h>.
 *
 * A tunable is vfs.zfs.<scope>.<name>, its scope split at each
 * underscore, as FreeBSD names them.  sysctl_createv() hands back the
 * node that is already there when asked to create one of the same name
 * again, so a scope's nodes are created by whichever of its tunables
 * comes first and shared by the rest.  Every node goes on the module's
 * sysctl log, and kern_module.c takes them all down at unload.
 *
 * A tunable with handlers is a string node: reading it prints what the
 * get handler writes, and writing it hands the new string to the set
 * handler.  The handlers have the Linux shape, which returns a negative
 * errno.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kmem.h>
#include <sys/queue.h>
#include <sys/sysctl.h>
#include <sys/mod.h>
#include <sys/sunddi.h>
#include <sys/debug.h>

struct spl_param {
	LIST_ENTRY(spl_param)	sp_link;
	zfs_kernel_param_t	sp_kp;
	spl_param_set_t		sp_set;
	spl_param_get_t		sp_get;
};

/*
 * The handlers' state, freed at unload.  It is added to only from the
 * module's SYSCTL_SETUP functions, which kern_module.c runs one after
 * another while it loads the module and before its modcmd, so nothing
 * here is shared and there is no lock to initialise yet.
 */
static LIST_HEAD(, spl_param) spl_params = LIST_HEAD_INITIALIZER(spl_params);

/*
 * sysctl_createv() is called by its own name, past the macro of the
 * same name in <sys/sysctl.h>: that macro checks the data pointer's
 * type against a CTLTYPE_ spelled out at the call, and here the type is
 * a variable, or the data is a handler's state.  Each tunable's type
 * and size come together from SPL_PARAM_TYPE_* in <sys/mod.h>.
 */
const struct sysctlnode *
spl_sysctl_scope(struct sysctllog **clog, const char *scope)
{
	const struct sysctlnode *node = NULL;
	char comp[SYSCTL_NAMELEN];
	const char *p, *q;

	(void) (sysctl_createv)(clog, 0, NULL, &node, 0, CTLTYPE_NODE, "vfs",
	    NULL, NULL, 0, NULL, 0, CTL_VFS, CTL_EOL);
	for (p = scope; *p != '\0'; p = (*q == '_') ? q + 1 : q) {
		size_t len;

		for (q = p; *q != '\0' && *q != '_'; q++)
			continue;
		len = MIN((size_t)(q - p), sizeof (comp) - 1);
		memcpy(comp, p, len);
		comp[len] = '\0';
		if ((sysctl_createv)(clog, 0, &node, &node, 0, CTLTYPE_NODE,
		    comp, NULL, NULL, 0, NULL, 0, CTL_CREATE, CTL_EOL) != 0)
			return (NULL);
	}
	return (node);
}

void
spl_sysctl_param(struct sysctllog **clog, const char *scope, const char *leaf,
    int type, int flags, void *data, size_t size, const char *desc)
{
	const struct sysctlnode *node;

	if ((node = spl_sysctl_scope(clog, scope)) == NULL)
		return;
	(void) (sysctl_createv)(clog, 0, &node, NULL, flags, type, leaf, desc,
	    NULL, 0, data, size, CTL_CREATE, CTL_EOL);
}

static int
spl_sysctl_handler(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	struct spl_param *sp = rnode->sysctl_data;
	char *buf;
	size_t len;
	int error;

	buf = kmem_zalloc(PAGE_SIZE, KM_SLEEP);
	if (sp->sp_get != NULL)
		(void) sp->sp_get(buf, &sp->sp_kp);
	/* The Linux handlers end what they print with a newline. */
	len = strlen(buf);
	if (len > 0 && buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	node.sysctl_data = buf;
	node.sysctl_size = PAGE_SIZE;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error != 0 || newp == NULL)
		goto out;
	if (sp->sp_set == NULL) {
		error = EPERM;
		goto out;
	}
	error = sp->sp_set(buf, &sp->sp_kp);
	if (error < 0)
		error = -error;
out:
	kmem_free(buf, PAGE_SIZE);
	return (error);
}

void
spl_sysctl_param_call(struct sysctllog **clog, const char *scope,
    const char *leaf, int perm, spl_param_set_t set, spl_param_get_t get,
    void *var, const char *desc)
{
	const struct sysctlnode *node;
	struct spl_param *sp;

	if ((node = spl_sysctl_scope(clog, scope)) == NULL)
		return;
	sp = kmem_zalloc(sizeof (*sp), KM_SLEEP);
	sp->sp_kp.zkp_name = leaf;
	sp->sp_kp.zkp_arg = var;
	sp->sp_set = set;
	sp->sp_get = get;
	LIST_INSERT_HEAD(&spl_params, sp, sp_link);
	(void) (sysctl_createv)(clog, 0, &node, NULL, perm, CTLTYPE_STRING, leaf,
	    desc, spl_sysctl_handler, 0, (void *)sp, 0, CTL_CREATE, CTL_EOL);
}

/* Linux's generic handlers. */
int
param_get_uint(char *buf, zfs_kernel_param_t *kp)
{

	return (snprintf(buf, PAGE_SIZE, "%u\n", *(uint_t *)kp->zkp_arg));
}

int
param_set_uint(const char *buf, zfs_kernel_param_t *kp)
{
	unsigned long long v;
	int error;

	error = ddi_strtoull(buf, NULL, 0, &v);
	if (error != 0)
		return (-error);
	if (v > UINT_MAX)
		return (-ERANGE);
	*(uint_t *)kp->zkp_arg = (uint_t)v;
	return (0);
}

int
spl_param_get_u64(char *buf, zfs_kernel_param_t *kp)
{

	return (snprintf(buf, PAGE_SIZE, "%llu\n",
	    (unsigned long long)*(uint64_t *)kp->zkp_arg));
}

int
param_get_charp(char *buf, zfs_kernel_param_t *kp)
{
	const char *s = *(const char **)kp->zkp_arg;

	return (snprintf(buf, PAGE_SIZE, "%s\n", s != NULL ? s : ""));
}

/* The nodes are gone by now; their handlers' state goes with them. */
void
spl_sysctl_fini(void)
{
	struct spl_param *sp;

	while ((sp = LIST_FIRST(&spl_params)) != NULL) {
		LIST_REMOVE(sp, sp_link);
		kmem_free(sp, sizeof (*sp));
	}
}
