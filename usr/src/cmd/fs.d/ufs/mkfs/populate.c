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
 * Populate a freshly made UFS filesystem from a directory tree.
 *
 * mkfs(8) has always produced an *empty* filesystem: a superblock, cylinder
 * groups, a root directory and lost+found, and nothing else.  Putting anything
 * into it means mounting it, which means being the kind of machine that can
 * mount it.  That is the one thing an image builder is not: it wants to hand
 * over a finished root filesystem, from a machine that may not be running
 * illumos at all.
 *
 * The BSDs solved this with a separate makefs(8).  Doing it here instead keeps
 * one definition of the on-disk format -- the same source that lays out the
 * cylinder groups fills them in, so the two cannot drift.  That matters more
 * for UFS than it might sound: illumos' `struct direct` carries a 16-bit
 * d_namlen exactly where 4.4BSD FFS puts a d_type byte and an 8-bit namlen, so
 * a foreign makefs writes directories this kernel cannot read.
 *
 * This runs as a pass over the finished filesystem rather than as part of
 * laying it out.  Everything below re-reads the superblock from the image and
 * does its own I/O, so it depends on nothing mkfs holds in memory and cannot
 * disturb mkfs's own (asynchronous) write path.  The cost is one extra read of
 * the superblock and each cylinder group it touches.
 *
 * What is deliberately *not* here: allocation policy.  ffs tries to keep a
 * file's blocks near its inode and to spread directories across cylinder
 * groups, because that mattered on a spinning disk being written by a running
 * system.  An image is written once, sequentially, and typically read back
 * from something with no seek time at all, so this fills cylinder groups in
 * order and stops.  It is the layout that must be right, not the layout's
 * cleverness.
 */

#include	<stdio.h>
#include	<stdlib.h>
#include	<stdarg.h>
#include	<string.h>
#include	<strings.h>
#include	<unistd.h>
#include	<fcntl.h>
#include	<errno.h>
#include	<dirent.h>
#include	<limits.h>
#include	<sys/param.h>
#include	<sys/types.h>
#include	<sys/stat.h>
#include	<sys/sysmacros.h>
#include	<sys/vnode.h>

/*
 * <dirent.h> and <sys/fs/ufs_fsdir.h> both define MAXNAMLEN, and they mean
 * different things by it: 512 is how long a name the directory *interfaces*
 * accept, 255 is how long a name a UFS `struct direct` can actually hold. The
 * on-disk limit is the one that matters here, and it is the one that has to be
 * in force when struct direct is declared -- so drop the other rather than let
 * the order of these includes decide it.
 */
#undef	MAXNAMLEN
#include	<sys/fs/ufs_fsdir.h>
#include	<sys/fs/ufs_inode.h>
#include	<sys/fs/ufs_fs.h>

#include	"populate.h"

/*
 * Everything is addressed in *fragments*, which is how the on-disk structures
 * count: fs_fsize bytes each, fs_frag of them to a block.
 */
static int		fsfd = -1;
static struct fs	*sb;		/* the superblock we are working against */
static struct csum	*csums;		/* fs_cssize bytes of summary */
static struct cg	*cgp;		/* the one cylinder group we hold */
static int		cgheld = -1;	/* which one, or -1 */
static int		cgdirty;


static void
pfatal(const char *fmt, ...)
{
	va_list ap;

	(void) fprintf(stderr, "mkfs: populate: ");
	va_start(ap, fmt);
	(void) vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void) fprintf(stderr, "\n");
	exit(32);
}

static void *
zalloc(size_t n)
{
	void *p = calloc(1, n);

	if (p == NULL)
		pfatal("out of memory (%lu bytes)", (ulong_t)n);
	return (p);
}

/*
 * Fragment-addressed I/O.  pread/pwrite rather than seek-and-read so that
 * nothing here depends on, or disturbs, the file offset mkfs is using.
 */
static void
devread(daddr32_t frag, void *buf, size_t len)
{
	off_t	off = (off_t)frag * sb->fs_fsize;
	ssize_t	n = pread(fsfd, buf, len, off);

	if (n == (ssize_t)len)
		return;
	if (n < 0)
		pfatal("read of %lu bytes at frag %d failed: %s",
		    (ulong_t)len, frag, strerror(errno));
	pfatal("short read at frag %d: %ld of %lu bytes", frag, (long)n,
	    (ulong_t)len);
}

