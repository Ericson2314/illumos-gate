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
 * VIRTIO FS TRANSPORT DRIVER
 *
 * A Virtio FS device (PCI 1af4:105a) is a FUSE server reachable over a
 * virtqueue: the payload of every descriptor chain is an ordinary FUSE
 * message.  This driver owns the device and the queue; the FUSE protocol
 * itself, and the file system built on top of it, live in the separate
 * "virtiofs" module.  The split follows the shape of the module namespace
 * rather than any deep layering: a driver has to live in kernel/drv so that
 * the device tree can bind it, and a file system has to live in kernel/fs so
 * that domount() can load it by name, and no single module can be in both
 * places.
 *
 * Each device advertises a "tag" in its configuration space.  That tag is what
 * a mount names as its special: "mount -F virtiofs <tag> /mnt".  Attached
 * instances are kept on a global list and looked up by tag with
 * vtfs_hold_by_tag().
 *
 * REQUEST CHAIN LAYOUT
 *
 * The Virtio FS specification requires that a request chain be split into a
 * device-readable part followed by a device-writable part, and that the FUSE
 * header begin each part.  We use at most three logical buffers:
 *
 *    +-0-----------------------------------------+
 *    | struct fuse_in_header, then the in-args    |  device reads
 *    |   (a per-slot bounce buffer)               |
 *    +-1-----------------------------------------+
 *    | struct fuse_out_header, then the out-args  |  device writes
 *    |   (a per-slot bounce buffer)               |
 *    +-2-----------------------------------------+
 *    | bulk payload                               |  device writes
 *    |   (caller's memory, bound for the duration |
 *    |    of the request; may span many cookies)  |
 *    +--------------------------------------------+
 *
 * Only FUSE_READ and FUSE_READDIR use the third buffer.  Everything else we
 * send has a small fixed size body that fits comfortably in the bounce
 * buffers, which keeps the DMA bookkeeping to one allocation per slot made
 * once at attach time.
 *
 * Requests are synchronous.  A caller takes a free slot (blocking if there are
 * none), submits, and sleeps on the slot's condition variable until the
 * interrupt handler returns the chain.  This is not the last word in
 * performance, but it is what a read-only file system serving a page fault
 * needs, and it is simple enough to reason about.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/conf.h>
#include <sys/devops.h>
#include <sys/ksynch.h>
#include <sys/stat.h>
#include <sys/modctl.h>
#include <sys/debug.h>
#include <sys/pci.h>
#include <sys/list.h>
#include <sys/sysmacros.h>
#include <sys/sunddi.h>
#include <sys/cmn_err.h>

#include "virtio.h"
#include "vtfs.h"

/*
 * We negotiate no device specific features.  VIRTIO_FS_F_NOTIFICATION (bit 0)
 * would give us the notification queue, which a read-only client has no use
 * for.
 */
#define	VTFS_WANTED_FEATURES		0

/*
 * How many requests may be in flight at once.  Capped again at attach time by
 * the size of the virtqueue the device gives us.
 */
#define	VTFS_MAX_SLOTS			32

typedef enum vtfs_slot_state {
	VTFS_SLOT_FREE = 0,
	VTFS_SLOT_INFLIGHT,
	VTFS_SLOT_DONE
} vtfs_slot_state_t;

typedef struct vtfs_slot {
	list_node_t		vs_link;
	struct vtfs		*vs_vtfs;
	virtio_chain_t		*vs_chain;

	/*
	 * The outbound and inbound bounce buffers are two halves of a single
	 * DMA allocation, so that a slot costs one handle rather than two.
	 */
	virtio_dma_t		*vs_bounce;

	/* Bound per request, when the caller supplies a bulk buffer. */
	virtio_dma_t		*vs_data;
	boolean_t		vs_data_bound;

	vtfs_slot_state_t	vs_state;
	size_t			vs_resid;
} vtfs_slot_t;

struct vtfs {
	list_node_t		vtf_link;	/* vtfs_devices */
	dev_info_t		*vtf_dip;
	virtio_t		*vtf_virtio;
	virtio_queue_t		*vtf_vq;

	kmutex_t		vtf_mutex;
	kcondvar_t		vtf_cv;		/* slot became free or done */

