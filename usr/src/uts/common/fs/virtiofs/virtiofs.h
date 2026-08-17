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

/*
 * Copyright 2026 John Ericson
 */

#ifndef	_FS_VIRTIOFS_VIRTIOFS_H
#define	_FS_VIRTIOFS_VIRTIOFS_H

#include <sys/types.h>
#include <sys/vnode.h>
#include <sys/vfs.h>
#include <sys/list.h>
#include <sys/ksynch.h>
#include <sys/avl.h>

#include "fuse.h"
#include "vtfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The largest read we will ask the server for in one go.  This also bounds the
 * kluster that VOP_GETPAGE assembles, and hence the number of scatter-gather
 * cookies one request can need.
 */
#define	VIRTIOFS_MAXBSIZE	(64 * 1024)

/*
 * The size of the buffer we hand to FUSE_READDIR.  One buffer's worth of
 * fuse_dirent records is decoded into the caller's dirent64 buffer per pass.
 */
#define	VIRTIOFS_DIRBUFSZ	(8 * 1024)

/*
 * Node hash.  Nodes are keyed by the server's nodeid, which is unique and
 * stable for the lifetime of the reference we hold on it.
 */
#define	VIRTIOFS_NHASH		251

typedef struct virtiofs_node vfnode_t;
typedef struct virtiofs vfsmnt_t;

struct virtiofs_node {
	vfnode_t	*vfn_hash;	/* next in hash bucket */
	vnode_t		*vfn_vnode;
	vfsmnt_t	*vfn_mnt;

	uint64_t	vfn_nodeid;	/* FUSE nodeid */
	uint64_t	vfn_gen;	/* FUSE generation */

	/*
	 * How many successful FUSE_LOOKUPs we have done for this node.  The
	 * server keeps a matching count, and VOP_INACTIVE must return all of
	 * them at once with a single FUSE_FORGET.
	 */
	uint64_t	vfn_nlookup;

	kmutex_t	vfn_lock;	/* covers everything below */
	struct fuse_attr vfn_attr;	/* last known attributes */
	hrtime_t	vfn_attr_expiry; /* when vfn_attr goes stale */
	boolean_t	vfn_attr_valid;

	uint64_t	vfn_fh;		/* server file handle, when open */
	uint_t		vfn_fh_count;	/* opens that share vfn_fh */
	boolean_t	vfn_fh_valid;

	uint_t		vfn_mapcnt;	/* pages currently mapped */
};

struct virtiofs {
	vfs_t		*vfs_vfsp;
	vtfs_t		*vfs_dev;
	vnode_t		*vfs_rootvp;
	dev_t		vfs_dev_no;	/* synthetic, for st_dev */

	kmutex_t	vfs_lock;
	uint64_t	vfs_unique;	/* next FUSE request id */

	krwlock_t	vfs_hash_lock;
	vfnode_t	*vfs_hash[VIRTIOFS_NHASH];

	/*
	 * Negotiated at FUSE_INIT time.
	 */
	uint32_t	vfs_major;
	uint32_t	vfs_minor;
	uint32_t	vfs_max_write;
	uint32_t	vfs_max_read;

	/*
	 * Mount options.  The server reports whatever uid and gid it likes;
	 * on a store mount those are the host's, which need not mean anything
	 * locally, so they can be overridden wholesale.
	 */
	boolean_t	vfs_override_ids;
	uid_t		vfs_uid;
	gid_t		vfs_gid;
};

#define	VFS_TO_VIRTIOFS(vfsp)	((vfsmnt_t *)((vfsp)->vfs_data))
#define	VTOVF(vp)		((vfnode_t *)((vp)->v_data))
#define	VFTOV(vfn)		((vfn)->vfn_vnode)

/*
 * The file identifier we hand out for NFS export and for VFS_VGET.  A nodeid
 * is only meaningful while we hold a lookup count on it, so this is not a
 * durable handle; it is enough for the lifetime of a mount.
 */
typedef struct virtiofs_fid {
	uint16_t	vfid_len;
	uint16_t	vfid_pad;
	uint32_t	vfid_gen;
	uint64_t	vfid_nodeid;
} vfid_t;

extern struct vnodeops *virtiofs_vnodeops;
extern const fs_operation_def_t virtiofs_vnodeops_template[];

/*
 * virtiofs_subr.c: the FUSE operations themselves, plus the node cache.
 */
extern int virtiofs_errno(int32_t);
extern int virtiofs_fuse_init(vfsmnt_t *);
extern int virtiofs_fuse_lookup(vfsmnt_t *, uint64_t, const char *,
    struct fuse_entry_out *);
extern void virtiofs_fuse_forget(vfsmnt_t *, uint64_t, uint64_t);
extern int virtiofs_fuse_getattr(vfsmnt_t *, uint64_t, struct fuse_attr_out *);
extern int virtiofs_fuse_open(vfsmnt_t *, uint64_t, boolean_t, uint64_t *);
extern int virtiofs_fuse_release(vfsmnt_t *, uint64_t, uint64_t, boolean_t);
extern int virtiofs_fuse_read(vfsmnt_t *, uint64_t, uint64_t, u_offset_t,
    void *, size_t, size_t *);
extern int virtiofs_fuse_readdir(vfsmnt_t *, uint64_t, uint64_t, u_offset_t,
    void *, size_t, size_t *);
extern int virtiofs_fuse_readlink(vfsmnt_t *, uint64_t, char *, size_t,
    size_t *);
extern int virtiofs_fuse_statfs(vfsmnt_t *, struct fuse_kstatfs *);

extern vfnode_t *virtiofs_node_hold(vfsmnt_t *, uint64_t,
    const struct fuse_attr *, uint64_t);
extern vfnode_t *virtiofs_node_find(vfsmnt_t *, uint64_t);
extern void virtiofs_node_free(vfnode_t *);
extern void virtiofs_node_setattr(vfnode_t *, const struct fuse_attr *,
    uint64_t, uint32_t);
extern int virtiofs_node_getattr(vfnode_t *, struct fuse_attr *);
extern void virtiofs_attr_to_vattr(vfsmnt_t *, const struct fuse_attr *,
    vattr_t *);
extern enum vtype virtiofs_mode_to_vtype(uint32_t);
extern int virtiofs_putapage(vnode_t *, page_t *, u_offset_t *, size_t *,
    int, cred_t *);
extern int virtiofs_openfh(vfnode_t *, boolean_t, uint64_t *);
extern void virtiofs_closefh(vfnode_t *, boolean_t);

#ifdef __cplusplus
}
#endif

#endif	/* _FS_VIRTIOFS_VIRTIOFS_H */
