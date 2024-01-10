#include "asm/io.h"
#include "asm/stat.h"
#include "linux/mmzone.h"
#include "linux/spinlock.h"
#include "linux/types.h"
#include "linux/virtio_config.h"
#include "linux/virtio_types.h"
#include "llfree.h"
#include <linux/virtio.h>
#include <linux/virtio_llfree_balloon.h>
#include <linux/swap.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/balloon_compaction.h>
#include <linux/oom.h>
#include <linux/wait.h>
#include <linux/mm.h>
#include <linux/page_reporting.h>

#include "llfree_alloc.h"
#include "llfree_qemu.h"
#include "llfree_qemu_test.h"

extern uint32_t shrink_pagecache_for_reclaim(uint32_t nr_to_reclaim);

enum virtio_llfree_balloon_vq {
	VIRTIO_LLFREE_BALLOON_LLFREE_INFO_VQ,
	VIRTIO_LLFREE_BALLOON_LLFREE_REQUEST_VQ,
	VIRTIO_LLFREE_BALLOON_VQ_MAX,
};

struct llfree_vq_buffer {
	void *buf;
	size_t len;
};

enum RequestType {
    SCHEDULE_LLFREE_BALLOON_UPDATE,
    AUTO_DEFLATE,
};

typedef enum RequestType RequestType;

struct LLFreeBalloonRequest {
    RequestType req;
};

typedef struct LLFreeBalloonRequest LLFreeBalloonRequest;

enum llfree_zone_type {
	LLFREE_NON_EXISTING,
	LLFREE_ZONE_DMA,
	LLFREE_ZONE_DMA32,
	LLFREE_ZONE_NORMAL,
	LLFREE_ZONE_HIGHMEM,
	LLFREE_ZONE_MOVABLE,
	LLFREE_ZONE_DEVICE,
	LLFREE_MAX_NR_ZONES
};

struct virtio_llfree_balloon {
	struct virtio_device *vdev;
	struct virtqueue *llfree_info_vq; 
	struct virtqueue *llfree_request_vq;
	struct work_struct shrink_pagecache_work;
	spinlock_t lock_auto_deflate;
	LLFreeBalloonRequest guest_req;
	llfree_info_t qemu_info;
	struct llfree_vq_buffer vq_buffer;
	wait_queue_head_t acked;
	enum llfree_zone_type map_zone_type[LLFREE_MAX_NR_ZONES];
};

struct virtio_llfree_balloon *vb_llfree;

static void noinline virtio_llfree_init_map_zone_types(enum llfree_zone_type *map_zone_type) {
	for(uint32_t i = 0; i < LLFREE_MAX_NR_ZONES; i++) {
		map_zone_type[i] = LLFREE_NON_EXISTING;
	}

	#ifdef CONFIG_ZONE_DMA
	map_zone_type[ZONE_DMA] = LLFREE_ZONE_DMA;
	#endif

	#ifdef CONFIG_ZONE_DMA32
	map_zone_type[ZONE_DMA32] = LLFREE_ZONE_DMA32;
	#endif	

	map_zone_type[ZONE_NORMAL] = LLFREE_ZONE_NORMAL;
	
	#ifdef CONFIG_HIGHMEM
	map_zone_type[ZONE_HIGHMEM] = LLFREE_ZONE_HIGHMEM;
	#endif

	map_zone_type[ZONE_MOVABLE] = LLFREE_ZONE_MOVABLE;

	#ifdef CONFIG_ZONE_DEVICE
	map_zone_type[ZONE_DEVICE] = LLFREE_ZONE_DEVICE;
	#endif
}

static void noinline virtio_llfree_send_llfree_info(struct virtio_llfree_balloon *vb) {
	struct scatterlist sg;
	struct zone *zone;

	// struct pglist_data *pgdat = first_online_pgdat();
	// zone = &pgdat->node_zones[ZONE_NORMAL];

	for_each_populated_zone(zone) {
		vb->qemu_info.zone_normal_free_pages = (_Atomic(int64_t) *) &zone->vm_stat[NR_FREE_PAGES];
		vb->qemu_info.qemu_llfree = (llfree_t *) zone->llfree;
		vb->qemu_info.num_pagecache_reclaimable_pages = (_Atomic(int64_t) *) &zone->zone_pgdat->vm_stat[NR_FILE_PAGES];
		llfree_copy_into_buffer(&vb->qemu_info, vb->vq_buffer.buf);

		sg_init_one(&sg, vb->vq_buffer.buf, vb->vq_buffer.len);
		virtqueue_add_outbuf(vb->llfree_info_vq, &sg, 1, vb, GFP_KERNEL);
		virtqueue_kick(vb->llfree_info_vq);
	}
}

static void noinline virtio_llfree_send_request(struct virtio_llfree_balloon *vb, RequestType llfree_request) {
		struct scatterlist sg;
		uint32_t len;

		vb->guest_req.req = llfree_request;
		sg_init_one(&sg, &vb->guest_req, sizeof(LLFreeBalloonRequest));
		virtqueue_add_outbuf(vb->llfree_request_vq, &sg, 1, vb, GFP_KERNEL);
		virtqueue_kick(vb->llfree_request_vq);
		wait_event(vb->acked, virtqueue_get_buf(vb->llfree_request_vq, &len));
}

void noinline virtio_llfree_auto_deflate(void) {
	//spinloc
	spin_lock(&vb_llfree->lock_auto_deflate);
	virtio_llfree_send_request(vb_llfree, AUTO_DEFLATE);
	spin_unlock(&vb_llfree->lock_auto_deflate);
}
EXPORT_SYMBOL(virtio_llfree_auto_deflate);