	char			vtf_tag[VIRTIO_FS_TAG_LEN + 1];
	uint32_t		vtf_nrequestq;

	uint_t			vtf_nslots;
	vtfs_slot_t		*vtf_slots;
	list_t			vtf_free;

	uint_t			vtf_refcnt;	/* held mounts */
	boolean_t		vtf_detaching;
};

/*
 * Every attached instance, so that a mount can find one by tag.  The lock also
 * covers each instance's vtf_refcnt.
 */
static kmutex_t vtfs_devices_lock;
static list_t vtfs_devices;

static int vtfs_attach(dev_info_t *, ddi_attach_cmd_t);
static int vtfs_detach(dev_info_t *, ddi_detach_cmd_t);
static int vtfs_quiesce(dev_info_t *);
static uint_t vtfs_int_handler(caddr_t, caddr_t);

static struct dev_ops vtfs_dev_ops = {
	.devo_rev =			DEVO_REV,
	.devo_refcnt =			0,

	.devo_attach =			vtfs_attach,
	.devo_detach =			vtfs_detach,
	.devo_quiesce =			vtfs_quiesce,

	.devo_getinfo =			ddi_no_info,
	.devo_identify =		nulldev,
	.devo_probe =			nulldev,
	.devo_reset =			nodev,
	.devo_cb_ops =			NULL,
	.devo_bus_ops =			NULL,
	.devo_power =			NULL,
};

static struct modldrv vtfs_modldrv = {
	.drv_modops =			&mod_driverops,
	.drv_linkinfo =			"VIRTIO FS transport driver",
	.drv_dev_ops =			&vtfs_dev_ops
};

static struct modlinkage vtfs_modlinkage = {
	.ml_rev =			MODREV_1,
	.ml_linkage =			{ &vtfs_modldrv, NULL }
};

/*
 * Attributes for the per-slot bounce buffers: small, and we ask for a single
 * cookie so that a bounce buffer is always one descriptor.
 */
static const ddi_dma_attr_t vtfs_bounce_dma_attr = {
	.dma_attr_version =		DMA_ATTR_V0,
	.dma_attr_addr_lo =		0x0000000000000000,
	.dma_attr_addr_hi =		0xFFFFFFFFFFFFFFFF,
	.dma_attr_count_max =		0x00000000FFFFFFFF,
	.dma_attr_align =		1,
	.dma_attr_burstsizes =		1,
	.dma_attr_minxfer =		1,
	.dma_attr_maxxfer =		0x00000000FFFFFFFF,
	.dma_attr_seg =			0x00000000FFFFFFFF,
	.dma_attr_sgllen =		1,
	.dma_attr_granular =		1,
	.dma_attr_flags =		0
};

/*
 * Attributes for binding a caller's bulk buffer.  Such a buffer is a kernel
 * mapping of an arbitrary set of pages, so it may need as many cookies as it
 * has pages.
 */
static const ddi_dma_attr_t vtfs_data_dma_attr = {
	.dma_attr_version =		DMA_ATTR_V0,
	.dma_attr_addr_lo =		0x0000000000000000,
	.dma_attr_addr_hi =		0xFFFFFFFFFFFFFFFF,
	.dma_attr_count_max =		0x00000000FFFFFFFF,
	.dma_attr_align =		1,
	.dma_attr_burstsizes =		1,
	.dma_attr_minxfer =		1,
	.dma_attr_maxxfer =		0x00000000FFFFFFFF,
	.dma_attr_seg =			0x00000000FFFFFFFF,
	.dma_attr_sgllen =		VTFS_SEG_MAX,
	.dma_attr_granular =		1,
	.dma_attr_flags =		0
};

/*
 * The exported interface.
 */

vtfs_t *
vtfs_hold_by_tag(const char *tag)
{
	vtfs_t *vtf;

	mutex_enter(&vtfs_devices_lock);
	for (vtf = list_head(&vtfs_devices); vtf != NULL;
	    vtf = list_next(&vtfs_devices, vtf)) {
		if (vtf->vtf_detaching)
			continue;
		if (strcmp(vtf->vtf_tag, tag) == 0) {
			vtf->vtf_refcnt++;
			break;
		}
	}
	mutex_exit(&vtfs_devices_lock);

	return (vtf);
}

