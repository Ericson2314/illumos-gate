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
 * VIRTIO FS: FUSE OPERATIONS AND THE NODE CACHE
 *
 * Everything here is the plumbing between the vnode layer above and the
 * "vtfs" transport below: marshalling a FUSE request, unmarshalling the reply,
 * translating the server's error and attribute encodings into ours, and
 * keeping a hash of the nodeids the server has told us about.
 *
 * LIFETIME OF A NODEID
 *
 * A FUSE server hands out a nodeid from a successful FUSE_LOOKUP and keeps a
 * count of how many times it has done so.  The client must eventually return
 * that count with a FUSE_FORGET, at which point the server may reuse the
 * nodeid.  So each of our nodes counts the lookups behind it in vfn_nlookup,
 * and VOP_INACTIVE forgets the whole count at once.  Getting this wrong does
 * not corrupt anything locally, but it does leak inodes on the server, or --
 * worse -- lets the server reuse a nodeid we still believe in.
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
#include <sys/stat.h>
#include <sys/mode.h>
#include <sys/time.h>
#include <sys/atomic.h>
#include <sys/proc.h>
#include <sys/cred.h>

#include <vm/pvn.h>

#include "virtiofs.h"

/*
 * A FUSE server reports Linux errno values.  The first thirty-four agree with
 * ours by historical accident; past that they do not, and the ones that a
 * read-only client can plausibly see have to be translated by hand.  Anything
 * we do not recognise becomes EIO rather than something that would be silently
 * misinterpreted.
 */
int
virtiofs_errno(int32_t err)
{
	int e = -err;

	if (e <= 0)
		return (0);

	if (e <= 34)
		return (e);

	switch (e) {
	case 36:			/* Linux ENAMETOOLONG */
		return (ENAMETOOLONG);
	case 39:			/* Linux ENOTEMPTY */
		return (ENOTEMPTY);
	case 40:			/* Linux ELOOP */
		return (ELOOP);
	case 61:			/* Linux ENODATA */
		return (ENODATA);
	case 75:			/* Linux EOVERFLOW */
		return (EOVERFLOW);
	case 78:			/* Linux EREMOTE */
		return (EREMOTE);
	case 95:			/* Linux EOPNOTSUPP */
		return (ENOTSUP);
	case 122:			/* Linux EDQUOT */
		return (EDQUOT);
	default:
		return (EIO);
	}
}

/*
 * Issue one FUSE request and wait for its reply.
 *
 * "inbody" and "inbodylen" are the in-args that follow the fuse_in_header;
 * "outbody" and "outbodylen" are the fixed size part of the reply that follows
 * the fuse_out_header.  "data" is an optional buffer for a variable length
 * tail -- the payload of a FUSE_READ, or the dirent stream of a FUSE_READDIR
 * -- and on success *residp is set to how many bytes of it the server filled.
 *
 * An outbodylen and datalen of zero, with a NULL data, means the request has
 * no reply at all; only FUSE_FORGET is like that.
 */
