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
 * VIRTIO FS: VNODE OPERATIONS
 *
 * This is a read-only file system, so the operation set is close to hsfs':
 * lookup, read, readdir, readlink, getattr, access, and the paging entry
 * points, with everything that would modify the file system left out of the
 * table entirely so that the vnode layer's fs_nosys() stubs answer for them.
 *
 * PAGING
 *
 * VOP_GETPAGE is the reason this module cannot be a thin translation of
 * read(2) into FUSE_READ.  Executables and shared libraries are mapped, never
 * read, so a file system whose pages do not come from the page cache cannot
 * run a program out of itself.  The structure here is the conventional one --
 * pvn_getpages() over a per-page routine which klusters with
 * pvn_read_kluster(), sets up a pageio buffer, maps it into the kernel with
 * bp_mapin(9F), and fills it -- with the FUSE_READ round trip standing where a
 * disk based file system would call its strategy routine.
 *
 * VOP_PUTPAGE exists only to take clean pages off a vnode.  Nothing here is
 * ever dirty; a dirty page would mean somebody wrote to a MAP_SHARED mapping,
 * which VOP_MAP refuses to create.
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
#include <sys/uio.h>
#include <sys/stat.h>
#include <sys/mode.h>
#include <sys/dirent.h>
#include <sys/pathname.h>
#include <sys/policy.h>
#include <sys/cred.h>
#include <sys/buf.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/dnlc.h>
#include <sys/statvfs.h>
#include <sys/mman.h>
#include <sys/vmsystm.h>

#include <vm/hat.h>
#include <vm/as.h>
#include <vm/page.h>
#include <vm/pvn.h>
#include <vm/seg.h>
#include <vm/seg_map.h>
#include <vm/seg_vn.h>
#include <vm/seg_kmem.h>

#include "fs/fs_subr.h"
#include "virtiofs.h"

struct vnodeops *virtiofs_vnodeops;

static int virtiofs_getapage(vnode_t *, u_offset_t, size_t, uint_t *,
    page_t *[], size_t, struct seg *, caddr_t, enum seg_rw, cred_t *);

/*
 * Read [off, off + len) of "vp" into the kernel buffer "buf", asking the
 * server for at most one maximum sized read at a time.  A short reply means
 * end of file: the remainder of the buffer is zeroed, which is what the paging
 * path needs for the partial page at the end of a file.
 */
static int
virtiofs_readio(vnode_t *vp, caddr_t buf, u_offset_t off, size_t len)
{
	vfnode_t *vfn = VTOVF(vp);
	vfsmnt_t *vfs = vfn->vfn_mnt;
	uint64_t fh;
	size_t done = 0;
	int r;

	if ((r = virtiofs_openfh(vfn, B_FALSE, &fh)) != 0)
		return (r);

	while (done < len) {
		size_t want = MIN(len - done, vfs->vfs_max_read);
		size_t got = 0;

		r = virtiofs_fuse_read(vfs, vfn->vfn_nodeid, fh, off + done,
		    buf + done, want, &got);
		if (r != 0)
			break;

		done += got;

		if (got < want) {
			/*
			 * End of file.  Everything past it reads as zero.
			 */
			break;
		}
	}

	/*
	 * The handle stays open: see the comment in virtiofs_closefh() about
	 * mappings that outlive their file descriptor.  All we give back here
	 * is the count we took.
	 */
	virtiofs_closefh(vfn, B_FALSE);

	if (r != 0)
		return (r);

	if (done < len)
		bzero(buf + done, len - done);

	return (0);
}

/*
 * The access check.  Modelled on hs_access(): a read-only medium never grants
 * write, and the rest is the usual owner/group/other shift into
 * secpolicy_vnode_access2().
 */