void
vtfs_rele(vtfs_t *vtf)
{
	mutex_enter(&vtfs_devices_lock);
	VERIFY3U(vtf->vtf_refcnt, >, 0);
	vtf->vtf_refcnt--;
	mutex_exit(&vtfs_devices_lock);
}

const char *
vtfs_tag(vtfs_t *vtf)
{
	return (vtf->vtf_tag);
}

static vtfs_slot_t *
vtfs_slot_get(vtfs_t *vtf)
{
	vtfs_slot_t *vs;

	VERIFY(MUTEX_HELD(&vtf->vtf_mutex));

	while ((vs = list_remove_head(&vtf->vtf_free)) == NULL) {
		if (vtf->vtf_detaching)
			return (NULL);
		cv_wait(&vtf->vtf_cv, &vtf->vtf_mutex);
	}

	VERIFY3S(vs->vs_state, ==, VTFS_SLOT_FREE);
	vs->vs_state = VTFS_SLOT_INFLIGHT;
	vs->vs_resid = 0;

	return (vs);
}

static void
vtfs_slot_put(vtfs_t *vtf, vtfs_slot_t *vs)
{
	VERIFY(MUTEX_HELD(&vtf->vtf_mutex));

	if (vs->vs_data_bound) {
		virtio_dma_unbind(vs->vs_data);
		vs->vs_data_bound = B_FALSE;
	}
	virtio_chain_clear(vs->vs_chain);
	vs->vs_state = VTFS_SLOT_FREE;
	vs->vs_resid = 0;

	list_insert_head(&vtf->vtf_free, vs);
	cv_broadcast(&vtf->vtf_cv);
}

int
vtfs_xfer(vtfs_t *vtf, vtfs_xfer_t *vx)
{
	vtfs_slot_t *vs;
	virtio_chain_t *vic;
	uint64_t bpa;
	int r = 0;

	if (vx->vx_outlen > VTFS_BOUNCE_SIZE ||
	    vx->vx_inlen > VTFS_BOUNCE_SIZE) {
		return (EINVAL);
	}
	if (vx->vx_datalen > VTFS_DATA_MAX) {
		return (EINVAL);
	}

	mutex_enter(&vtf->vtf_mutex);
	if ((vs = vtfs_slot_get(vtf)) == NULL) {
		mutex_exit(&vtf->vtf_mutex);
		return (ENXIO);
	}
	mutex_exit(&vtf->vtf_mutex);

	vic = vs->vs_chain;
	bpa = virtio_dma_cookie_pa(vs->vs_bounce, 0);

	bcopy(vx->vx_out, virtio_dma_va(vs->vs_bounce, 0), vx->vx_outlen);
	bzero((caddr_t)virtio_dma_va(vs->vs_bounce, VTFS_BOUNCE_SIZE),
	    VTFS_BOUNCE_SIZE);

	/*
	 * The device-readable part first, as the specification requires.
	 */
	if (virtio_chain_append(vic, bpa, vx->vx_outlen,
	    VIRTIO_DIR_DEVICE_READS) != DDI_SUCCESS) {
		r = ENOMEM;
		goto out;
	}

	/*
	 * A zero length inbound buffer means the request has no reply at all,
	 * as is the case for FUSE_FORGET.  Such a chain is entirely
	 * device-readable, which is legal, and the device still returns it to
	 * us so that we can reclaim the slot.
	 */
	if (vx->vx_inlen > 0 &&
	    virtio_chain_append(vic, bpa + VTFS_BOUNCE_SIZE, vx->vx_inlen,
	    VIRTIO_DIR_DEVICE_WRITES) != DDI_SUCCESS) {
		r = ENOMEM;
		goto out;
	}

	if (vx->vx_data != NULL && vx->vx_datalen > 0) {
		if (virtio_dma_bind(vs->vs_data, vx->vx_data, vx->vx_datalen,
		    DDI_DMA_READ | DDI_DMA_STREAMING, KM_SLEEP) !=
		    DDI_SUCCESS) {
			r = ENOMEM;
			goto out;
		}
		vs->vs_data_bound = B_TRUE;

		for (uint_t n = 0; n < virtio_dma_ncookies(vs->vs_data); n++) {
			if (virtio_chain_append(vic,
			    virtio_dma_cookie_pa(vs->vs_data, n),
			    virtio_dma_cookie_size(vs->vs_data, n),
			    VIRTIO_DIR_DEVICE_WRITES) != DDI_SUCCESS) {
				r = ENOMEM;
				goto out;
			}
		}
	}

	virtio_dma_sync(vs->vs_bounce, DDI_DMA_SYNC_FORDEV);
	if (vs->vs_data_bound)
		virtio_dma_sync(vs->vs_data, DDI_DMA_SYNC_FORDEV);

	mutex_enter(&vtf->vtf_mutex);
	virtio_chain_submit(vic, B_TRUE);

	while (vs->vs_state != VTFS_SLOT_DONE && !vtf->vtf_detaching)
		cv_wait(&vtf->vtf_cv, &vtf->vtf_mutex);

	if (vs->vs_state != VTFS_SLOT_DONE) {
		/*
		 * The device is going away underneath us.  virtio_fini() will
		 * reset it and reclaim the descriptors; there is nothing to
		 * copy back.
		 */
		vtfs_slot_put(vtf, vs);
		mutex_exit(&vtf->vtf_mutex);
		return (ENXIO);
	}
	mutex_exit(&vtf->vtf_mutex);

	virtio_dma_sync(vs->vs_bounce, DDI_DMA_SYNC_FORCPU);
	if (vs->vs_data_bound)
		virtio_dma_sync(vs->vs_data, DDI_DMA_SYNC_FORCPU);

	/*
	 * The device reports the total number of bytes it wrote across the
	 * whole device-writable part of the chain.  Some devices are known to
	 * be careless with this value; the file system code treats a count
	 * that is short of the fixed sized reply as a protocol error and
	 * otherwise only uses it to size the variable length tail.
	 */
	vs->vs_resid = virtio_chain_received_length(vic);

	if (vx->vx_inlen > 0) {
		bcopy((caddr_t)virtio_dma_va(vs->vs_bounce, VTFS_BOUNCE_SIZE),
		    vx->vx_in, vx->vx_inlen);
	}
	vx->vx_resid = vs->vs_resid;

out:
	mutex_enter(&vtf->vtf_mutex);
	vtfs_slot_put(vtf, vs);
	mutex_exit(&vtf->vtf_mutex);

	return (r);
}