/*
 * Byte-offset I/O, for the one structure that is not addressed in fragments:
 * SBLOCK is expressed in DEV_BSIZE sectors, unlike cgtod(), itod() and
 * fs_csaddr, which are all fragment numbers. Mixing the two silently writes
 * the superblock past where anything looks for it.
 */
static void
devwrite_at(off_t off, const void *buf, size_t len)
{
	ssize_t n = pwrite(fsfd, buf, len, off);

	if (n == (ssize_t)len)
		return;
	if (n < 0)
		pfatal("write of %lu bytes at offset %lld failed: %s",
		    (ulong_t)len, (long long)off, strerror(errno));
	pfatal("short write at offset %lld: %ld of %lu bytes",
	    (long long)off, (long)n, (ulong_t)len);
}

static void
devwrite(daddr32_t frag, const void *buf, size_t len)
{
	off_t	off = (off_t)frag * sb->fs_fsize;
	ssize_t	n = pwrite(fsfd, buf, len, off);

	if (n == (ssize_t)len)
		return;
	if (n < 0)
		pfatal("write of %lu bytes at frag %d failed: %s",
		    (ulong_t)len, frag, strerror(errno));
	pfatal("short write at frag %d: %ld of %lu bytes", frag, (long)n,
	    (ulong_t)len);
}

static void
cgflush(void)
{
	if (cgheld >= 0 && cgdirty)
		devwrite(cgtod(sb, cgheld), cgp, sb->fs_cgsize);
	cgdirty = 0;
}

static void
cgload(int c)
{
	if (cgheld == c)
		return;
	cgflush();
	devread(cgtod(sb, c), cgp, sb->fs_cgsize);
	if (cgp->cg_magic != CG_MAGIC)
		pfatal("cg %d: bad magic number", c);
	cgheld = c;
}

/*
 * The free map is per fragment.  `isblock`-style helpers in mkfs work on
 * whole blocks through masks that vary with fs_frag; testing the fragments
 * one at a time says the same thing and does not care what fs_frag is.
 */
static int
frag_free(daddr32_t cgfrag)
{
	return (isset(cg_blksfree(cgp), cgfrag) != 0);
}

static int
block_free(daddr32_t cgfrag)
{
	int i;

	for (i = 0; i < sb->fs_frag; i++)
		if (!frag_free(cgfrag + i))
			return (0);
	return (1);
}

/*
 * Account for a whole block leaving the free pool.  The rotational-position
 * summaries are what fsck checks against, so they have to move too, even
 * though nothing in an image cares where the heads are.
 */
static void
block_taken(daddr32_t cgfrag)
{
	daddr32_t abs = cgbase(sb, cgheld) + cgfrag;
	int i;

	for (i = 0; i < sb->fs_frag; i++)
		clrbit(cg_blksfree(cgp), cgfrag + i);

	cgp->cg_cs.cs_nbfree--;
	sb->fs_cstotal.cs_nbfree--;
	csums[cgheld].cs_nbfree--;

	cg_blktot(cgp)[cbtocylno(sb, cgfrag)]--;
	cg_blks(sb, cgp, cbtocylno(sb, cgfrag))[cbtorpos(sb, cgfrag)]--;
	cgdirty = 1;

	/* Silence "set but not used" in builds without the assertion. */
	(void) abs;
}

/*
 * Where to carry on looking for space.  Nothing here ever frees, so the cursor
 * only moves forwards, and the whole run costs one pass over the filesystem
 * rather than one pass per file.
 */
static int		scan_cg;	/* cylinder group the cursor is in */
static daddr32_t	scan_blk;	/* next untried block, relative to it */

/*
 * The block that partial allocations are being carved out of.  Without this,
 * every file's tail would strand the rest of a block -- on a tree of many
 * small files that is most of the image.
 */
static int		part_cg = -1;	/* -1 when there is no such block */
static daddr32_t	part_blk;	/* its first fragment, cg-relative */
static int		part_used;	/* how many of its fragments are gone */

/*
 * Take the next wholly free block and mark all of its fragments used.  Returns
 * the block's first fragment relative to its cylinder group, and leaves that
 * group loaded.
 */