static int
virtiofs_call(vfsmnt_t *vfs, uint32_t opcode, uint64_t nodeid,
    const void *inbody, size_t inbodylen,
    void *outbody, size_t outbodylen,
    void *data, size_t datalen, size_t *residp)
{
	struct fuse_in_header *ihdr;
	struct fuse_out_header *ohdr;
	vtfs_xfer_t vx;
	char *obuf, *ibuf;
	size_t obuflen = sizeof (*ihdr) + inbodylen;
	size_t ibuflen;
	boolean_t noreply = (outbodylen == 0 && data == NULL);
	size_t got;
	int r;

	ibuflen = noreply ? 0 : sizeof (*ohdr) + outbodylen;

	if (obuflen > VTFS_BOUNCE_SIZE || ibuflen > VTFS_BOUNCE_SIZE)
		return (EINVAL);

	obuf = kmem_zalloc(obuflen, KM_SLEEP);
	ibuf = ibuflen > 0 ? kmem_zalloc(ibuflen, KM_SLEEP) : NULL;

	ihdr = (struct fuse_in_header *)obuf;
	ihdr->len = (uint32_t)obuflen;
	ihdr->opcode = opcode;
	mutex_enter(&vfs->vfs_lock);
	ihdr->unique = ++vfs->vfs_unique;
	mutex_exit(&vfs->vfs_lock);
	ihdr->nodeid = nodeid;
	ihdr->uid = crgetuid(CRED());
	ihdr->gid = crgetgid(CRED());
	ihdr->pid = (uint32_t)ttoproc(curthread)->p_pid;

	if (inbodylen > 0)
		bcopy(inbody, obuf + sizeof (*ihdr), inbodylen);

	bzero(&vx, sizeof (vx));
	vx.vx_out = obuf;
	vx.vx_outlen = obuflen;
	vx.vx_in = ibuf;
	vx.vx_inlen = ibuflen;
	vx.vx_data = data;
	vx.vx_datalen = datalen;

	r = vtfs_xfer(vfs->vfs_dev, &vx);
	if (r != 0 || noreply)
		goto out;

	ohdr = (struct fuse_out_header *)ibuf;

	if (ohdr->error != 0) {
		r = virtiofs_errno(ohdr->error);
		goto out;
	}

	/*
	 * The server's own length field, not the transport's idea of how many
	 * bytes it wrote, is what says how much of the reply is meaningful;
	 * the latter is documented in the Virtio specification as advisory.
	 */
	if (ohdr->len < sizeof (*ohdr) + outbodylen) {
		r = EIO;
		goto out;
	}

	got = ohdr->len - sizeof (*ohdr) - outbodylen;
	if (got > datalen) {
		r = EIO;
		goto out;
	}

	if (outbodylen > 0)
		bcopy(ibuf + sizeof (*ohdr), outbody, outbodylen);
	if (residp != NULL)
		*residp = got;

out:
	kmem_free(obuf, obuflen);
	if (ibuf != NULL)
		kmem_free(ibuf, ibuflen);
	return (r);
}

int
virtiofs_fuse_init(vfsmnt_t *vfs)
{
	struct fuse_init_in in;
	struct fuse_init_out out;
	int r;

	bzero(&in, sizeof (in));
	in.major = FUSE_KERNEL_VERSION;
	in.minor = FUSE_KERNEL_MINOR_VERSION;
	in.max_readahead = VIRTIOFS_MAXBSIZE;
	in.flags = 0;

	bzero(&out, sizeof (out));
	r = virtiofs_call(vfs, FUSE_INIT, 0, &in, sizeof (in),
	    &out, sizeof (out), NULL, 0, NULL);
	if (r != 0)
		return (r);

	if (out.major != FUSE_KERNEL_VERSION) {
		cmn_err(CE_WARN, "virtiofs: server wants FUSE %u.%u, "
		    "we speak %u.%u", out.major, out.minor,
		    FUSE_KERNEL_VERSION, FUSE_KERNEL_MINOR_VERSION);
		return (ENOTSUP);
	}

	vfs->vfs_major = out.major;
	vfs->vfs_minor = out.minor;
	vfs->vfs_max_write = out.max_write != 0 ? out.max_write :
	    VIRTIOFS_MAXBSIZE;
	vfs->vfs_max_read = VIRTIOFS_MAXBSIZE;

	return (0);
}

int
virtiofs_fuse_lookup(vfsmnt_t *vfs, uint64_t parent, const char *name,
    struct fuse_entry_out *eo)
{
	size_t namelen = strlen(name) + 1;
	int r;

	if (namelen > MAXNAMELEN)
		return (ENAMETOOLONG);

	r = virtiofs_call(vfs, FUSE_LOOKUP, parent, name, namelen,
	    eo, sizeof (*eo), NULL, 0, NULL);
	if (r != 0)
		return (r);

	/*
	 * A server is allowed to answer a lookup for a nonexistent name with a
	 * zero nodeid and a positive entry timeout, which is its way of
	 * caching a negative result.  We have nowhere to keep that, so it is
	 * simply an ENOENT.
	 */
	if (eo->nodeid == 0)
		return (ENOENT);

	return (0);
}