static int
virtiofs_taccess(vnode_t *vp, mode_t m, cred_t *cr)
{
	vfnode_t *vfn = VTOVF(vp);
	vfsmnt_t *vfs = vfn->vfn_mnt;
	struct fuse_attr fa;
	uid_t uid;
	gid_t gid;
	int shift = 0;
	int r;

	if (m & VWRITE)
		return (EROFS);

	if ((r = virtiofs_node_getattr(vfn, &fa)) != 0)
		return (r);

	uid = vfs->vfs_override_ids ? vfs->vfs_uid : (uid_t)fa.uid;
	gid = vfs->vfs_override_ids ? vfs->vfs_gid : (gid_t)fa.gid;

	if (crgetuid(cr) != uid) {
		shift += 3;
		if (!groupmember(gid, cr))
			shift += 3;
	}

	return (secpolicy_vnode_access2(cr, vp, uid,
	    (fa.mode & MODEMASK) << shift, m));
}

/* ARGSUSED */
static int
virtiofs_open(vnode_t **vpp, int flag, cred_t *cr, caller_context_t *ct)
{
	vnode_t *vp = *vpp;
	vfnode_t *vfn = VTOVF(vp);

	if (flag & (FWRITE | FTRUNC | FCREAT))
		return (EROFS);

	/*
	 * opendir(3C) is an open(2), so a directory must be openable.  It
	 * needs no server handle here: VOP_READDIR does its own FUSE_OPENDIR
	 * for the duration of each call.
	 */
	if (vp->v_type != VREG)
		return (0);

	return (virtiofs_openfh(vfn, B_FALSE, NULL));
}

/* ARGSUSED */
static int
virtiofs_close(vnode_t *vp, int flag, int count, offset_t offset, cred_t *cr,
    caller_context_t *ct)
{
	vfnode_t *vfn = VTOVF(vp);

	if (count > 1)
		return (0);

	if (vp->v_type != VREG)
		return (0);

	cleanlocks(vp, ttoproc(curthread)->p_pid, 0);
	cleanshares(vp, ttoproc(curthread)->p_pid);

	virtiofs_closefh(vfn, B_FALSE);

	return (0);
}

/*
 * VOP_READ, by way of the page cache.  This is hsfs_read() with the ISO
 * specifics taken out: one segmap window at a time, which faults through
 * virtiofs_getpage() and so shares every page with any mapping of the same
 * file.
 */
/* ARGSUSED */
static int
virtiofs_read(vnode_t *vp, struct uio *uiop, int ioflag, cred_t *cr,
    caller_context_t *ct)
{
	vfnode_t *vfn = VTOVF(vp);
	struct fuse_attr fa;
	u_offset_t filesize;
	caddr_t base;
	int error;

	if (vp->v_type == VDIR)
		return (EISDIR);
	if (vp->v_type != VREG)
		return (EINVAL);

	if ((error = virtiofs_node_getattr(vfn, &fa)) != 0)
		return (error);
	filesize = fa.size;

	if (uiop->uio_resid == 0 || uiop->uio_loffset < 0)
		return (uiop->uio_loffset < 0 ? EINVAL : 0);
	if ((u_offset_t)uiop->uio_loffset >= filesize)
		return (0);

	do {
		offset_t mapon;
		offset_t diff;
		size_t nbytes;
		size_t n;
		uint_t flags;

		mapon = uiop->uio_loffset & MAXBOFFSET;
		diff = filesize - uiop->uio_loffset;
		nbytes = (size_t)MIN(MAXBSIZE - mapon, uiop->uio_resid);
		n = (size_t)MIN(diff, (offset_t)nbytes);
		if (n == 0)
			return (0);

		base = segmap_getmapflt(segkmap, vp,
		    (u_offset_t)uiop->uio_loffset, n, 1, S_READ);

		error = uiomove(base + mapon, n, UIO_READ, uiop);

		if (error == 0) {
			/*
			 * A window we have finished with, or one that ends at
			 * end of file, will not be wanted again soon.
			 */
			if (n + mapon == MAXBSIZE ||
			    (u_offset_t)uiop->uio_loffset == filesize)
				flags = SM_DONTNEED;
			else
				flags = 0;

			error = segmap_release(segkmap, base, flags);
		} else {
			(void) segmap_release(segkmap, base, 0);
		}
	} while (error == 0 && uiop->uio_resid > 0);

	return (error);
}