static daddr32_t
alloc_block_raw(void)
{
	for (; scan_cg < sb->fs_ncg; scan_cg++, scan_blk = 0) {
		cgload(scan_cg);
		for (; scan_blk + sb->fs_frag <= cgp->cg_ndblk;
		    scan_blk += sb->fs_frag) {
			if (!block_free(scan_blk))
				continue;
			block_taken(scan_blk);
			return (scan_blk);
		}
	}

	pfatal("filesystem is full. Make the image larger.");
	/*NOTREACHED*/
	return (0);
}

/*
 * Mark `nfrags` fragments at the front of a just-taken block as used and give
 * the rest back, which is what makes it available to carve from again.
 */
static void
block_split(daddr32_t blk, int nfrags)
{
	int left = sb->fs_frag - nfrags;
	int i;

	if (left <= 0) {
		part_cg = -1;
		return;
	}

	for (i = nfrags; i < sb->fs_frag; i++)
		setbit(cg_blksfree(cgp), blk + i);

	cgp->cg_cs.cs_nffree += left;
	sb->fs_cstotal.cs_nffree += left;
	csums[cgheld].cs_nffree += left;
	cgp->cg_frsum[left]++;
	cgdirty = 1;

	part_cg = cgheld;
	part_blk = blk;
	part_used = nfrags;
}

/*
 * Allocate `nfrags` contiguous fragments and return their absolute fragment
 * number.  nfrags == fs_frag means a whole block.
 */
static daddr32_t
alloc_frags(int nfrags)
{
	daddr32_t	blk;
	int		i;

	if (nfrags <= 0 || nfrags > sb->fs_frag)
		pfatal("internal error: bad fragment count %d", nfrags);

	if (nfrags == sb->fs_frag) {
		blk = alloc_block_raw();
		return (cgbase(sb, scan_cg) + blk);
	}

	/* Carve from the block already part way through, when it still fits. */
	if (part_cg >= 0 && sb->fs_frag - part_used >= nfrags) {
		int left = sb->fs_frag - part_used;

		cgload(part_cg);
		cgp->cg_frsum[left]--;
		for (i = 0; i < nfrags; i++)
			clrbit(cg_blksfree(cgp), part_blk + part_used + i);
		cgp->cg_cs.cs_nffree -= nfrags;
		sb->fs_cstotal.cs_nffree -= nfrags;
		csums[part_cg].cs_nffree -= nfrags;
		if (left > nfrags)
			cgp->cg_frsum[left - nfrags]++;
		cgdirty = 1;

		blk = cgbase(sb, part_cg) + part_blk + part_used;
		part_used += nfrags;
		if (part_used == sb->fs_frag)
			part_cg = -1;
		return (blk);
	}

	/*
	 * Otherwise start a new one.  Whatever was left of the old block stays
	 * free and simply stops being reused; it is at most fs_frag - 1
	 * fragments, once per run of allocations that outgrew it.
	 */
	blk = alloc_block_raw();
	block_split(blk, nfrags);
	return (cgbase(sb, scan_cg) + blk);
}


/*
 * Allocate an inode.  Inode numbers are dense per cylinder group, so this is
 * a straight scan of each group's used map.
 */
static ino_t
alloc_inode(int isdir)
{
	int	c;
	ino_t	i;

	for (c = 0; c < sb->fs_ncg; c++) {
		cgload(c);
		for (i = 0; i < (ino_t)sb->fs_ipg; i++) {
			if (isset(cg_inosused(cgp), i))
				continue;

			setbit(cg_inosused(cgp), i);
			cgp->cg_cs.cs_nifree--;
			sb->fs_cstotal.cs_nifree--;
			csums[c].cs_nifree--;
			if (isdir) {
				cgp->cg_cs.cs_ndir++;
				sb->fs_cstotal.cs_ndir++;
				csums[c].cs_ndir++;
			}
			cgdirty = 1;
			return ((ino_t)c * sb->fs_ipg + i);
		}
	}

	pfatal("filesystem is out of inodes. Make the image larger, or pass "
	    "a smaller `nbpi` so that mkfs lays down more of them.");
	/*NOTREACHED*/
	return (0);
}

static void
inode_read(ino_t ino, struct dinode *dp)
{
	struct dinode	*blk = zalloc(sb->fs_bsize);

	devread(itod(sb, (int)ino), blk, sb->fs_bsize);
	*dp = blk[itoo(sb, (int)ino)];
	free(blk);
}

