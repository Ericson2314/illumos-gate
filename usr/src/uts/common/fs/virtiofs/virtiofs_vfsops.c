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

/*
 * VIRTIO FS: VFS OPERATIONS
 *
 * A mount names a Virtio FS device by the tag that device advertises in its
 * configuration space:
 *
 *	mount -F virtiofs [-o ro,uid=0,gid=0] <tag> /mnt
 *
 * There is no block device and nothing to read a superblock out of; the mount
 * consists of finding the tagged device, negotiating a FUSE version with it,
 * and asking it for the attributes of the root nodeid.
 *
 * The file system is read-only, and not because the server is: a Virtio FS
 * device will happily accept writes.  There is simply no write path in this
 * module, so the mount asserts VFS_RDONLY and every vnode operation that would
 * modify anything is absent from the operation table.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/sysmacros.h>
#include <sys/cmn_err.h>
#include <sys/debug.h>
#include <sys/vnode.h>
#include <sys/vfs.h>
#include <sys/vfs_opreg.h>
#include <sys/mount.h>
#include <sys/mntent.h>
#include <sys/statvfs.h>
#include <sys/pathname.h>
#include <sys/policy.h>
#include <sys/modctl.h>
#include <sys/cred.h>
#include <sys/dnlc.h>
#include <sys/mkdev.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>

#include "fs/fs_subr.h"
#include "virtiofs.h"

/*
 * Mount options.  "uid" and "gid" replace whatever the server reports for
 * every file; on a host store mount the server's ids are the host's, which
 * need not correspond to anything in this system's name service.
 */
#define	VFOPT_UID	"uid"
#define	VFOPT_GID	"gid"

static char *ro_cancel[] = { MNTOPT_RW, NULL };

static mntopt_t virtiofs_options[] = {
	{ MNTOPT_RO, ro_cancel, NULL, MO_DEFAULT, NULL },
	{ VFOPT_UID, NULL, NULL, MO_HASVALUE, NULL },
	{ VFOPT_GID, NULL, NULL, MO_HASVALUE, NULL }
};

static mntopts_t virtiofs_proto_opttbl = {
	sizeof (virtiofs_options) / sizeof (mntopt_t),
	virtiofs_options
};

static int virtiofsinit(int, char *);

static vfsdef_t vfw = {
	VFSDEF_VERSION,
	"virtiofs",
	virtiofsinit,
	VSW_HASPROTO | VSW_STATS,
	&virtiofs_proto_opttbl
};

static struct modlfs modlfs = {
	&mod_fsops, "filesystem for VIRTIO FS", &vfw
};

static struct modlinkage modlinkage = {
	MODREV_1, { (void *)&modlfs, NULL }
};

static int virtiofsfstype;
static major_t virtiofs_major;
static minor_t virtiofs_minor;
static kmutex_t virtiofs_minor_lock;

static int virtiofs_mount(vfs_t *, vnode_t *, struct mounta *, cred_t *);
static int virtiofs_unmount(vfs_t *, int, cred_t *);
static int virtiofs_root(vfs_t *, vnode_t **);
static int virtiofs_statvfs(vfs_t *, struct statvfs64 *);
static int virtiofs_vget(vfs_t *, vnode_t **, struct fid *);

int
_init(void)
{
	return (mod_install(&modlinkage));
}

int
_fini(void)
{
	int error;

	if ((error = mod_remove(&modlinkage)) != 0)
		return (error);

	(void) vfs_freevfsops_by_type(virtiofsfstype);
	vn_freevnodeops(virtiofs_vnodeops);
	mutex_destroy(&virtiofs_minor_lock);

	return (0);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}

