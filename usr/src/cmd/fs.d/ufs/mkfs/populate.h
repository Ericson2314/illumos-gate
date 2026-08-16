/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

#ifndef	_POPULATE_H
#define	_POPULATE_H

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * Fill a newly made UFS filesystem from a directory tree.
 *
 * `fd` must be open for reading and writing on a filesystem mkfs has finished
 * writing, with all of its own writes already flushed -- this reads the
 * superblock back off the image and trusts what it finds.  `dir` is the root
 * of the tree to copy in; its contents become the contents of the
 * filesystem's root directory.
 *
 * Errors are fatal: anything that goes wrong here leaves a half-built
 * filesystem, which is not something a caller can do anything useful with.
 */
extern void ufs_populate(int fd, const char *dir);

#ifdef	__cplusplus
}
#endif

#endif	/* _POPULATE_H */