static void
inode_write(ino_t ino, const struct dinode *dp)
{
	struct dinode	*blk = zalloc(sb->fs_bsize);

	devread(itod(sb, (int)ino), blk, sb->fs_bsize);
	blk[itoo(sb, (int)ino)] = *dp;
	devwrite(itod(sb, (int)ino), blk, sb->fs_bsize);
	free(blk);
}

/*
 * Hard links.  A source file with more than one link must become one inode
 * with more than one directory entry, or the tree silently gains copies --
 * which for a store full of hard-linked files changes the size of the image
 * by a large factor.
 */
struct linkmap {
	struct linkmap	*lm_next;
	dev_t		lm_dev;
	ino_t		lm_ino;		/* in the *source* filesystem */
	ino_t		lm_target;	/* what we allocated for it */
};
static struct linkmap *links;

static ino_t
link_lookup(dev_t dev, ino_t ino)
{
	struct linkmap *lm;

	for (lm = links; lm != NULL; lm = lm->lm_next)
		if (lm->lm_dev == dev && lm->lm_ino == ino)
			return (lm->lm_target);
	return (0);
}

static void
link_record(dev_t dev, ino_t ino, ino_t target)
{
	struct linkmap *lm = zalloc(sizeof (*lm));

	lm->lm_dev = dev;
	lm->lm_ino = ino;
	lm->lm_target = target;
	lm->lm_next = links;
	links = lm;
}

/*
 * Attach one data block to an inode at logical block `lbn`, allocating
 * indirect blocks as needed.
 *
 * UFS addresses the first NDADDR blocks straight from the inode and everything
 * after that through one, two or three levels of indirection.  The recursion
 * here is over those levels: at each one it works out which slot of the
 * current indirect block the logical number falls in, faulting the block in
 * (or creating it) on the way down.
 */
static void
inode_addblk(struct dinode *dp, daddr32_t lbn, daddr32_t frag, int *nblocks)
{
	daddr32_t	*ind;
	daddr32_t	off, lvlsz;
	int		level, i;

	if (lbn < NDADDR) {
		dp->di_db[lbn] = frag;
		return;
	}

	off = lbn - NDADDR;

	/*
	 * Which level of indirection covers this logical block: a single
	 * indirect block reaches NINDIR blocks, a double NINDIR^2, and so on.
	 */
	lvlsz = NINDIR(sb);
	for (level = 0; level < NIADDR; level++) {
		if (off < lvlsz)
			break;
		off -= lvlsz;
		lvlsz *= NINDIR(sb);
	}
	if (level == NIADDR)
		pfatal("file is too large for this filesystem");

	if (dp->di_ib[level] == 0) {
		dp->di_ib[level] = alloc_frags(sb->fs_frag);
		(*nblocks) += btodb(sb->fs_bsize);
		ind = zalloc(sb->fs_bsize);
		devwrite(dp->di_ib[level], ind, sb->fs_bsize);
		free(ind);
	}

	/*
	 * Walk down from the top of this level's tree, creating the
	 * intermediate blocks as we meet them.
	 */
	ind = zalloc(sb->fs_bsize);
	{
		daddr32_t cur = dp->di_ib[level];
		daddr32_t span = 1;

		for (i = 0; i < level; i++)
			span *= NINDIR(sb);

		for (i = level; i >= 0; i--) {
			daddr32_t slot = (span == 0) ? off : (off / span);

			devread(cur, ind, sb->fs_bsize);
			if (i == 0) {
				ind[off % NINDIR(sb)] = frag;
				devwrite(cur, ind, sb->fs_bsize);
				break;
			}
			if (ind[slot] == 0) {
				daddr32_t nb = alloc_frags(sb->fs_frag);
				daddr32_t *fresh = zalloc(sb->fs_bsize);

				(*nblocks) += btodb(sb->fs_bsize);
				devwrite(nb, fresh, sb->fs_bsize);
				free(fresh);
				ind[slot] = nb;
				devwrite(cur, ind, sb->fs_bsize);
			}
			cur = ind[slot];
			off %= span;
			span /= NINDIR(sb);
		}
	}
	free(ind);
}