static int
virtiofsinit(int fstype, char *name)
{
	static const fs_operation_def_t virtiofs_vfsops_template[] = {
		VFSNAME_MOUNT,		{ .vfs_mount = virtiofs_mount },
		VFSNAME_UNMOUNT,	{ .vfs_unmount = virtiofs_unmount },
		VFSNAME_ROOT,		{ .vfs_root = virtiofs_root },
		VFSNAME_STATVFS,	{ .vfs_statvfs = virtiofs_statvfs },
		VFSNAME_VGET,		{ .vfs_vget = virtiofs_vget },
		NULL,			NULL
	};
	int error;

	error = vfs_setfsops(fstype, virtiofs_vfsops_template, NULL);
	if (error != 0) {
		cmn_err(CE_WARN, "virtiofsinit: bad vfs ops template");
		return (error);
	}

	error = vn_make_ops(name, virtiofs_vnodeops_template,
	    &virtiofs_vnodeops);
	if (error != 0) {
		(void) vfs_freevfsops_by_type(fstype);
		cmn_err(CE_WARN, "virtiofsinit: bad vnode ops template");
		return (error);
	}

	if ((virtiofs_major = getudev()) == (major_t)-1) {
		cmn_err(CE_WARN, "virtiofsinit: no unique device number");
		virtiofs_major = 0;
	}
	mutex_init(&virtiofs_minor_lock, NULL, MUTEX_DEFAULT, NULL);

	virtiofsfstype = fstype;

	return (0);
}

/*
 * Pick a device number that no other mount is using.  There is no real device
 * behind this file system, but stat(2) still has to report something unique
 * per mount for st_dev.
 */
static dev_t
virtiofs_alloc_dev(void)
{
	dev_t dev;

	mutex_enter(&virtiofs_minor_lock);
	do {
		virtiofs_minor = (virtiofs_minor + 1) & L_MAXMIN32;
		dev = makedevice(virtiofs_major, virtiofs_minor);
	} while (vfs_devismounted(dev));
	mutex_exit(&virtiofs_minor_lock);

	return (dev);
}