/* ARGSUSED */
static int
virtiofs_getattr(vnode_t *vp, vattr_t *vap, int flags, cred_t *cr,
    caller_context_t *ct)
{
	vfnode_t *vfn = VTOVF(vp);
	struct fuse_attr fa;
	int r;

	if ((r = virtiofs_node_getattr(vfn, &fa)) != 0)
		return (r);

	virtiofs_attr_to_vattr(vfn->vfn_mnt, &fa, vap);

	return (0);
}

/* ARGSUSED */
static int
virtiofs_access(vnode_t *vp, int mode, int flags, cred_t *cr,
    caller_context_t *ct)
{
	return (virtiofs_taccess(vp, (mode_t)mode, cr));
}

/*
 * VOP_LOOKUP.  "." and ".." would both be answered correctly by the server,
 * but "." can be answered without a round trip, and the DNLC covers the rest.
 */
/* ARGSUSED */
static int
virtiofs_lookup(vnode_t *dvp, char *nm, vnode_t **vpp, struct pathname *pnp,
    int flags, vnode_t *rdir, cred_t *cr, caller_context_t *ct,
    int *direntflags, pathname_t *realpnp)
{
	vfnode_t *dvfn = VTOVF(dvp);
	vfsmnt_t *vfs = dvfn->vfn_mnt;
	struct fuse_entry_out eo;
	vfnode_t *vfn;
	vnode_t *vp;
	int error;

	if (dvp->v_type != VDIR)
		return (ENOTDIR);

	if (*nm == '\0') {
		VN_HOLD(dvp);
		*vpp = dvp;
		return (0);
	}

	if ((error = virtiofs_taccess(dvp, VEXEC, cr)) != 0)
		return (error);

	if (nm[0] == '.' && nm[1] == '\0') {
		VN_HOLD(dvp);
		*vpp = dvp;
		return (0);
	}

	if ((vp = dnlc_lookup(dvp, nm)) != NULL) {
		if (vp == DNLC_NO_VNODE) {
			VN_RELE(vp);
			return (ENOENT);
		}
		*vpp = vp;
		return (0);
	}

	bzero(&eo, sizeof (eo));
	if ((error = virtiofs_fuse_lookup(vfs, dvfn->vfn_nodeid, nm,
	    &eo)) != 0) {
		/*
		 * Only a definitively absent name is worth a negative cache
		 * entry; an I/O error says nothing about the name.
		 */
		if (error == ENOENT)
			dnlc_enter(dvp, nm, DNLC_NO_VNODE);
		return (error);
	}

	vfn = virtiofs_node_hold(vfs, eo.nodeid, &eo.attr, 1);
	vfn->vfn_gen = eo.generation;
	virtiofs_node_setattr(vfn, &eo.attr, eo.attr_valid, eo.attr_valid_nsec);

	*vpp = VFTOV(vfn);
	dnlc_enter(dvp, nm, *vpp);

	return (0);
}

/*
 * VOP_READDIR.  One serverside buffer of packed fuse_dirent records is decoded
 * into the caller's dirent64 buffer per pass; d_off carries the server's
 * opaque cookie straight back out, since that is what the next FUSE_READDIR
 * expects to be given as its offset.
 */