void
virtiofs_fuse_forget(vfsmnt_t *vfs, uint64_t nodeid, uint64_t nlookup)
{
	struct fuse_forget_in in;

	if (nlookup == 0)
		return;

	in.nlookup = nlookup;

	/*
	 * FUSE_FORGET has no reply, so there is nothing to check and nothing
	 * useful to do about a transport failure either.
	 */
	(void) virtiofs_call(vfs, FUSE_FORGET, nodeid, &in, sizeof (in),
	    NULL, 0, NULL, 0, NULL);
}

int
virtiofs_fuse_getattr(vfsmnt_t *vfs, uint64_t nodeid, struct fuse_attr_out *ao)
{
	struct fuse_getattr_in in;

	bzero(&in, sizeof (in));

	return (virtiofs_call(vfs, FUSE_GETATTR, nodeid, &in, sizeof (in),
	    ao, sizeof (*ao), NULL, 0, NULL));
}

int
virtiofs_fuse_open(vfsmnt_t *vfs, uint64_t nodeid, boolean_t isdir,
    uint64_t *fhp)
{
	struct fuse_open_in in;
	struct fuse_open_out out;
	int r;

	bzero(&in, sizeof (in));
	/*
	 * O_RDONLY is zero in the Linux encoding these flags use, and this
	 * module never opens anything any other way.
	 */
	in.flags = 0;

	bzero(&out, sizeof (out));
	r = virtiofs_call(vfs, isdir ? FUSE_OPENDIR : FUSE_OPEN, nodeid,
	    &in, sizeof (in), &out, sizeof (out), NULL, 0, NULL);
	if (r != 0)
		return (r);

	*fhp = out.fh;
	return (0);
}

int
virtiofs_fuse_release(vfsmnt_t *vfs, uint64_t nodeid, uint64_t fh,
    boolean_t isdir)
{
	struct fuse_release_in in;

	bzero(&in, sizeof (in));
	in.fh = fh;
	in.flags = 0;

	return (virtiofs_call(vfs, isdir ? FUSE_RELEASEDIR : FUSE_RELEASE,
	    nodeid, &in, sizeof (in), NULL, 0, NULL, 0, NULL));
}

static int
virtiofs_read_common(vfsmnt_t *vfs, uint32_t opcode, uint64_t nodeid,
    uint64_t fh, u_offset_t off, void *buf, size_t len, size_t *residp)
{
	struct fuse_read_in in;
	struct fuse_out_header ohdr;
	int r;

	if (len > UINT32_MAX)
		return (EINVAL);

	bzero(&in, sizeof (in));
	in.fh = fh;
	in.offset = off;
	in.size = (uint32_t)len;

	/*
	 * There is no fixed size out-args for either of these; the whole reply
	 * past the header is the payload, which goes straight into the
	 * caller's buffer without passing through the bounce buffer.
	 */
	bzero(&ohdr, sizeof (ohdr));
	r = virtiofs_call(vfs, opcode, nodeid, &in, sizeof (in),
	    NULL, 0, buf, len, residp);

	return (r);
}

int
virtiofs_fuse_read(vfsmnt_t *vfs, uint64_t nodeid, uint64_t fh,
    u_offset_t off, void *buf, size_t len, size_t *residp)
{
	return (virtiofs_read_common(vfs, FUSE_READ, nodeid, fh, off, buf, len,
	    residp));
}

int
virtiofs_fuse_readdir(vfsmnt_t *vfs, uint64_t nodeid, uint64_t fh,
    u_offset_t off, void *buf, size_t len, size_t *residp)
{
	return (virtiofs_read_common(vfs, FUSE_READDIR, nodeid, fh, off, buf,
	    len, residp));
}

int
virtiofs_fuse_readlink(vfsmnt_t *vfs, uint64_t nodeid, char *buf, size_t len,
    size_t *residp)
{
	/*
	 * The reply to FUSE_READLINK is the target, without a NUL terminator;
	 * the caller is responsible for adding one if it wants one.
	 */
	return (virtiofs_call(vfs, FUSE_READLINK, nodeid, NULL, 0,
	    NULL, 0, buf, len, residp));
}

int
virtiofs_fuse_statfs(vfsmnt_t *vfs, struct fuse_kstatfs *ks)
{
	return (virtiofs_call(vfs, FUSE_STATFS, FUSE_ROOT_ID, NULL, 0,
	    ks, sizeof (*ks), NULL, 0, NULL));
}

