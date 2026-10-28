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
 * mount_zfs [-o options] dataset node
 *
 * NetBSD's mount(8) runs mount_<type> for a file system it does not
 * know, so "mount -t zfs pool/fs /mnt", an fstab line of type zfs, and
 * a dataset with mountpoint=legacy all come here.  The standard
 * options become mount(2) flags, as for any file system, and the whole
 * option string also goes to the kernel, which takes the ZFS ones from
 * it as zfs mount does.
 */

#include <sys/param.h>
#include <sys/mount.h>
#include <err.h>
#include <errno.h>
#include <mntopts.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <os/bsd/zfs/sys/zfs_mount_args.h>

static const struct mntopt mopts[] = {
	MOPT_STDOPTS,
	MOPT_UPDATE,
	MOPT_FORCE,
	MOPT_GETARGS,
	MOPT_NULL,
};

static void
usage(void)
{

	(void) fprintf(stderr,
	    "usage: mount_zfs [-o options] dataset node\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	struct zfs_args za;
	char node[MAXPATHLEN], opts[MAXPATHLEN];
	mntoptparse_t mp;
	char *buf, *o, *last;
	int ch, mntflags = 0, altflags = 0;

	opts[0] = '\0';
	/*
	 * getmntopts() gives up on the whole string at the first option
	 * it does not know, so each goes to it alone; one it does not know
	 * is ZFS's, for the kernel.
	 */
	getmnt_silent = 1;
	while ((ch = getopt(argc, argv, "o:")) != -1) {
		switch (ch) {
		case 'o':
			if ((buf = strdup(optarg)) == NULL)
				err(1, "strdup");
			for (o = strtok_r(buf, ",", &last); o != NULL;
			    o = strtok_r(NULL, ",", &last)) {
				if ((mp = getmntopts(o, mopts, &mntflags,
				    &altflags)) != NULL)
					freemntopts(mp);
			}
			free(buf);
			if (opts[0] != '\0')
				(void) strlcat(opts, ",", sizeof (opts));
			if (strlcat(opts, optarg, sizeof (opts)) >=
			    sizeof (opts))
				errx(1, "options too long");
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc != 2)
		usage();

	if (realpath(argv[1], node) == NULL)
		err(1, "%s", argv[1]);

	memset(&za, 0, sizeof (za));
	if (strlcpy(za.fspec, argv[0], sizeof (za.fspec)) >=
	    sizeof (za.fspec))
		errx(1, "%s: name too long", argv[0]);
	(void) strlcpy(za.optptr, opts, sizeof (za.optptr));
	za.optlen = (int)strlen(za.optptr);
	za.mflag = mntflags;

	if (mount(MOUNT_ZFS, node, mntflags, &za, sizeof (za)) == -1)
		err(1, "%s on %s", argv[0], node);
	return (0);
}