/* ARGSUSED */
static int
virtiofs_readdir(vnode_t *vp, struct uio *uiop, cred_t *cr, int *eofp,
    caller_context_t *ct, int flags)
{
	vfnode_t *vfn = VTOVF(vp);
	vfsmnt_t *vfs = vfn->vfn_mnt;
	uint64_t fh;
	char *fbuf = NULL;
	char *obuf = NULL;
	struct dirent64 *nd;
	size_t obufsize;
	size_t outoff = 0;
	size_t got = 0;
	size_t pos;
	u_offset_t cookie;
	int error;

	if (vp->v_type != VDIR)
		return (ENOTDIR);
	if (uiop->uio_loffset < 0 || uiop->uio_resid <= 0)
		return (EINVAL);
	if (uiop->uio_iovcnt != 1)
		return (EINVAL);

	if (eofp != NULL)
		*eofp = 0;

	if ((error = virtiofs_openfh(vfn, B_TRUE, &fh)) != 0)
		return (error);

	cookie = (u_offset_t)uiop->uio_loffset;

	fbuf = kmem_alloc(VIRTIOFS_DIRBUFSZ, KM_SLEEP);
	obufsize = uiop->uio_resid;
	obuf = kmem_alloc(obufsize, KM_SLEEP);

	error = virtiofs_fuse_readdir(vfs, vfn->vfn_nodeid, fh, cookie,
	    fbuf, VIRTIOFS_DIRBUFSZ, &got);
	if (error != 0)
		goto done;

	if (got == 0) {
		/*
		 * An empty reply is the end of the directory.
		 */
		if (eofp != NULL)
			*eofp = 1;
		goto done;
	}

	pos = 0;
	while (pos + FUSE_NAME_OFFSET <= got) {
		struct fuse_dirent *fd = (struct fuse_dirent *)(fbuf + pos);
		size_t reclen;
		long ndlen;

		if (fd->namelen == 0 || fd->namelen > MAXNAMELEN - 1) {
			error = EIO;
			goto done;
		}

		reclen = FUSE_DIRENT_SIZE(fd);
		if (pos + reclen > got) {
			/*
			 * A record that runs off the end of the buffer.  The
			 * server should never send one, since it is told how
			 * much room it has.
			 */
			error = EIO;
			goto done;
		}

		ndlen = (long)DIRENT64_RECLEN(fd->namelen);
		if (outoff + ndlen > obufsize) {
			/*
			 * The caller's buffer is full.  Everything decoded so
			 * far is good, and the next call resumes from the
			 * cookie of the last record we did copy.
			 */
			break;
		}

		nd = (struct dirent64 *)(obuf + outoff);
		nd->d_ino = (ino64_t)fd->ino;
		nd->d_off = (offset_t)fd->off;
		nd->d_reclen = (ushort_t)ndlen;

		/*
		 * bcopy() of exactly namelen bytes, then terminate and zero the
		 * padding by hand.  NOT strncpy(): it stops at a NUL in the
		 * *source*, and a fuse_dirent name is not NUL terminated -- it
		 * is exactly namelen bytes followed by alignment padding.  Since
		 * DIRENT64_NAMELEN() is the padded *destination* length, and is
		 * therefore larger, strncpy() reads past the name looking for a
		 * terminator that is not there.
		 *
		 * For the last record in the reply buffer that runs off the end
		 * of the allocation, which panics the kernel:
		 *
		 *     BAD TRAP: type=e (#pf Page fault) addr=fffffe091ff42000
		 *     coreutils: #pf Page fault
		 *     Bad kernel fault at addr=0xfffffe091ff42000
		 *
		 * with the fault address exactly at the page after the buffer.
		 * Every earlier record survives only because whatever follows it
		 * happens to contain a zero byte.
		 */
		bcopy(fd->name, nd->d_name, fd->namelen);
		bzero(nd->d_name + fd->namelen,
		    DIRENT64_NAMELEN(ndlen) - fd->namelen);

		outoff += ndlen;
		cookie = fd->off;
		pos += reclen;
	}

	if (outoff == 0) {
		/*
		 * Not one record fitted.  If we decoded nothing at all then
		 * the server has run out of entries; otherwise the caller
		 * simply asked with too small a buffer.
		 */
		if (pos == 0) {
			if (eofp != NULL)
				*eofp = 1;
		} else {
			error = EINVAL;
		}
		goto done;
	}

	error = uiomove(obuf, outoff, UIO_READ, uiop);
	if (error == 0)
		uiop->uio_loffset = (offset_t)cookie;

done:
	if (fbuf != NULL)
		kmem_free(fbuf, VIRTIOFS_DIRBUFSZ);
	if (obuf != NULL)
		kmem_free(obuf, obufsize);
	virtiofs_closefh(vfn, B_TRUE);

	return (error);
}

