#include "asm/io.h"
#include "asm/stat.h"
#include "linux/mmzone.h"
#include "linux/virtio_types.h"
#include "llfree.h"
#include <linux/virtio.h>
#include <linux/virtio_balloon.h>
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

enum virtio_llfree_balloon_vq {
	VIRTIO_LLFREE_BALLOON_VQ_GUEST_INFO,
	VIRTIO_LLFREE_BALLOON_VQ_MAX,
};

struct llfree_vq_buffer {
	void *buf;
	size_t len;
};

struct virtio_llfree_balloon {
	struct virtio_device *vdev;
	struct virtqueue *guest_info_vq; 
	llfree_info_t qemu_info;
	struct llfree_vq_buffer vq_buffer;
};

static void noinline virtio_llfree_send_llfree_info(struct virtio_llfree_balloon *vb) {
	struct scatterlist sg;
	struct zone *zone;

	for_each_populated_zone(zone) {
		vb->qemu_info.zone_normal_free_pages = (_Atomic(int64_t) *) &zone->vm_stat[NR_FREE_PAGES];
		vb->qemu_info.qemu_llfree = (llfree_t *) zone->llfree;
		llfree_copy_into_buffer(&vb->qemu_info, vb->vq_buffer.buf);

		sg_init_one(&sg, vb->vq_buffer.buf, vb->vq_buffer.len);
		virtqueue_add_outbuf(vb->guest_info_vq, &sg, 1, vb, GFP_KERNEL);
		virtqueue_kick(vb->guest_info_vq);
	}
}

static void virtio_llfree_config_changed(struct virtio_device *vdev) {
	struct virtio_llfree_balloon *vb;
	vb = (struct virtio_llfree_balloon *) vdev->priv;
	llfree_inspect_llfree(vb->qemu_info.qemu_llfree);
}

// static void noinline virtio_llfree_send_test_llfree_info(struct virtio_llfree_balloon *vb) {
// 	struct scatterlist sg;
// 	struct pglist_data *pgdat = first_online_pgdat();
// 	struct zone *zone_normal = &pgdat->node_zones[ZONE_NORMAL];
//
// 	vb->qemu_info.zone_normal_free_pages = (_Atomic(int64_t) *) &zone_normal->vm_stat[NR_FREE_PAGES];
// 	// vb->qemu_info.qemu_llfree = (llfree_t *) zone_normal->llfree;
// 	vb->qemu_info.qemu_llfree = llfree_create_test_llfree();
// 	llfree_copy_into_buffer(&vb->qemu_info, vb->vq_buffer.buf);
//
// 	sg_init_one(&sg, vb->vq_buffer.buf, vb->vq_buffer.len);
// 	virtqueue_add_outbuf(vb->guest_info_vq, &sg, 1, vb, GFP_KERNEL);
// 	virtqueue_kick(vb->guest_info_vq);
// }

// IDs can't be arbitrarily chosen, for now
// say that we are virtio-balloon
static const struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_BALLOON, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static void noinline virtio_llfree_callback(struct virtqueue *vq) {
	
}

static int init_vqs(struct virtio_llfree_balloon *vb)
{
	struct virtqueue *vqs[VIRTIO_LLFREE_BALLOON_VQ_MAX];
	vq_callback_t *callbacks[VIRTIO_LLFREE_BALLOON_VQ_MAX];
	const char *names[VIRTIO_LLFREE_BALLOON_VQ_MAX];
	int err;

	callbacks[VIRTIO_LLFREE_BALLOON_VQ_GUEST_INFO] = virtio_llfree_callback;
	names[VIRTIO_LLFREE_BALLOON_VQ_GUEST_INFO] = "guest info";
	err = virtio_find_vqs(vb->vdev, VIRTIO_LLFREE_BALLOON_VQ_MAX, vqs,
			      callbacks, names, NULL);
	if (err)
		return err;
	vb->guest_info_vq = vqs[VIRTIO_LLFREE_BALLOON_VQ_GUEST_INFO];
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
	
	llfree_create_buffer(&vb->vq_buffer.buf, &vb->vq_buffer.len);
	if(!vb->vq_buffer.buf) {
		err = -ENOMEM;
		goto out;
	}

	vb->vdev = vdev;
	err = init_vqs(vb);
	if (err)
		goto out_free_vb;

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