static int
virtiofs_mount(vfs_t *vfsp, vnode_t *mvp, struct mounta *uap, cred_t *cr)
{
	vfsmnt_t *vfs = NULL;
	vtfs_t *dev = NULL;
	vfnode_t *rootvfn;
	struct fuse_attr_out ao;
	struct pathname dpn;
	char *tag = NULL;
	char *optval;
	size_t taglen;
	int error;

	if ((error = secpolicy_fs_mount(cr, mvp, vfsp)) != 0)
		return (error);

	if (mvp->v_type != VDIR)
		return (ENOTDIR);

	mutex_enter(&mvp->v_lock);
	if ((uap->flags & MS_REMOUNT) == 0 && (uap->flags & MS_OVERLAY) == 0 &&
	    (mvp->v_count != 1 || (mvp->v_flag & VROOT))) {
		mutex_exit(&mvp->v_lock);
		return (EBUSY);
	}
	mutex_exit(&mvp->v_lock);

	/*
	 * We have no state that a remount could usefully change.
	 */
	if (uap->flags & MS_REMOUNT)
		return (ENOTSUP);

	/*
	 * The "special" is the device tag rather than a path, but it arrives
	 * the same way any mount's special does.
	 */
	tag = kmem_alloc(VIRTIO_FS_TAG_LEN + 1, KM_SLEEP);
	if (uap->flags & MS_SYSSPACE) {
		error = copystr(uap->spec, tag, VIRTIO_FS_TAG_LEN + 1, &taglen);
	} else {
		error = copyinstr(uap->spec, tag, VIRTIO_FS_TAG_LEN + 1,
		    &taglen);
	}
	if (error != 0) {
		kmem_free(tag, VIRTIO_FS_TAG_LEN + 1);
		return (error == ENAMETOOLONG ? ENAMETOOLONG : EFAULT);
	}
	tag[VIRTIO_FS_TAG_LEN] = '\0';

	error = pn_get(uap->dir, (uap->flags & MS_SYSSPACE) ? UIO_SYSSPACE :
	    UIO_USERSPACE, &dpn);
	if (error != 0) {
		kmem_free(tag, VIRTIO_FS_TAG_LEN + 1);
		return (error);
	}

	if ((dev = vtfs_hold_by_tag(tag)) == NULL) {
		cmn_err(CE_NOTE, "virtiofs: no Virtio FS device with tag "
		    "\"%s\"", tag);
		error = ENXIO;
		goto fail;
	}

	vfs = kmem_zalloc(sizeof (*vfs), KM_SLEEP);
	vfs->vfs_vfsp = vfsp;
	vfs->vfs_dev = dev;
	vfs->vfs_dev_no = virtiofs_alloc_dev();
	mutex_init(&vfs->vfs_lock, NULL, MUTEX_DEFAULT, NULL);
	rw_init(&vfs->vfs_hash_lock, NULL, RW_DEFAULT, NULL);

	if (vfs_optionisset(vfsp, VFOPT_UID, &optval)) {
		long v;

		if (ddi_strtol(optval, NULL, 10, &v) != 0 || v < 0) {
			error = EINVAL;
			goto fail;
		}
		vfs->vfs_uid = (uid_t)v;
		vfs->vfs_override_ids = B_TRUE;
	}
	if (vfs_optionisset(vfsp, VFOPT_GID, &optval)) {
		long v;

		if (ddi_strtol(optval, NULL, 10, &v) != 0 || v < 0) {
			error = EINVAL;
			goto fail;
		}
		vfs->vfs_gid = (gid_t)v;
		vfs->vfs_override_ids = B_TRUE;
	}

	vfsp->vfs_data = (caddr_t)vfs;
	vfsp->vfs_fstype = virtiofsfstype;
	vfsp->vfs_dev = vfs->vfs_dev_no;
	vfsp->vfs_bsize = MAXBSIZE;
	vfsp->vfs_flag |= VFS_RDONLY | VFS_NOTRUNC;
	vfs_make_fsid(&vfsp->vfs_fsid, vfs->vfs_dev_no, virtiofsfstype);
	vfs_setmntopt(vfsp, MNTOPT_RO, NULL, 0);

	/*
	 * Negotiate the protocol version before anything else; a server will
	 * not answer any other opcode until it has seen a FUSE_INIT.
	 */
	if ((error = virtiofs_fuse_init(vfs)) != 0) {
		cmn_err(CE_WARN, "virtiofs: FUSE_INIT failed on tag \"%s\": "
		    "%d", tag, error);
		goto fail;
	}

	bzero(&ao, sizeof (ao));
	if ((error = virtiofs_fuse_getattr(vfs, FUSE_ROOT_ID, &ao)) != 0) {
		cmn_err(CE_WARN, "virtiofs: cannot stat the root of tag "
		    "\"%s\": %d", tag, error);
		goto fail;
	}

	/*
	 * The root nodeid is never the result of a lookup, so there is no
	 * lookup count to hand over and none to forget later.
	 */
	rootvfn = virtiofs_node_hold(vfs, FUSE_ROOT_ID, &ao.attr, 0);
	virtiofs_node_setattr(rootvfn, &ao.attr, ao.attr_valid,
	    ao.attr_valid_nsec);

	vfs->vfs_rootvp = VFTOV(rootvfn);
	vfs->vfs_rootvp->v_flag |= VROOT;

	pn_free(&dpn);
	kmem_free(tag, VIRTIO_FS_TAG_LEN + 1);

	return (0);

fail:
	if (vfs != NULL) {
		mutex_destroy(&vfs->vfs_lock);
		rw_destroy(&vfs->vfs_hash_lock);
		kmem_free(vfs, sizeof (*vfs));
		vfsp->vfs_data = NULL;
	}
	if (dev != NULL)
		vtfs_rele(dev);
	pn_free(&dpn);
	kmem_free(tag, VIRTIO_FS_TAG_LEN + 1);

	return (error);
}