/*
 * Copy `size` bytes from `fd` into a fresh inode's data blocks.
 *
 * Every block but the last is a whole fs_bsize block; the last is rounded up
 * to whole fragments, which is what makes a filesystem of many small files
 * affordable and is also what fsck expects when it recomputes i_blocks from
 * i_size.
 */
static void
inode_writedata(struct dinode *dp, int fd, off_t size, const char *what)
{
	char		*buf = zalloc(sb->fs_bsize);
	off_t		done = 0;
	daddr32_t	lbn = 0;
	int		nblocks = 0;

	while (done < size) {
		off_t	left = size - done;
		int	want = (left >= sb->fs_bsize) ? sb->fs_bsize :
		    (int)left;
		int	nfrags = (left >= sb->fs_bsize) ? sb->fs_frag :
		    (int)numfrags(sb, fragroundup(sb, want));
		daddr32_t frag;
		ssize_t	n;

		bzero(buf, sb->fs_bsize);
		n = read(fd, buf, want);
		if (n < 0)
			pfatal("%s: read failed: %s", what, strerror(errno));
		if (n != want)
			pfatal("%s: short read (%ld of %d); did it change "
			    "underneath us?", what, (long)n, want);

		frag = alloc_frags(nfrags);
		devwrite(frag, buf, (size_t)nfrags * sb->fs_fsize);
		nblocks += btodb(nfrags * sb->fs_fsize);

		inode_addblk(dp, lbn, frag, &nblocks);
		lbn++;
		done += want;
	}

	dp->di_size = size;
	dp->di_blocks = nblocks;
	free(buf);
}

/*
 * A directory is a sequence of DIRBLKSIZ chunks.  Within a chunk the entries'
 * d_reclen values tile it exactly, and the last entry's reclen runs to the end
 * of the chunk -- that slack is where the kernel puts new names later.  An
 * entry may not straddle a chunk boundary.
 */
/*
 * The fixed part of a struct direct, before the name. Anything less than this
 * left in a chunk cannot hold another entry.
 */
#define	DIRHDRSIZ	(sizeof (struct direct) - (MAXNAMLEN + 1))

struct dirbuf {
	char	*db_buf;
	size_t	db_size;	/* bytes in use, always a DIRBLKSIZ multiple */
	size_t	db_cap;
	size_t	db_used;	/* bytes used in the current chunk */
};

static void
dirbuf_init(struct dirbuf *d)
{
	d->db_cap = DIRBLKSIZ * 8;
	d->db_buf = zalloc(d->db_cap);
	d->db_size = DIRBLKSIZ;
	d->db_used = 0;
}

static void
dirbuf_addentry(struct dirbuf *d, ino_t ino, const char *name)
{
	struct direct	*ep;
	size_t		namlen = strlen(name);
	size_t		need;

	if (namlen > MAXNAMLEN)
		pfatal("name too long for UFS (%lu > %d): %s",
		    (ulong_t)namlen, MAXNAMLEN, name);

	/* DIRSIZ, without needing a struct direct to ask about. */
	need = DIRHDRSIZ + ((namlen + 1 + 3) & ~3);

	if (d->db_used + need > DIRBLKSIZ) {
		/*
		 * No room in this chunk: give the slack to the previous
		 * entry and start a new one.
		 */
		if (d->db_size + DIRBLKSIZ > d->db_cap) {
			d->db_cap *= 2;
			d->db_buf = realloc(d->db_buf, d->db_cap);
			if (d->db_buf == NULL)
				pfatal("out of memory growing a directory");
			bzero(d->db_buf + d->db_size,
			    d->db_cap - d->db_size);
		}
		d->db_size += DIRBLKSIZ;
		d->db_used = 0;
	}

	ep = (struct direct *)(d->db_buf + (d->db_size - DIRBLKSIZ) +
	    d->db_used);
	ep->d_ino = (uint32_t)ino;
	ep->d_namlen = (ushort_t)namlen;
	ep->d_reclen = (ushort_t)need;
	(void) strcpy(ep->d_name, name);

	d->db_used += need;
}