/* ARGSUSED */
static int
virtiofs_readlink(vnode_t *vp, struct uio *uiop, cred_t *cr,
    caller_context_t *ct)
{
	vfnode_t *vfn = VTOVF(vp);
	char *buf;
	size_t got = 0;
	int error;

	if (vp->v_type != VLNK)
		return (EINVAL);

	buf = kmem_alloc(MAXPATHLEN, KM_SLEEP);

	error = virtiofs_fuse_readlink(vfn->vfn_mnt, vfn->vfn_nodeid, buf,
	    MAXPATHLEN, &got);
	if (error == 0 && got > 0)
		error = uiomove(buf, got, UIO_READ, uiop);

	kmem_free(buf, MAXPATHLEN);

	return (error);
}

/* ARGSUSED */
static int
virtiofs_fsync(vnode_t *vp, int syncflag, cred_t *cr, caller_context_t *ct)
{
	/*
	 * Nothing here is ever dirty.
	 */
	return (0);
}

/* ARGSUSED */
static void
virtiofs_inactive(vnode_t *vp, cred_t *cr, caller_context_t *ct)
{
	vfnode_t *vfn = VTOVF(vp);

	mutex_enter(&vp->v_lock);
	VERIFY3U(vp->v_count, >=, 1);
	VN_RELE_LOCKED(vp);
	if (vp->v_count > 0) {
		/*
		 * Somebody found and held this vnode while we were on our way
		 * here, so it is alive again.
		 */
		mutex_exit(&vp->v_lock);
		return;
	}
	mutex_exit(&vp->v_lock);

	virtiofs_node_free(vfn);
}

/* ARGSUSED */
static int
virtiofs_fid(vnode_t *vp, struct fid *fidp, caller_context_t *ct)
{
	vfnode_t *vfn = VTOVF(vp);
	vfid_t *vfid;

	if (fidp->fid_len < sizeof (vfid_t) - sizeof (uint16_t)) {
		fidp->fid_len = sizeof (vfid_t) - sizeof (uint16_t);
		return (ENOSPC);
	}

	vfid = (vfid_t *)fidp;
	bzero(vfid, sizeof (*vfid));
	vfid->vfid_len = sizeof (vfid_t) - sizeof (uint16_t);
	vfid->vfid_gen = (uint32_t)vfn->vfn_gen;
	vfid->vfid_nodeid = vfn->vfn_nodeid;

	return (0);
}

/* ARGSUSED */
static int
virtiofs_seek(vnode_t *vp, offset_t ooff, offset_t *noffp,
    caller_context_t *ct)
{
	return (*noffp < 0 ? EINVAL : 0);
}

/* ARGSUSED */
static int
virtiofs_frlock(vnode_t *vp, int cmd, struct flock64 *bfp, int flag,
    offset_t offset, struct flk_callback *flk_cbp, cred_t *cr,
    caller_context_t *ct)
{
	vfnode_t *vfn = VTOVF(vp);
	struct fuse_attr fa;

	/*
	 * As in hsfs, a mapped file cannot take a mandatory lock.  The map
	 * count is read without its lock for the same reason hsfs reads it
	 * that way: every lock is dropped before fs_frlock() is entered, so a
	 * consistent snapshot would not stay consistent.
	 */
	if (vfn->vfn_mapcnt > 0 && virtiofs_node_getattr(vfn, &fa) == 0 &&
	    MANDLOCK(vp, fa.mode))
		return (EAGAIN);

	return (fs_frlock(vp, cmd, bfp, flag, offset, flk_cbp, cr, ct));
}

/*
 * Fetch one page-cache page's worth of file, klustering out to a full block
 * where we can.  Called only from pvn_getpages().
 */