/*
 * Device operation.
 */

static uint_t
vtfs_poll(vtfs_t *vtf)
{
	virtio_chain_t *vic;
	uint_t count = 0;

	VERIFY(MUTEX_HELD(&vtf->vtf_mutex));

	while ((vic = virtio_queue_poll(vtf->vtf_vq)) != NULL) {
		vtfs_slot_t *vs = virtio_chain_data(vic);

		VERIFY3S(vs->vs_state, ==, VTFS_SLOT_INFLIGHT);
		vs->vs_state = VTFS_SLOT_DONE;
		count++;
	}

	if (count > 0)
		cv_broadcast(&vtf->vtf_cv);

	return (count);
}

static uint_t
vtfs_int_handler(caddr_t arg0, caddr_t arg1 __unused)
{
	vtfs_t *vtf = (vtfs_t *)arg0;

	mutex_enter(&vtf->vtf_mutex);
	(void) vtfs_poll(vtf);
	mutex_exit(&vtf->vtf_mutex);

	return (DDI_INTR_CLAIMED);
}

static void
vtfs_free_slots(vtfs_t *vtf)
{
	if (vtf->vtf_slots == NULL)
		return;

	for (uint_t i = 0; i < vtf->vtf_nslots; i++) {
		vtfs_slot_t *vs = &vtf->vtf_slots[i];

		if (list_link_active(&vs->vs_link))
			list_remove(&vtf->vtf_free, vs);

		if (vs->vs_data_bound) {
			virtio_dma_unbind(vs->vs_data);
			vs->vs_data_bound = B_FALSE;
		}
		if (vs->vs_data != NULL) {
			virtio_dma_free(vs->vs_data);
			vs->vs_data = NULL;
		}
		if (vs->vs_chain != NULL) {
			virtio_chain_free(vs->vs_chain);
			vs->vs_chain = NULL;
		}
		if (vs->vs_bounce != NULL) {
			virtio_dma_free(vs->vs_bounce);
			vs->vs_bounce = NULL;
		}
	}

	kmem_free(vtf->vtf_slots, sizeof (vtfs_slot_t) * vtf->vtf_nslots);
	vtf->vtf_slots = NULL;
	vtf->vtf_nslots = 0;
}