/*
 * Close off a directory: give each chunk's leftover space to the last real
 * entry in it, by stretching that entry's d_reclen to the end of the chunk.
 *
 * It has to go to an existing entry rather than to a new empty one. The slack
 * can be as little as four bytes -- shorter than a struct direct's fixed part
 * -- and writing an entry header there runs off the end of the chunk and into
 * the *next* chunk's first d_ino. That corruption is quiet and specific: the
 * following name keeps working but points at whatever inode number the stray
 * two bytes happen to spell.
 *
 * mkfs's own makedir() does the same thing for the root directory, for the
 * same reason.
 */
static void
dirbuf_finish(struct dirbuf *d)
{
	size_t chunk;

	for (chunk = 0; chunk < d->db_size; chunk += DIRBLKSIZ) {
		char	*base = d->db_buf + chunk;
		size_t	off = 0, last = 0;

		/*
		 * Entries were written contiguously from the start of the
		 * chunk and the rest is still zero, so a zero d_reclen marks
		 * the end of the real ones.
		 */
		for (;;) {
			struct direct *ep;

			/*
			 * Stop before reading a header that does not fit: the
			 * fields of a struct direct at this offset would lie in
			 * the *next* chunk, so d_reclen would be read out of the
			 * following entry's d_ino and the slack mistaken for a
			 * real entry.
			 */
			if (off + DIRHDRSIZ > DIRBLKSIZ)
				break;
			ep = (struct direct *)(base + off);
			if (ep->d_reclen == 0)
				break;
			last = off;
			off += ep->d_reclen;
			if (off >= DIRBLKSIZ)
				break;
		}

		((struct direct *)(base + last))->d_reclen =
		    (ushort_t)(DIRBLKSIZ - last);
	}
}

static void
dirbuf_free(struct dirbuf *d)
{
	free(d->db_buf);
	d->db_buf = NULL;
}

/*
 * Write a built-up directory image into an inode's blocks.  Same block/frag
 * rules as a regular file; a directory's size is always a DIRBLKSIZ multiple,
 * so the tail is fragment-aligned by construction.
 */
static void
dir_store(struct dinode *dp, const struct dirbuf *d)
{
	size_t		done = 0;
	daddr32_t	lbn = 0;
	int		nblocks = 0;

	while (done < d->db_size) {
		size_t	left = d->db_size - done;
		int	want = (left >= (size_t)sb->fs_bsize) ?
		    sb->fs_bsize : (int)left;
		int	nfrags = (int)numfrags(sb, fragroundup(sb, want));
		daddr32_t frag = alloc_frags(nfrags);
		char	*chunk = zalloc((size_t)nfrags * sb->fs_fsize);

		bcopy(d->db_buf + done, chunk, want);
		devwrite(frag, chunk, (size_t)nfrags * sb->fs_fsize);
		free(chunk);

		nblocks += btodb(nfrags * sb->fs_fsize);
		inode_addblk(dp, lbn, frag, &nblocks);
		lbn++;
		done += want;
	}

	dp->di_size = d->db_size;
	dp->di_blocks = nblocks;
}

/*
 * Ownership and timestamps.
 *
 * uid/gid are forced to root rather than carried over: the source tree belongs
 * to whoever ran the build, and an image that says so is wrong everywhere it
 * is later used.  Mode bits *are* carried over -- the execute bit in
 * particular is a property of the content, not of the builder.
 *
 * Times are taken from the source too, so that a reproducible input tree
 * yields a reproducible image.
 */
static void
inode_setmeta(struct dinode *dp, const struct stat64 *st)
{
	dp->di_uid = 0;
	dp->di_gid = 0;
	dp->di_suid = 0;
	dp->di_sgid = 0;
	dp->di_atime = (time32_t)st->st_atime;
	dp->di_mtime = (time32_t)st->st_mtime;
	dp->di_ctime = (time32_t)st->st_ctime;
}

static ino_t populate_tree(const char *path, ino_t parent, int isroot);

/*
 * One entry of a directory: work out what it is, give it an inode, and hand
 * back the inode number for the caller's directory entry.
 */