/* ARGSUSED */
static int
virtiofs_getapage(vnode_t *vp, u_offset_t off, size_t len, uint_t *protp,
    page_t *pl[], size_t plsz, struct seg *seg, caddr_t addr, enum seg_rw rw,
    cred_t *cr)
{
	vfnode_t *vfn = VTOVF(vp);
	struct fuse_attr fa;
	page_t *pp = NULL;
	page_t *pagefound;
	struct buf *bp;
	u_offset_t io_off;
	u_offset_t blkoff;
	size_t io_len = 0;
	size_t blksize;
	uint_t bsize;
	int error = 0;

	bsize = MAX(vp->v_vfsp->vfs_bsize, PAGESIZE);

	if ((error = virtiofs_node_getattr(vfn, &fa)) != 0)
		return (error);

reread:
	bp = NULL;
	pp = NULL;

	if (pl != NULL)
		pl[0] = NULL;

	error = 0;

again:
	if ((pagefound = page_exists(vp, off)) == NULL) {
		if (pl == NULL) {
			/*
			 * A read-ahead request.  We have no asynchronous
			 * machinery, and a synchronous round trip here would
			 * make the caller pay for work it did not ask for.
			 */
			return (0);
		}

		blkoff = (off / bsize) * bsize;
		if (blkoff < fa.size && blkoff + bsize >= fa.size) {
			/*
			 * The last block of the file: ask only for what is
			 * really there, but never for less than one page.
			 */
			if (fa.size <= off)
				blksize = off + PAGESIZE - blkoff;
			else
				blksize = fa.size - blkoff;
		} else if (off == 0) {
			blksize = PAGESIZE;
			blkoff = off;
		} else {
			blksize = bsize;
		}

		pp = pvn_read_kluster(vp, off, seg, addr, &io_off, &io_len,
		    blkoff, blksize, 0);

		/*
		 * Another thread got there first and has the page; start over
		 * and pick it up from the cache.
		 */
		if (pp == NULL)
			goto again;

		/*
		 * Round out to whole pages so that the tail of the last page
		 * is zeroed rather than left holding whatever was there.
		 */
		io_len = ptob(btopr(io_len));

		bp = pageio_setup(pp, io_len, vp, B_READ);
		ASSERT(bp != NULL);

		/*
		 * pageio_setup() leaves b_addr at zero, which is what we want:
		 * bp_mapin() reads it as the offset within the first page and
		 * then replaces it with the kernel virtual address of the
		 * mapping it made.
		 */
		ASSERT(bp->b_un.b_addr == 0);

		bp->b_edev = 0;
		bp->b_dev = 0;
		bp->b_lblkno = lbtodb(io_off);
		bp->b_file = vp;
		bp->b_offset = (offset_t)off;
		bp_mapin(bp);

		if (io_off >= fa.size) {
			/*
			 * Entirely past end of file.  There is nothing to ask
			 * the server for.
			 */
			bzero(bp->b_un.b_addr, io_len);
		} else {
			error = virtiofs_readio(vp, bp->b_un.b_addr, io_off,
			    io_len);
		}

		bp_mapout(bp);
		pageio_done(bp);
	}

	if (pl == NULL)
		return (error);

	if (error != 0) {
		if (pp != NULL)
			pvn_read_done(pp, B_ERROR);
		return (error);
	}

	if (pagefound != NULL) {
		se_t se = (rw == S_CREATE ? SE_EXCL : SE_SHARED);

		/*
		 * The page was in the cache; take the right lock on it.  If it
		 * has gone away underneath us, start again.
		 */
		if ((pp = page_lookup(vp, off, se)) == NULL)
			goto reread;

		pl[0] = pp;
		pl[1] = NULL;
		return (0);
	}

	if (pp != NULL)
		pvn_plist_init(pp, pl, plsz, off, io_len, rw);

	return (0);
}

/* ARGSUSED */
static int
virtiofs_getpage(vnode_t *vp, offset_t off, size_t len, uint_t *protp,
    page_t *pl[], size_t plsz, struct seg *seg, caddr_t addr, enum seg_rw rw,
    cred_t *cr, caller_context_t *ct)
{
	vfnode_t *vfn = VTOVF(vp);
	struct fuse_attr fa;
	int error;

	if (vp->v_flag & VNOMAP)
		return (ENOSYS);

	if (off < 0 || off + (offset_t)len < 0)
		return (EINVAL);

	if (vp->v_type != VREG)
		return (EINVAL);

	if (protp != NULL) {
		/*
		 * Read-only: never hand out write permission, or a private
		 * mapping would be able to become dirty without going through
		 * VOP_MAP's refusal.
		 */
		*protp = PROT_ALL & ~PROT_WRITE;
	}

	if ((error = virtiofs_node_getattr(vfn, &fa)) != 0)
		return (error);

	if ((u_offset_t)off + len > fa.size + PAGEOFFSET)
		return (EFAULT);

	return (pvn_getpages(virtiofs_getapage, vp, (u_offset_t)off, len,
	    protp, pl, plsz, seg, addr, rw, cr));
}