static int
vtfs_alloc_slots(vtfs_t *vtf)
{
	vtf->vtf_nslots = MIN(virtio_queue_size(vtf->vtf_vq), VTFS_MAX_SLOTS);
	vtf->vtf_slots = kmem_zalloc(
	    sizeof (vtfs_slot_t) * vtf->vtf_nslots, KM_SLEEP);

	for (uint_t i = 0; i < vtf->vtf_nslots; i++) {
		vtfs_slot_t *vs = &vtf->vtf_slots[i];

		vs->vs_vtfs = vtf;
		vs->vs_state = VTFS_SLOT_FREE;

		vs->vs_bounce = virtio_dma_alloc(vtf->vtf_virtio,
		    2 * VTFS_BOUNCE_SIZE, &vtfs_bounce_dma_attr,
		    DDI_DMA_RDWR | DDI_DMA_CONSISTENT, KM_SLEEP);
		if (vs->vs_bounce == NULL)
			goto fail;

		vs->vs_data = virtio_dma_alloc_nomem(vtf->vtf_virtio,
		    &vtfs_data_dma_attr, KM_SLEEP);
		if (vs->vs_data == NULL)
			goto fail;

		vs->vs_chain = virtio_chain_alloc(vtf->vtf_vq, KM_SLEEP);
		if (vs->vs_chain == NULL)
			goto fail;
		virtio_chain_data_set(vs->vs_chain, vs);

		list_insert_tail(&vtf->vtf_free, vs);
	}

	return (0);

fail:
	vtfs_free_slots(vtf);
	return (ENOMEM);
}

static void
vtfs_read_tag(vtfs_t *vtf)
{
	uint_t i;

	/*
	 * The tag is a fixed size field padded with NUL bytes; it is only NUL
	 * terminated if it is shorter than the field.
	 */
	for (i = 0; i < VIRTIO_FS_TAG_LEN; i++) {
		vtf->vtf_tag[i] = (char)virtio_dev_get8(vtf->vtf_virtio,
		    VIRTIO_FS_CONFIG_TAG + i);
	}
	vtf->vtf_tag[VIRTIO_FS_TAG_LEN] = '\0';
}

static int
vtfs_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	vtfs_t *vtf;
	virtio_t *vio;
	boolean_t did_mutex = B_FALSE;

	if (cmd != DDI_ATTACH)
		return (DDI_FAILURE);

	if ((vio = virtio_init(dip)) == NULL) {
		dev_err(dip, CE_WARN, "failed to start Virtio init");
		return (DDI_FAILURE);
	}
	if (!virtio_init_features(vio, VTFS_WANTED_FEATURES, B_TRUE)) {
		virtio_fini(vio, B_TRUE);
		return (DDI_FAILURE);
	}

	vtf = kmem_zalloc(sizeof (*vtf), KM_SLEEP);
	vtf->vtf_dip = dip;
	vtf->vtf_virtio = vio;
	ddi_set_driver_private(dip, vtf);
	list_create(&vtf->vtf_free, sizeof (vtfs_slot_t),
	    offsetof(vtfs_slot_t, vs_link));

	vtfs_read_tag(vtf);

	vtf->vtf_nrequestq = virtio_dev_get32(vio,
	    VIRTIO_FS_CONFIG_NUM_REQ_VQ);
	if (vtf->vtf_nrequestq == 0 ||
	    vtf->vtf_nrequestq == PCI_EINVAL32) {
		dev_err(dip, CE_WARN, "device advertises no request queues");
		goto fail;
	}

	/*
	 * Only the first request queue.  A read-only client gains nothing from
	 * multiqueue that it does not gain more cheaply from having several
	 * requests in flight on one queue.
	 */
	if ((vtf->vtf_vq = virtio_queue_alloc(vio, VIRTIO_FS_VIRTQ_REQUEST,
	    "request", vtfs_int_handler, vtf, B_FALSE, VTFS_SEG_MAX)) == NULL) {
		dev_err(dip, CE_WARN, "failed to allocate request queue");
		goto fail;
	}

	if (virtio_init_complete(vio, VIRTIO_ANY_INTR_TYPE) != DDI_SUCCESS) {
		dev_err(dip, CE_WARN, "failed to complete Virtio init");
		goto fail;
	}

	mutex_init(&vtf->vtf_mutex, NULL, MUTEX_DRIVER, virtio_intr_pri(vio));
	cv_init(&vtf->vtf_cv, NULL, CV_DRIVER, NULL);
	did_mutex = B_TRUE;

	if (vtfs_alloc_slots(vtf) != 0) {
		dev_err(dip, CE_WARN, "failed to allocate request slots");
		goto fail;
	}

	if (virtio_interrupts_enable(vio) != DDI_SUCCESS) {
		dev_err(dip, CE_WARN, "failed to enable interrupts");
		goto fail;
	}

	mutex_enter(&vtfs_devices_lock);
	list_insert_tail(&vtfs_devices, vtf);
	mutex_exit(&vtfs_devices_lock);

	dev_err(dip, CE_CONT, "?Virtio FS tag \"%s\", %u request queue%s\n",
	    vtf->vtf_tag, vtf->vtf_nrequestq,
	    vtf->vtf_nrequestq == 1 ? "" : "s");

	return (DDI_SUCCESS);