static ino_t
populate_entry(const char *path, const char *name, ino_t parent)
{
	struct stat64	st;
	struct dinode	dp;
	ino_t		ino;

	if (lstat64(path, &st) != 0)
		pfatal("%s: cannot stat: %s", path, strerror(errno));

	if (S_ISDIR(st.st_mode))
		return (populate_tree(path, parent, 0));

	/* An already-seen hard link needs no new inode, only a new name. */
	if (!S_ISDIR(st.st_mode) && st.st_nlink > 1) {
		ino = link_lookup(st.st_dev, st.st_ino);
		if (ino != 0) {
			inode_read(ino, &dp);
			dp.di_nlink++;
			inode_write(ino, &dp);
			return (ino);
		}
	}

	bzero(&dp, sizeof (dp));
	dp.di_nlink = 1;
	inode_setmeta(&dp, &st);

	if (S_ISREG(st.st_mode)) {
		int fd = open64(path, O_RDONLY);

		if (fd < 0)
			pfatal("%s: cannot open: %s", path, strerror(errno));
		dp.di_mode = IFREG | (st.st_mode & 07777);
		ino = alloc_inode(0);
		inode_writedata(&dp, fd, st.st_size, path);
		(void) close(fd);
	} else if (S_ISLNK(st.st_mode)) {
		char		target[PATH_MAX + 1];
		ssize_t		n = readlink(path, target, sizeof (target) - 1);
		daddr32_t	frag;
		char		*blk;
		int		nfrags;

		if (n < 0)
			pfatal("%s: cannot read link: %s", path,
			    strerror(errno));
		target[n] = '\0';

		/*
		 * illumos UFS has no "fast symlink": there is no
		 * fs_maxsymlinklen, so the target always lives in a data
		 * block rather than in the inode's block pointers.
		 */
		dp.di_mode = IFLNK | 0777;
		ino = alloc_inode(0);
		nfrags = (int)numfrags(sb, fragroundup(sb, (int)n));
		if (nfrags == 0)
			nfrags = 1;
		frag = alloc_frags(nfrags);
		blk = zalloc((size_t)nfrags * sb->fs_fsize);
		bcopy(target, blk, (size_t)n);
		devwrite(frag, blk, (size_t)nfrags * sb->fs_fsize);
		free(blk);

		dp.di_db[0] = frag;
		dp.di_size = n;
		dp.di_blocks = btodb(nfrags * sb->fs_fsize);
	} else {
		/*
		 * Device nodes, fifos and sockets. /dev on illumos is built by
		 * devfs at run time, so an image has no business carrying
		 * them; say so rather than quietly dropping the entry.
		 */
		pfatal("%s: not a regular file, directory or symbolic link. "
		    "Special files cannot be placed in an image; devfs makes "
		    "them at boot.", path);
		/*NOTREACHED*/
		return (0);
	}

	if (st.st_nlink > 1)
		link_record(st.st_dev, st.st_ino, ino);

	inode_write(ino, &dp);
	return (ino);
}

/*
 * Populate one directory, recursively.
 *
 * `isroot` means write into the root inode mkfs already made rather than
 * allocating a new one: the root's number is fixed (UFSROOTINO) and the kernel
 * will look for it there.
 */
