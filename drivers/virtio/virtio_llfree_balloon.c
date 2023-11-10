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

struct virtio_llfree_balloon {
	struct virtio_device *vdev;
	struct virtqueue *guest_info_vq; 

	/* Waiting for host to ack the pages we released. */
	wait_queue_head_t acked;


	/* Synchronize access/update to this struct virtio_balloon elements */
	struct mutex balloon_lock;
};

static const struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_LLFREE_BALLOON, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static int init_vqs(struct virtio_llfree_balloon *vb)
{
	struct virtqueue *vqs[VIRTIO_LLFREE_BALLOON_VQ_MAX];
	vq_callback_t *callbacks[VIRTIO_LLFREE_BALLOON_VQ_MAX];
	const char *names[VIRTIO_LLFREE_BALLOON_VQ_MAX];
	int err;

	callbacks[VIRTIO_LLFREE_BALLOON_VQ_GUEST_INFO] = NULL;
	names[VIRTIO_LLFREE_BALLOON_VQ_GUEST_INFO] = "guest info";

	err = virtio_find_vqs(vb->vdev, VIRTIO_LLFREE_BALLOON_VQ_MAX, vqs,
			      callbacks, names, NULL);
	if (err)
		return err;

	vb->guest_info_vq = vqs[VIRTIO_LLFREE_BALLOON_VQ_GUEST_INFO];

	return 0;
}

static int virtballoon_probe(struct virtio_device *vdev)
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

	mutex_init(&vb->balloon_lock);
	init_waitqueue_head(&vb->acked);
	vb->vdev = vdev;

	err = init_vqs(vb);
	if (err)
		goto out_free_vb;

	virtio_device_ready(vdev);

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

static void virtballoon_remove(struct virtio_device *vdev)
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
	.probe =	virtballoon_probe,
	.remove =	virtballoon_remove,
};

module_virtio_driver(virtio_llfree_balloon_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio llfree balloon driver");
MODULE_LICENSE("GPL");