/*
 * The node cache.
 */

static uint_t
virtiofs_hash(uint64_t nodeid)
{
	return ((uint_t)(nodeid % VIRTIOFS_NHASH));
}

enum vtype
virtiofs_mode_to_vtype(uint32_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFREG:
		return (VREG);
	case S_IFDIR:
		return (VDIR);
	case S_IFLNK:
		return (VLNK);
	case S_IFCHR:
		return (VCHR);
	case S_IFBLK:
		return (VBLK);
	case S_IFIFO:
		return (VFIFO);
	case S_IFSOCK:
		return (VSOCK);
	default:
		return (VNON);
	}
}

void
virtiofs_attr_to_vattr(vfsmnt_t *vfs, const struct fuse_attr *fa, vattr_t *vap)
{
	vap->va_type = virtiofs_mode_to_vtype(fa->mode);
	vap->va_mode = fa->mode & MODEMASK;
	vap->va_uid = vfs->vfs_override_ids ? vfs->vfs_uid : (uid_t)fa->uid;
	vap->va_gid = vfs->vfs_override_ids ? vfs->vfs_gid : (gid_t)fa->gid;
	vap->va_fsid = vfs->vfs_dev_no;
	vap->va_nodeid = fa->ino;
	vap->va_nlink = fa->nlink;
	vap->va_size = fa->size;
	vap->va_atime.tv_sec = fa->atime;
	vap->va_atime.tv_nsec = fa->atimensec;
	vap->va_mtime.tv_sec = fa->mtime;
	vap->va_mtime.tv_nsec = fa->mtimensec;
	vap->va_ctime.tv_sec = fa->ctime;
	vap->va_ctime.tv_nsec = fa->ctimensec;
	vap->va_rdev = fa->rdev;
	vap->va_blksize = fa->blksize != 0 ? fa->blksize : PAGESIZE;
	vap->va_nblocks = fa->blocks;
	vap->va_seq = 0;
}

/*
 * Record a fresh set of attributes, with the validity window the server asked
 * for.  A window of zero means "do not cache", which we honour by making the
 * entry expire immediately rather than by refusing to store it, so that the
 * read path stays uniform.
 */
void
virtiofs_node_setattr(vfnode_t *vfn, const struct fuse_attr *fa,
    uint64_t valid_sec, uint32_t valid_nsec)
{
	hrtime_t window;

	mutex_enter(&vfn->vfn_lock);
	vfn->vfn_attr = *fa;
	vfn->vfn_attr_valid = B_TRUE;

	/*
	 * Servers routinely report a validity of one year, or of UINT64_MAX,
	 * for an immutable tree.  Clamp before scaling so the arithmetic
	 * cannot overflow.
	 */
	if (valid_sec > 3600)
		valid_sec = 3600;
	window = (hrtime_t)valid_sec * NANOSEC + (hrtime_t)valid_nsec;
	vfn->vfn_attr_expiry = gethrtime() + window;

	VFTOV(vfn)->v_type = virtiofs_mode_to_vtype(fa->mode);
	mutex_exit(&vfn->vfn_lock);
}

/*
 * Fetch this node's attributes, going to the server if what we have has
 * expired.
 */
int
virtiofs_node_getattr(vfnode_t *vfn, struct fuse_attr *fa)
{
	vfsmnt_t *vfs = vfn->vfn_mnt;
	struct fuse_attr_out ao;
	int r;

	mutex_enter(&vfn->vfn_lock);
	if (vfn->vfn_attr_valid && gethrtime() < vfn->vfn_attr_expiry) {
		*fa = vfn->vfn_attr;
		mutex_exit(&vfn->vfn_lock);
		return (0);
	}
	mutex_exit(&vfn->vfn_lock);

	bzero(&ao, sizeof (ao));
	if ((r = virtiofs_fuse_getattr(vfs, vfn->vfn_nodeid, &ao)) != 0) {
		/*
		 * Fall back on a stale cached copy rather than failing outright
		 * only if we have nothing at all; a stale size would be worse
		 * than an error for the paging path.
		 */
		return (r);
	}

	virtiofs_node_setattr(vfn, &ao.attr, ao.attr_valid,
	    ao.attr_valid_nsec);
	*fa = ao.attr;

	return (0);
}