fail:
	vtfs_free_slots(vtf);
	if (vio != NULL)
		virtio_fini(vio, B_TRUE);
	if (did_mutex) {
		mutex_destroy(&vtf->vtf_mutex);
		cv_destroy(&vtf->vtf_cv);
	}
	list_destroy(&vtf->vtf_free);
	kmem_free(vtf, sizeof (*vtf));
	return (DDI_FAILURE);
}

static int
vtfs_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	vtfs_t *vtf = ddi_get_driver_private(dip);

	if (cmd != DDI_DETACH)
		return (DDI_FAILURE);

	mutex_enter(&vtfs_devices_lock);
	if (vtf->vtf_refcnt > 0) {
		mutex_exit(&vtfs_devices_lock);
		return (DDI_FAILURE);
	}
	list_remove(&vtfs_devices, vtf);
	mutex_exit(&vtfs_devices_lock);

	mutex_enter(&vtf->vtf_mutex);
	vtf->vtf_detaching = B_TRUE;
	cv_broadcast(&vtf->vtf_cv);
	mutex_exit(&vtf->vtf_mutex);

	/*
	 * Tear down the Virtio framework first, so that no interrupt handler
	 * is running by the time we free the slots it would touch.
	 */
	virtio_fini(vtf->vtf_virtio, B_FALSE);

	mutex_enter(&vtf->vtf_mutex);
	vtfs_free_slots(vtf);
	mutex_exit(&vtf->vtf_mutex);

	mutex_destroy(&vtf->vtf_mutex);
	cv_destroy(&vtf->vtf_cv);
	list_destroy(&vtf->vtf_free);
	kmem_free(vtf, sizeof (*vtf));

	return (DDI_SUCCESS);
}

static int
vtfs_quiesce(dev_info_t *dip)
{
	vtfs_t *vtf;

	if ((vtf = ddi_get_driver_private(dip)) == NULL)
		return (DDI_FAILURE);

	return (virtio_quiesce(vtf->vtf_virtio));
}

int
_init(void)
{
	int rv;


	mutex_init(&vtfs_devices_lock, NULL, MUTEX_DRIVER, NULL);
	list_create(&vtfs_devices, sizeof (vtfs_t),
	    offsetof(vtfs_t, vtf_link));

	if ((rv = mod_install(&vtfs_modlinkage)) != 0) {
		list_destroy(&vtfs_devices);
		mutex_destroy(&vtfs_devices_lock);
	}

	return (rv);
}

int
_fini(void)
{
	int rv;

	if ((rv = mod_remove(&vtfs_modlinkage)) == 0) {
		list_destroy(&vtfs_devices);
		mutex_destroy(&vtfs_devices_lock);
	}

	return (rv);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&vtfs_modlinkage, modinfop));
}