static ino_t
populate_tree(const char *path, ino_t parent, int isroot)
{
	struct stat64	st;
	struct dinode	dp;
	struct dirbuf	db;
	struct dirent	*de;
	DIR		*dir;
	ino_t		ino;
	int		nsub = 0;
	char		**names = NULL;
	size_t		nnames = 0, cap = 0;
	size_t		i;

	if (lstat64(path, &st) != 0)
		pfatal("%s: cannot stat: %s", path, strerror(errno));

	ino = isroot ? UFSROOTINO : alloc_inode(1);

	/*
	 * Read the whole directory before descending. readdir(3C) state is
	 * per-DIR and we are about to open many more of them, and sorting the
	 * names makes the resulting image depend on the tree's content rather
	 * than on the order the host filesystem happens to return.
	 */
	if ((dir = opendir(path)) == NULL)
		pfatal("%s: cannot open directory: %s", path, strerror(errno));
	while ((de = readdir(dir)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0)
			continue;
		if (nnames == cap) {
			cap = cap ? cap * 2 : 32;
			names = realloc(names, cap * sizeof (*names));
			if (names == NULL)
				pfatal("out of memory reading %s", path);
		}
		names[nnames] = strdup(de->d_name);
		if (names[nnames] == NULL)
			pfatal("out of memory reading %s", path);
		nnames++;
	}
	(void) closedir(dir);

	for (i = 1; i < nnames; i++) {
		char	*key = names[i];
		size_t	j = i;

		while (j > 0 && strcmp(names[j - 1], key) > 0) {
			names[j] = names[j - 1];
			j--;
		}
		names[j] = key;
	}

	dirbuf_init(&db);
	dirbuf_addentry(&db, ino, ".");
	dirbuf_addentry(&db, parent, "..");

	/*
	 * The root directory is rewritten from scratch, so it has to carry
	 * over what mkfs already put there. mkfs creates lost+found before
	 * this runs and counts it in the cylinder groups; dropping its name
	 * would leave the inode allocated but unreferenced, which fsck reports
	 * as an unconnected directory, and would leave the root's link count
	 * one short.
	 *
	 * Ask the filesystem rather than assuming: whether lost+found exists
	 * is a compile-time choice in mkfs (LOSTDIR).
	 */
	if (isroot) {
		struct dinode lfd;

		inode_read(LOSTFOUNDINO, &lfd);
		if ((lfd.di_mode & IFMT) == IFDIR) {
			dirbuf_addentry(&db, LOSTFOUNDINO, "lost+found");
			nsub++;
		}
	}

	for (i = 0; i < nnames; i++) {
		char	*sub;
		ino_t	subino;
		size_t	len = strlen(path) + 1 + strlen(names[i]) + 1;

		sub = zalloc(len);
		(void) snprintf(sub, len, "%s/%s", path, names[i]);

		subino = populate_entry(sub, names[i], ino);
		dirbuf_addentry(&db, subino, names[i]);

		{
			struct stat64 sst;

			if (lstat64(sub, &sst) == 0 && S_ISDIR(sst.st_mode))
				nsub++;
		}

		free(sub);
		free(names[i]);
	}
	free(names);

	dirbuf_finish(&db);

	bzero(&dp, sizeof (dp));
	dp.di_mode = IFDIR | (st.st_mode & 07777);
	/* "." plus the parent's entry for us, plus one ".." per subdirectory */
	dp.di_nlink = (short)(2 + nsub);
	inode_setmeta(&dp, &st);
	dir_store(&dp, &db);
	inode_write(ino, &dp);

	dirbuf_free(&db);
	return (ino);
}

/*
 * Write the superblock and the cylinder-group summary back out, including the
 * per-group backup copies -- fsck -o b= reads those, and a backup that
 * disagrees with the primary is worse than no backup.
 */
static void
flush_super(void)
{
	int	c;
	char	*buf;

	cgflush();

	sb->fs_fmod = 0;
	sb->fs_clean = FSCLEAN;
	sb->fs_ronly = 0;

	devwrite(sb->fs_csaddr, csums, sb->fs_cssize);
	devwrite_at((off_t)SBLOCK * DEV_BSIZE, sb, SBSIZE);

	buf = zalloc(SBSIZE);
	bcopy(sb, buf, SBSIZE);
	for (c = 0; c < sb->fs_ncg; c++)
		devwrite(cgsblock(sb, c), buf, SBSIZE);
	free(buf);
}

void
ufs_populate(int fd, const char *dir)
{
	struct stat64	st;

	if (stat64(dir, &st) != 0)
		pfatal("%s: cannot stat: %s", dir, strerror(errno));
	if (!S_ISDIR(st.st_mode))
		pfatal("%s: not a directory", dir);

	fsfd = fd;

	/*
	 * Read the superblock back off the image rather than using the one
	 * mkfs has in memory: this pass has to see the filesystem exactly as
	 * it was written, and reading it is also a check that it was.
	 */
	sb = zalloc(SBSIZE);
	{
		off_t	off = (off_t)SBLOCK * DEV_BSIZE;
		ssize_t	n = pread(fd, sb, SBSIZE, off);

		if (n != SBSIZE)
			pfatal("cannot read the superblock back: %s",
			    n < 0 ? strerror(errno) : "short read");
	}
	if (sb->fs_magic != FS_MAGIC && sb->fs_magic != MTB_UFS_MAGIC)
		pfatal("no UFS superblock where one was just written "
		    "(magic 0x%x)", sb->fs_magic);

	csums = zalloc(sb->fs_cssize);
	devread(sb->fs_csaddr, csums, sb->fs_cssize);

	cgp = zalloc(sb->fs_cgsize);

	(void) populate_tree(dir, UFSROOTINO, 1);

	flush_super();

	free(cgp);
	free(csums);
	free(sb);
	sb = NULL;
}
