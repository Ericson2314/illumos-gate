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

#ifndef _VTFS_H
#define	_VTFS_H

/*
 * VIRTIO FS TRANSPORT
 *
 * This is the private interface between the "vtfs" driver, which owns the
 * Virtio FS PCI devices, and the "virtiofs" file system module, which speaks
 * the FUSE protocol over them.  See the block comment at the top of vtfs.c.
 */

#include <sys/types.h>
#include <sys/list.h>
#include <sys/ksynch.h>
#include <sys/dditypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Virtio FS device configuration space, from the Virtio 1.2 specification,
 * section 5.11.4.  Only "tag" and "num_request_queues" are of interest to us;
 * "notify_buf_size" is only meaningful with VIRTIO_FS_F_NOTIFICATION, which we
 * do not negotiate.
 */
#define	VIRTIO_FS_TAG_LEN		36

#define	VIRTIO_FS_CONFIG_TAG		0x00	/* char[36] */
#define	VIRTIO_FS_CONFIG_NUM_REQ_VQ	0x24	/* uint32_t */
#define	VIRTIO_FS_CONFIG_NOTIFY_BUF_SZ	0x28	/* uint32_t */

/*
 * Virtqueue indices.  Queue 0 is the high priority queue, used only for
 * FUSE_INTERRUPT and FUSE_FORGET; queues 1..n are the request queues.  We
 * presently drive a single request queue.
 */
#define	VIRTIO_FS_VIRTQ_HIPRIO		0
#define	VIRTIO_FS_VIRTQ_REQUEST		1

/*
 * The largest FUSE header-plus-body that we will send or receive out of the
 * per-slot bounce buffer.  A FUSE_LOOKUP carries a NUL terminated name, so
 * this has to comfortably exceed MAXNAMELEN; everything else we send is a
 * fixed size structure of well under a hundred bytes.
 */
#define	VTFS_BOUNCE_SIZE		1024

/*
 * The number of scatter-gather cookies we are willing to use for one request:
 * one each for the outbound and inbound bounce buffers, plus enough for a
 * fully discontiguous large data buffer.
 *
 * PAGESIZE is not a compile time constant in the kernel, so the worst case
 * page count is computed against the smallest page any supported platform
 * has.
 */
#define	VTFS_DATA_MAX			(128 * 1024)
#define	VTFS_MIN_PAGESIZE		4096
#define	VTFS_SEG_MAX			\
	((VTFS_DATA_MAX / VTFS_MIN_PAGESIZE) + 2)

typedef struct vtfs vtfs_t;

/*
 * A single FUSE round trip.  The caller fills in the request fields and calls
 * vtfs_xfer(); on success "vx_resid" holds the number of bytes the device
 * wrote in total, across both the inbound bounce buffer and the data buffer.
 *
 * "vx_data" is an arbitrary kernel virtual address -- typically a page cache
 * mapping established by bp_mapin(9F) -- which the transport binds for DMA for
 * the duration of the request.  It is always written by the device; we have no
 * write path.
 */
typedef struct vtfs_xfer {
	const void	*vx_out;	/* device reads: header and in-args */
	size_t		vx_outlen;
	void		*vx_in;		/* device writes: header and out-args */
	size_t		vx_inlen;
	/* device writes: bulk payload, or NULL */
	void		*vx_data;
	size_t		vx_datalen;
	size_t		vx_resid;	/* set on return */
} vtfs_xfer_t;

extern vtfs_t *vtfs_hold_by_tag(const char *);
extern void vtfs_rele(vtfs_t *);
extern const char *vtfs_tag(vtfs_t *);
extern int vtfs_xfer(vtfs_t *, vtfs_xfer_t *);

#ifdef __cplusplus
}
#endif

#endif /* _VTFS_H */