static void shrink_pagecache_func(struct work_struct *work) {
		struct virtio_llfree_balloon *vb;
		uint32_t reclaimed_nr_pages, shrink_pagecache_num_pages;

		vb = container_of(work, struct virtio_llfree_balloon, shrink_pagecache_work);

		virtio_cread_le(vb->vdev, struct virtio_llfree_balloon_config, 
		                shrink_pagecache_num_pages, &shrink_pagecache_num_pages);

		reclaimed_nr_pages = shrink_pagecache_for_reclaim(shrink_pagecache_num_pages);

		shrink_pagecache_num_pages = 0;
		virtio_cwrite_le(vb->vdev, struct virtio_llfree_balloon_config, 
		                 shrink_pagecache_num_pages, &shrink_pagecache_num_pages);

		virtio_llfree_send_request(vb, SCHEDULE_LLFREE_BALLOON_UPDATE);
}

static void virtio_llfree_config_changed(struct virtio_device *vdev) {
		struct virtio_llfree_balloon *vb;
		vb = (struct virtio_llfree_balloon *) vdev->priv;

		// dispatch
		uint32_t shrink_pagecache_num_pages;
		virtio_cread_le(vdev, struct virtio_llfree_balloon_config, 
		                shrink_pagecache_num_pages, &shrink_pagecache_num_pages);

		if(shrink_pagecache_num_pages > 0) {
				queue_work(system_freezable_wq, &vb->shrink_pagecache_work);
		}
}

// IDs can't be arbitrarily chosen, for now
// say that we are virtio-balloon
static const struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_BALLOON, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static void noinline virtio_llfree_callback(struct virtqueue *vq) {
	
}

static void balloon_ack(struct virtqueue *vq)
{
	struct virtio_llfree_balloon *vb = vq->vdev->priv;

	wake_up(&vb->acked);
}

static int init_vqs(struct virtio_llfree_balloon *vb)
{
	struct virtqueue *vqs[VIRTIO_LLFREE_BALLOON_VQ_MAX];
	vq_callback_t *callbacks[VIRTIO_LLFREE_BALLOON_VQ_MAX];
	const char *names[VIRTIO_LLFREE_BALLOON_VQ_MAX];
	int err;

	callbacks[VIRTIO_LLFREE_BALLOON_LLFREE_INFO_VQ] = virtio_llfree_callback;
	names[VIRTIO_LLFREE_BALLOON_LLFREE_INFO_VQ] = "llfree info";
	callbacks[VIRTIO_LLFREE_BALLOON_LLFREE_REQUEST_VQ] = balloon_ack;
	names[VIRTIO_LLFREE_BALLOON_LLFREE_REQUEST_VQ] = "llfree requests";
	err = virtio_find_vqs(vb->vdev, VIRTIO_LLFREE_BALLOON_VQ_MAX, vqs,
			      callbacks, names, NULL);
	if (err)
		return err;

	vb->llfree_info_vq = vqs[VIRTIO_LLFREE_BALLOON_LLFREE_INFO_VQ];
	vb->llfree_request_vq = vqs[VIRTIO_LLFREE_BALLOON_LLFREE_REQUEST_VQ];
	return 0;
}


static int virtio_llfree_balloon_probe(struct virtio_device *vdev)
{
	struct virtio_llfree_balloon *vb;
	int err;
	if (!vdev->config->get) {
		dev_err(&vdev->dev, "%s failure: config access disabled\n",
			__func__);
		return -EINVAL;
	}
	vdev->priv = vb = kzalloc(sizeof(*vb), GFP_KERNEL);
	if (!vb) {
		err = -ENOMEM;
		goto out;
	}

	INIT_WORK(&vb->shrink_pagecache_work, shrink_pagecache_func);
	virtio_llfree_init_map_zone_types((enum llfree_zone_type *) &vb->map_zone_type);

	init_waitqueue_head(&vb->acked);
	
	llfree_create_buffer(&vb->vq_buffer.buf, &vb->vq_buffer.len);
	if(!vb->vq_buffer.buf) {
		err = -ENOMEM;
		goto out;
	}

	vb->vdev = vdev;
	err = init_vqs(vb);
	if (err)
		goto out_free_vb;

	vb_llfree = vb;
	spin_lock_init(&vb->lock_auto_deflate);
	
	virtio_device_ready(vdev);
	virtio_llfree_send_llfree_info(vb);
	return 0;

out_free_vb:
	kfree(vb);
out:
	return err;
}

static void remove_common(struct virtio_llfree_balloon *vb)
{
	/* Now we reset the device so we can clean up the queues. */
	virtio_reset_device(vb->vdev);
	vb->vdev->config->del_vqs(vb->vdev);
}

static void virtio_llfree_remove(struct virtio_device *vdev)
{
	struct virtio_llfree_balloon *vb = vdev->priv;
	kfree(vb->vq_buffer.buf);
	remove_common(vb);
	kfree(vb);
}

static unsigned int features[] = {
};

static struct virtio_driver virtio_llfree_balloon_driver = {
	.feature_table = features,
	.feature_table_size = ARRAY_SIZE(features),
	.driver.name =	KBUILD_MODNAME,
	.driver.owner =	THIS_MODULE,
	.id_table =	id_table,
	.probe =	virtio_llfree_balloon_probe,
	.remove =	virtio_llfree_remove,
	.config_changed = virtio_llfree_config_changed
};

module_virtio_driver(virtio_llfree_balloon_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio llfree balloon driver");
MODULE_LICENSE("GPL");
