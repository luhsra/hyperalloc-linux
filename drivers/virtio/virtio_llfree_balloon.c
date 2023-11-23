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

enum virtio_balloon_vq {
	VIRTIO_LLFREE_BALLOON_VQ_GUEST_INFO,
	VIRTIO_LLFREE_BALLOON_VQ_MAX,
};

struct llfree_guest_info {
	void *tree_gpa;
	void *children_gpa;
	void *zone_normal_free_pages;
	u64 tree_len;
	u64 children_len;
	u64 test_data;
};

struct virtio_llfree_balloon {
	struct virtio_device *vdev;
	struct virtqueue *guest_info_vq; 
	__virtio32 test_data[32];
	struct llfree_guest_info guest_info;
	uint64_t *virt_test_data;
};

u64 *virt_test_data;

static void noinline virtio_llfree_send_guest_info(struct virtio_llfree_balloon *vb) 
{
	// llfree_get_guest_info(zone_normal->llfree, (void*) &guest_info);
	struct scatterlist sg;
	struct pglist_data *pgdat = first_online_pgdat();
	struct zone *zone_normal = &pgdat->node_zones[ZONE_NORMAL];

	vb->guest_info.children_gpa = (void*) 0x00;
	vb->guest_info.children_len = 10;
	vb->guest_info.tree_gpa = (void*) 0x7;
	vb->guest_info.tree_len = 15;
	vb->guest_info.zone_normal_free_pages = (void*) &zone_normal->vm_stat[NR_FREE_PAGES];
	
	virt_test_data = kzalloc(sizeof(uint64_t), GFP_KERNEL);
	vb->guest_info.test_data = virt_to_phys(virt_test_data);
	sg_init_one(&sg, &vb->guest_info, sizeof(struct llfree_guest_info));
	*virt_test_data = 5;
	/* We should always be able to add one buffer to an empty queue. */
	virtqueue_add_outbuf(vb->guest_info_vq, &sg, 1, vb, GFP_KERNEL);
	virtqueue_kick(vb->guest_info_vq);
}

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

	vb->vdev = vdev;

	err = init_vqs(vb);
	if (err)
		goto out_free_vb;

	virtio_device_ready(vdev);

	virtio_llfree_send_guest_info(vb);
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
};

module_virtio_driver(virtio_llfree_balloon_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio llfree balloon driver");
MODULE_LICENSE("GPL");