vfnode_t *
virtiofs_node_find(vfsmnt_t *vfs, uint64_t nodeid)
{
	vfnode_t *vfn;
	uint_t h = virtiofs_hash(nodeid);

	VERIFY(RW_LOCK_HELD(&vfs->vfs_hash_lock));

	for (vfn = vfs->vfs_hash[h]; vfn != NULL; vfn = vfn->vfn_hash) {
		if (vfn->vfn_nodeid == nodeid)
			return (vfn);
	}

	return (NULL);
}

/*
 * Return the node for "nodeid", creating it if we have not seen it before, and
 * with a held vnode either way.  "nlookup" is how many lookup references the
 * caller is handing over; it is one for a fresh FUSE_LOOKUP and zero when the
 * caller already owns the reference (as VFS_ROOT does).
 */
vfnode_t *
virtiofs_node_hold(vfsmnt_t *vfs, uint64_t nodeid, const struct fuse_attr *fa,
    uint64_t nlookup)
{
	vfnode_t *vfn, *found;
	vnode_t *vp;
	uint_t h = virtiofs_hash(nodeid);

	rw_enter(&vfs->vfs_hash_lock, RW_READER);
	if ((vfn = virtiofs_node_find(vfs, nodeid)) != NULL) {
		VN_HOLD(VFTOV(vfn));
		atomic_add_64(&vfn->vfn_nlookup, nlookup);
		rw_exit(&vfs->vfs_hash_lock);
		if (fa != NULL)
			virtiofs_node_setattr(vfn, fa, 0, 0);
		return (vfn);
	}
	rw_exit(&vfs->vfs_hash_lock);

	vfn = kmem_zalloc(sizeof (*vfn), KM_SLEEP);
	vfn->vfn_mnt = vfs;
	vfn->vfn_nodeid = nodeid;
	vfn->vfn_nlookup = nlookup;
	mutex_init(&vfn->vfn_lock, NULL, MUTEX_DEFAULT, NULL);

	vp = vn_alloc(KM_SLEEP);
	vfn->vfn_vnode = vp;
	vn_setops(vp, virtiofs_vnodeops);
	vp->v_data = vfn;
	vp->v_vfsp = vfs->vfs_vfsp;
	vp->v_type = fa != NULL ? virtiofs_mode_to_vtype(fa->mode) : VNON;
	vp->v_rdev = vfs->vfs_dev_no;
	vn_exists(vp);

	if (fa != NULL) {
		vfn->vfn_attr = *fa;
		vfn->vfn_attr_valid = B_TRUE;
		vfn->vfn_attr_expiry = gethrtime();
	}

	/*
	 * Another thread may have raced us here with the same nodeid, so look
	 * again with the lock held for writing before publishing ours.
	 */
	rw_enter(&vfs->vfs_hash_lock, RW_WRITER);
	if ((found = virtiofs_node_find(vfs, nodeid)) != NULL) {
		VN_HOLD(VFTOV(found));
		atomic_add_64(&found->vfn_nlookup, nlookup);
		rw_exit(&vfs->vfs_hash_lock);

		vn_invalid(vp);
		vn_free(vp);
		mutex_destroy(&vfn->vfn_lock);
		kmem_free(vfn, sizeof (*vfn));

		if (fa != NULL)
			virtiofs_node_setattr(found, fa, 0, 0);
		return (found);
	}

	vfn->vfn_hash = vfs->vfs_hash[h];
	vfs->vfs_hash[h] = vfn;
	rw_exit(&vfs->vfs_hash_lock);

	return (vfn);
}

/*
 * Unhash a node and release everything it owns.  Called only from
 * VOP_INACTIVE, once the last reference is gone and the node has been
 * unlinked from nothing else.
 */