/*
 * There is no write path, so a page can only reach here clean.  Destroy it if
 * that is what was asked, and otherwise just unlock it.
 */
/* ARGSUSED */
int
virtiofs_putapage(vnode_t *vp, page_t *pp, u_offset_t *offp, size_t *lenp,
    int flags, cred_t *cr)
{
	if (offp != NULL)
		*offp = pp->p_offset;
	if (lenp != NULL)
		*lenp = PAGESIZE;

	pvn_write_done(pp, B_ERROR | B_WRITE | B_INVAL | B_FORCE | flags);

	return (0);
}

/* ARGSUSED */
static int
virtiofs_putpage(vnode_t *vp, offset_t off, size_t len, int flags, cred_t *cr,
    caller_context_t *ct)
{
	if (vp->v_flag & VNOMAP)
		return (ENOSYS);

	if (!vn_has_cached_data(vp))
		return (0);

	if (len == 0)
		return (pvn_vplist_dirty(vp, off, virtiofs_putapage, flags,
		    cr));

	for (offset_t io_off = off; io_off < off + (offset_t)len;
	    io_off += PAGESIZE) {
		page_t *pp;

		if ((flags & B_INVAL) || (flags & B_ASYNC) == 0) {
			pp = page_lookup(vp, io_off,
			    (flags & (B_INVAL | B_FREE)) ? SE_EXCL : SE_SHARED);
		} else {
			pp = page_lookup_nowait(vp, io_off,
			    (flags & B_FREE) ? SE_EXCL : SE_SHARED);
		}

		if (pp == NULL)
			continue;

		/*
		 * pvn_getdirty() normally does the whole job and returns zero.
		 * A one means the page was dirty, which cannot happen on a
		 * file system with no write path; fake a failed write so that
		 * the page is destroyed rather than queued for one.
		 */
		if (pvn_getdirty(pp, flags) == 1) {
			cmn_err(CE_NOTE, "virtiofs: dirty page on a "
			    "read-only file system");
			pvn_write_done(pp, flags | B_ERROR | B_WRITE |
			    B_INVAL | B_FORCE);
		}
	}

	return (0);
}

/* ARGSUSED */
static int
virtiofs_map(vnode_t *vp, offset_t off, struct as *as, caddr_t *addrp,
    size_t len, uchar_t prot, uchar_t maxprot, uint_t flags, cred_t *cr,
    caller_context_t *ct)
{
	struct segvn_crargs vn_a;
	vfnode_t *vfn = VTOVF(vp);
	struct fuse_attr fa;
	int error;

	if (vp->v_flag & VNOMAP)
		return (ENOSYS);

	/*
	 * A shared writable mapping would create dirty pages that we have
	 * nowhere to put.
	 */
	if ((prot & PROT_WRITE) && (flags & MAP_SHARED))
		return (ENOSYS);

	if (off < 0 || off + (offset_t)len < 0)
		return (ENXIO);

	if (vp->v_type != VREG)
		return (ENODEV);

	if ((error = virtiofs_node_getattr(vfn, &fa)) != 0)
		return (error);

	if (vn_has_mandatory_locks(vp, fa.mode))
		return (EAGAIN);

	as_rangelock(as);
	error = choose_addr(as, addrp, len, off, ADDR_VACALIGN, flags);
	if (error != 0) {
		as_rangeunlock(as);
		return (error);
	}

	vn_a.vp = vp;
	vn_a.offset = (u_offset_t)off;
	vn_a.type = flags & MAP_TYPE;
	vn_a.prot = prot;
	vn_a.maxprot = maxprot & ~PROT_WRITE;
	vn_a.flags = flags & ~MAP_TYPE;
	vn_a.cred = cr;
	vn_a.amp = NULL;
	vn_a.szc = 0;
	vn_a.lgrp_mem_policy_flags = 0;

	error = as_map(as, *addrp, len, segvn_create, &vn_a);
	as_rangeunlock(as);

	return (error);
}

