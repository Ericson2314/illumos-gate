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

#ifndef	_FS_VIRTIOFS_FUSE_H
#define	_FS_VIRTIOFS_FUSE_H

/*
 * THE FUSE WIRE PROTOCOL
 *
 * This describes the messages a Virtio FS device exchanges with its driver.
 * illumos has no FUSE subsystem and this header does not create one: there is
 * no character device, no library ABI and no upcall path here, only the
 * on-the-wire structures of the protocol that a Virtio FS device happens to
 * speak.
 *
 * The layouts below are those of protocol version 7.31.  Every field is
 * little endian, which on the platforms this module builds for is also the
 * host order; a big endian port would need byte swapping throughout.
 *
 * Only the subset of the protocol used by a read-only client is declared.
 * Opcodes that would modify the file system are deliberately absent.
 */

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	FUSE_KERNEL_VERSION		7
#define	FUSE_KERNEL_MINOR_VERSION	31

/*
 * The nodeid of the root of any FUSE file system.  Nodeid 0 is never valid.
 */
#define	FUSE_ROOT_ID			1

/*
 * The opcodes a read-only client needs.  The numbering is the protocol's, so
 * the gaps are the write side of the protocol, which we do not implement.
 */
typedef enum fuse_opcode {
	FUSE_LOOKUP		= 1,
	FUSE_FORGET		= 2,	/* no reply */
	FUSE_GETATTR		= 3,
	FUSE_READLINK		= 5,
	FUSE_OPEN		= 14,
	FUSE_READ		= 15,
	FUSE_STATFS		= 17,
	FUSE_RELEASE		= 18,
	FUSE_INIT		= 26,
	FUSE_OPENDIR		= 27,
	FUSE_READDIR		= 28,
	FUSE_RELEASEDIR		= 29
} fuse_opcode_t;

/*
 * Every request begins with this header, and its "len" covers the header
 * itself plus whatever in-args follow.
 */
struct fuse_in_header {
	uint32_t	len;
	uint32_t	opcode;
	uint64_t	unique;
	uint64_t	nodeid;
	uint32_t	uid;
	uint32_t	gid;
	uint32_t	pid;
	uint32_t	padding;
};

/*
 * Every reply begins with this header.  "error" is a negative errno on
 * failure, and zero on success; note that the errno values are Linux's, not
 * illumos', and have to be translated.  On failure "len" is just the size of
 * the header.
 */
struct fuse_out_header {
	uint32_t	len;
	int32_t		error;
	uint64_t	unique;
};

/*
 * The attributes of one file.  "mode" carries the usual S_IF* type bits, in
 * their Linux encoding, which happens to agree with ours for every type we
 * care about.
 */
struct fuse_attr {
	uint64_t	ino;
	uint64_t	size;
	uint64_t	blocks;
	uint64_t	atime;
	uint64_t	mtime;
	uint64_t	ctime;
	uint32_t	atimensec;
	uint32_t	mtimensec;
	uint32_t	ctimensec;
	uint32_t	mode;
	uint32_t	nlink;
	uint32_t	uid;
	uint32_t	gid;
	uint32_t	rdev;
	uint32_t	blksize;
	uint32_t	flags;
};

struct fuse_kstatfs {
	uint64_t	blocks;
	uint64_t	bfree;
	uint64_t	bavail;
	uint64_t	files;
	uint64_t	ffree;
	uint32_t	bsize;
	uint32_t	namelen;
	uint32_t	frsize;
	uint32_t	padding;
	uint32_t	spare[6];
};

/*
 * FUSE_INIT.  The client proposes a version; the server replies with the
 * version it will actually speak, which is never newer than what was asked
 * for.  A server that cannot speak our major version replies with the major
 * version it wants and an error.
 */
struct fuse_init_in {
	uint32_t	major;
	uint32_t	minor;
	uint32_t	max_readahead;
	uint32_t	flags;
};

struct fuse_init_out {
	uint32_t	major;
	uint32_t	minor;
	uint32_t	max_readahead;
	uint32_t	flags;
	uint16_t	max_background;
	uint16_t	congestion_threshold;
	uint32_t	max_write;
	uint32_t	time_gran;
	uint16_t	max_pages;
	uint16_t	map_alignment;
	uint32_t	flags2;
	uint32_t	unused[7];
};

/*
 * FUSE_LOOKUP takes a NUL terminated name as its in-args and replies with a
 * fuse_entry_out.  A successful lookup takes a reference on the nodeid, which
 * must eventually be dropped with a matching count in FUSE_FORGET.
 */
struct fuse_entry_out {
	uint64_t	nodeid;
	uint64_t	generation;
	uint64_t	entry_valid;
	uint64_t	attr_valid;
	uint32_t	entry_valid_nsec;
	uint32_t	attr_valid_nsec;
	struct fuse_attr attr;
};

/*
 * FUSE_FORGET has no reply at all -- not even a fuse_out_header.
 */
struct fuse_forget_in {
	uint64_t	nlookup;
};

/*
 * FUSE_GETATTR.  FUSE_GETATTR_FH says that "fh" is meaningful, i.e. that the
 * request refers to an open file rather than to a nodeid.
 */
#define	FUSE_GETATTR_FH			(1 << 0)

struct fuse_getattr_in {
	uint32_t	getattr_flags;
	uint32_t	dummy;
	uint64_t	fh;
};

struct fuse_attr_out {
	uint64_t	attr_valid;
	uint32_t	attr_valid_nsec;
	uint32_t	dummy;
	struct fuse_attr attr;
};

/*
 * FUSE_OPEN and FUSE_OPENDIR.  "flags" holds open(2) flags in their Linux
 * encoding; a read-only client only ever sends O_RDONLY, which is zero in
 * both encodings.
 */
struct fuse_open_in {
	uint32_t	flags;
	uint32_t	open_flags;
};

struct fuse_open_out {
	uint64_t	fh;
	uint32_t	open_flags;
	int32_t		backing_id;
};

/*
 * FUSE_RELEASE and FUSE_RELEASEDIR.
 */
struct fuse_release_in {
	uint64_t	fh;
	uint32_t	flags;
	uint32_t	release_flags;
	uint64_t	lock_owner;
};

/*
 * FUSE_READ and FUSE_READDIR.  The reply is the fuse_out_header followed by
 * up to "size" bytes of data; the header's "len" says how much arrived.
 */
struct fuse_read_in {
	uint64_t	fh;
	uint64_t	offset;
	uint32_t	size;
	uint32_t	read_flags;
	uint64_t	lock_owner;
	uint32_t	flags;
	uint32_t	padding;
};

/*
 * The FUSE_READDIR reply is a packed stream of these, each followed by
 * "namelen" bytes of name -- not NUL terminated -- and then padding to the
 * next eight byte boundary.  "off" is an opaque cookie to be handed back as
 * the offset of the next FUSE_READDIR; it is not a byte offset and must not be
 * treated as one.
 */
struct fuse_dirent {
	uint64_t	ino;
	uint64_t	off;
	uint32_t	namelen;
	uint32_t	type;		/* a DT_* value, i.e. mode >> 12 */
	char		name[];
};

#define	FUSE_DIRENT_ALIGN(x)	\
	(((x) + sizeof (uint64_t) - 1) & ~(sizeof (uint64_t) - 1))
#define	FUSE_NAME_OFFSET	offsetof(struct fuse_dirent, name)
#define	FUSE_DIRENT_SIZE(d)	\
	FUSE_DIRENT_ALIGN(FUSE_NAME_OFFSET + (d)->namelen)

#ifdef __cplusplus
}
#endif

#endif	/* _FS_VIRTIOFS_FUSE_H */