void
virtiofs_node_free(vfnode_t *vfn)
{
	vfsmnt_t *vfs = vfn->vfn_mnt;
	vnode_t *vp = VFTOV(vfn);
	uint_t h = virtiofs_hash(vfn->vfn_nodeid);
	vfnode_t **pp;
	uint64_t nlookup;

	rw_enter(&vfs->vfs_hash_lock, RW_WRITER);
	for (pp = &vfs->vfs_hash[h]; *pp != NULL; pp = &(*pp)->vfn_hash) {
		if (*pp == vfn) {
			*pp = vfn->vfn_hash;
			break;
		}
	}
	rw_exit(&vfs->vfs_hash_lock);

	if (vfn->vfn_fh_valid) {
		(void) virtiofs_fuse_release(vfs, vfn->vfn_nodeid, vfn->vfn_fh,
		    vp->v_type == VDIR);
		vfn->vfn_fh_valid = B_FALSE;
	}

	nlookup = vfn->vfn_nlookup;
	vfn->vfn_nlookup = 0;
	virtiofs_fuse_forget(vfs, vfn->vfn_nodeid, nlookup);

	/*
	 * Nothing here is ever dirty, but the pages still have to be taken off
	 * the vnode before it goes away.
	 */
	if (vn_has_cached_data(vp)) {
		(void) pvn_vplist_dirty(vp, 0, virtiofs_putapage,
		    B_INVAL | B_TRUNC, kcred);
	}

	vn_invalid(vp);
	vn_free(vp);
	mutex_destroy(&vfn->vfn_lock);
	kmem_free(vfn, sizeof (*vfn));
}

/*
 * Server file handles.  One handle is shared by every open of a node; the
 * count says how many opens are still relying on it.  A read-only client has
 * no per-open state that could make sharing wrong, and the alternative --
 * a handle per open -- would mean threading state through VOP_READ and
 * VOP_GETPAGE, which do not always have an open file to hand.
 */
int
virtiofs_openfh(vfnode_t *vfn, boolean_t isdir, uint64_t *fhp)
{
	vfsmnt_t *vfs = vfn->vfn_mnt;
	uint64_t fh;
	int r;

	mutex_enter(&vfn->vfn_lock);
	if (vfn->vfn_fh_valid) {
		vfn->vfn_fh_count++;
		if (fhp != NULL)
			*fhp = vfn->vfn_fh;
		mutex_exit(&vfn->vfn_lock);
		return (0);
	}
	mutex_exit(&vfn->vfn_lock);

	if ((r = virtiofs_fuse_open(vfs, vfn->vfn_nodeid, isdir, &fh)) != 0)
		return (r);

	mutex_enter(&vfn->vfn_lock);
	if (vfn->vfn_fh_valid) {
		/*
		 * We raced with another opener.  Keep theirs and give ours
		 * back; two handles on one node is legal but pointless.
		 */
		vfn->vfn_fh_count++;
		if (fhp != NULL)
			*fhp = vfn->vfn_fh;
		mutex_exit(&vfn->vfn_lock);
		(void) virtiofs_fuse_release(vfs, vfn->vfn_nodeid, fh, isdir);
		return (0);
	}

	vfn->vfn_fh = fh;
	vfn->vfn_fh_valid = B_TRUE;
	vfn->vfn_fh_count = 1;
	if (fhp != NULL)
		*fhp = fh;
	mutex_exit(&vfn->vfn_lock);

	return (0);
}

void
virtiofs_closefh(vfnode_t *vfn, boolean_t isdir)
{
	vfsmnt_t *vfs = vfn->vfn_mnt;
	uint64_t fh;

	mutex_enter(&vfn->vfn_lock);
	if (!vfn->vfn_fh_valid || vfn->vfn_fh_count == 0) {
		mutex_exit(&vfn->vfn_lock);
		return;
	}

	if (--vfn->vfn_fh_count > 0) {
		mutex_exit(&vfn->vfn_lock);
		return;
	}

	/*
	 * Deliberately keep the handle across a zero count for a regular file:
	 * VOP_GETPAGE can be called long after the last close, for a mapping
	 * that outlives it, and it has no way to reopen.  The handle is
	 * released for good in virtiofs_node_free().  A directory has no such
	 * problem, so its handle goes back immediately.
	 */
	if (!isdir) {
		mutex_exit(&vfn->vfn_lock);
		return;
	}

	fh = vfn->vfn_fh;
	vfn->vfn_fh_valid = B_FALSE;
	mutex_exit(&vfn->vfn_lock);

	(void) virtiofs_fuse_release(vfs, vfn->vfn_nodeid, fh, B_TRUE);
}