/* ARGSUSED */
static int
virtiofs_addmap(vnode_t *vp, offset_t off, struct as *as, caddr_t addr,
    size_t len, uchar_t prot, uchar_t maxprot, uint_t flags, cred_t *cr,
    caller_context_t *ct)
{
	vfnode_t *vfn = VTOVF(vp);

	if (vp->v_flag & VNOMAP)
		return (ENOSYS);

	mutex_enter(&vfn->vfn_lock);
	vfn->vfn_mapcnt += btopr(len);
	mutex_exit(&vfn->vfn_lock);

	return (0);
}

/* ARGSUSED */
static int
virtiofs_delmap(vnode_t *vp, offset_t off, struct as *as, caddr_t addr,
    size_t len, uint_t prot, uint_t maxprot, uint_t flags, cred_t *cr,
    caller_context_t *ct)
{
	vfnode_t *vfn = VTOVF(vp);

	if (vp->v_flag & VNOMAP)
		return (ENOSYS);

	mutex_enter(&vfn->vfn_lock);
	vfn->vfn_mapcnt -= btopr(len);
	mutex_exit(&vfn->vfn_lock);

	return (0);
}

/* ARGSUSED */
static int
virtiofs_pathconf(vnode_t *vp, int cmd, ulong_t *valp, cred_t *cr,
    caller_context_t *ct)
{
	switch (cmd) {
	case _PC_FILESIZEBITS:
		*valp = 64;
		return (0);
	case _PC_TIMESTAMP_RESOLUTION:
		/*
		 * FUSE carries nanoseconds, whatever the server behind it can
		 * actually resolve.
		 */
		*valp = 1L;
		return (0);
	default:
		return (fs_pathconf(vp, cmd, valp, cr, ct));
	}
}

/*
 * Everything that would modify the file system is absent from this table, so
 * the vnode layer answers it with fs_nosys().
 */
const fs_operation_def_t virtiofs_vnodeops_template[] = {
	VOPNAME_OPEN,		{ .vop_open = virtiofs_open },
	VOPNAME_CLOSE,		{ .vop_close = virtiofs_close },
	VOPNAME_READ,		{ .vop_read = virtiofs_read },
	VOPNAME_GETATTR,	{ .vop_getattr = virtiofs_getattr },
	VOPNAME_ACCESS,		{ .vop_access = virtiofs_access },
	VOPNAME_LOOKUP,		{ .vop_lookup = virtiofs_lookup },
	VOPNAME_READDIR,	{ .vop_readdir = virtiofs_readdir },
	VOPNAME_READLINK,	{ .vop_readlink = virtiofs_readlink },
	VOPNAME_FSYNC,		{ .vop_fsync = virtiofs_fsync },
	VOPNAME_INACTIVE,	{ .vop_inactive = virtiofs_inactive },
	VOPNAME_FID,		{ .vop_fid = virtiofs_fid },
	VOPNAME_SEEK,		{ .vop_seek = virtiofs_seek },
	VOPNAME_FRLOCK,		{ .vop_frlock = virtiofs_frlock },
	VOPNAME_GETPAGE,	{ .vop_getpage = virtiofs_getpage },
	VOPNAME_PUTPAGE,	{ .vop_putpage = virtiofs_putpage },
	VOPNAME_MAP,		{ .vop_map = virtiofs_map },
	VOPNAME_ADDMAP,		{ .vop_addmap = virtiofs_addmap },
	VOPNAME_DELMAP,		{ .vop_delmap = virtiofs_delmap },
	VOPNAME_PATHCONF,	{ .vop_pathconf = virtiofs_pathconf },
	NULL,			NULL
};