/* ARGSUSED */
static int
virtiofs_unmount(vfs_t *vfsp, int flag, cred_t *cr)
{
	vfsmnt_t *vfs = VFS_TO_VIRTIOFS(vfsp);
	vnode_t *rvp = vfs->vfs_rootvp;
	int error;

	if ((error = secpolicy_fs_unmount(cr, vfsp)) != 0)
		return (error);

	/*
	 * A forced unmount would have to invalidate every mapping of every
	 * file on the mount, which is more machinery than a read-only store
	 * mount justifies.
	 */
	if (flag & MS_FORCE)
		return (ENOTSUP);

	/*
	 * Purge the name cache first: entries there hold vnodes, and the
	 * busy check below would see them.
	 */
	(void) dnlc_purge_vfsp(vfsp, 0);

	if (rvp->v_count > 1) {
		/*
		 * Something other than us still has the root.
		 */
		return (EBUSY);
	}

	/*
	 * The root is the only node that may still be around; anything else
	 * means a file on this mount is still referenced somewhere.
	 */
	rw_enter(&vfs->vfs_hash_lock, RW_READER);
	for (uint_t i = 0; i < VIRTIOFS_NHASH; i++) {
		for (vfnode_t *vfn = vfs->vfs_hash[i]; vfn != NULL;
		    vfn = vfn->vfn_hash) {
			if (vfn != VTOVF(rvp)) {
				rw_exit(&vfs->vfs_hash_lock);
				return (EBUSY);
			}
		}
	}
	rw_exit(&vfs->vfs_hash_lock);

	/*
	 * The root is the one node that never carries a lookup count, so it
	 * can simply be released; VOP_INACTIVE unhashes and frees it.
	 */
	vfs->vfs_rootvp = NULL;
	VN_RELE(rvp);

	vtfs_rele(vfs->vfs_dev);

	rw_destroy(&vfs->vfs_hash_lock);
	mutex_destroy(&vfs->vfs_lock);
	kmem_free(vfs, sizeof (*vfs));
	vfsp->vfs_data = NULL;

	return (0);
}

static int
virtiofs_root(vfs_t *vfsp, vnode_t **vpp)
{
	vfsmnt_t *vfs = VFS_TO_VIRTIOFS(vfsp);

	if (vfs->vfs_rootvp == NULL)
		return (EIO);

	*vpp = vfs->vfs_rootvp;
	VN_HOLD(*vpp);

	return (0);
}

static int
virtiofs_statvfs(vfs_t *vfsp, struct statvfs64 *sbp)
{
	vfsmnt_t *vfs = VFS_TO_VIRTIOFS(vfsp);
	struct fuse_kstatfs ks;
	dev32_t d32;
	int error;

	bzero(&ks, sizeof (ks));
	if ((error = virtiofs_fuse_statfs(vfs, &ks)) != 0)
		return (error);

	bzero(sbp, sizeof (*sbp));
	sbp->f_bsize = ks.bsize != 0 ? ks.bsize : vfsp->vfs_bsize;
	sbp->f_frsize = ks.frsize != 0 ? ks.frsize : sbp->f_bsize;
	sbp->f_blocks = (fsblkcnt64_t)ks.blocks;

	/*
	 * The mount is read-only whatever the server says about free space, so
	 * report none: an application that asks whether it can write here
	 * should be told no in the way it is most likely to understand.
	 */
	sbp->f_bfree = 0;
	sbp->f_bavail = 0;
	sbp->f_files = (fsfilcnt64_t)ks.files;
	sbp->f_ffree = 0;
	sbp->f_favail = 0;

	(void) cmpldev(&d32, vfsp->vfs_dev);
	sbp->f_fsid = d32;
	(void) strcpy(sbp->f_basetype, vfssw[vfsp->vfs_fstype].vsw_name);
	sbp->f_flag = vf_to_stf(vfsp->vfs_flag);
	sbp->f_namemax = ks.namelen != 0 ? ks.namelen : MAXNAMELEN - 1;
	(void) strncpy(sbp->f_fstr, vtfs_tag(vfs->vfs_dev),
	    sizeof (sbp->f_fstr) - 1);

	return (0);
}

/*
 * VFS_VGET.  A nodeid is only meaningful while we hold a reference to it, so
 * this only ever finds nodes that are already in the hash; it cannot resurrect
 * one the server may since have reused.
 */
static int
virtiofs_vget(vfs_t *vfsp, vnode_t **vpp, struct fid *fidp)
{
	vfsmnt_t *vfs = VFS_TO_VIRTIOFS(vfsp);
	vfid_t *vfid = (vfid_t *)fidp;
	vfnode_t *vfn;

	*vpp = NULL;

	if (vfid->vfid_len != sizeof (vfid_t) - sizeof (uint16_t))
		return (EINVAL);

	rw_enter(&vfs->vfs_hash_lock, RW_READER);
	if ((vfn = virtiofs_node_find(vfs, vfid->vfid_nodeid)) != NULL) {
		VN_HOLD(VFTOV(vfn));
		*vpp = VFTOV(vfn);
	}
	rw_exit(&vfs->vfs_hash_lock);

	return (*vpp == NULL ? ESTALE : 0);
}
