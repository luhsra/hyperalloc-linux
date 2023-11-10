// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Virtio balloon implementation, inspired by Dor Laor and Marcelo
 * Tosatti's implementations.
 *
 *  Copyright 2008 Rusty Russell IBM Corporation
 */

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

/*
 * Balloon device works in 4K page units.  So each page is pointed to by
 * multiple balloon pages.  All memory counters in this driver are in balloon
 * page units.
 */
#define VIRTIO_BALLOON_PAGES_PER_PAGE (unsigned int)(PAGE_SIZE >> VIRTIO_BALLOON_PFN_SHIFT)
#define VIRTIO_BALLOON_ARRAY_PFNS_MAX 256

#define VIRTIO_BALLOON_FREE_PAGE_ALLOC_FLAG (__GFP_NORETRY | __GFP_NOWARN | \
					     __GFP_NOMEMALLOC)

enum virtio_balloon_vq {
	VIRTIO_BALLOON_VQ_INFLATE,
	VIRTIO_BALLOON_VQ_DEFLATE,
	VIRTIO_BALLOON_VQ_MAX
};

enum virtio_balloon_config_read {
	VIRTIO_BALLOON_CONFIG_READ_CMD_ID = 0,
};

struct virtio_balloon {
	struct virtio_device *vdev;
	struct virtqueue *inflate_vq, *deflate_vq; 

	/* Balloon's own wq for cpu-intensive work items */
	struct workqueue_struct *balloon_wq;

	/* The balloon servicing is delegated to a freezable workqueue. */
	struct work_struct update_balloon_stats_work;
	struct work_struct update_balloon_size_work;

	/* Prevent updating balloon when it is being canceled. */
	spinlock_t stop_update_lock;
	bool stop_update;
	/* Bitmap to indicate if reading the related config fields are needed */
	unsigned long config_read_bitmap;

	/* Waiting for host to ack the pages we released. */
	wait_queue_head_t acked;

	/* Number of balloon pages we've told the Host we're not using. */
	unsigned int num_pages;
	/*
	 * The pages we've told the Host we're not using are enqueued
	 * at vb_dev_info->pages list.
	 * Each page on this list adds VIRTIO_BALLOON_PAGES_PER_PAGE
	 * to num_pages above.
	 */
	struct balloon_dev_info vb_dev_info;

	/* Synchronize access/update to this struct virtio_balloon elements */
	struct mutex balloon_lock;

	/* The array of pfns we tell the Host about. */
	unsigned int num_pfns;
	__virtio32 pfns[VIRTIO_BALLOON_ARRAY_PFNS_MAX];
};

static const struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_BALLOON, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static u32 page_to_balloon_pfn(struct page *page)
{
	unsigned long pfn = page_to_pfn(page);

	BUILD_BUG_ON(PAGE_SHIFT < VIRTIO_BALLOON_PFN_SHIFT);
	/* Convert pfn from Linux page size to balloon page size. */
	return pfn * VIRTIO_BALLOON_PAGES_PER_PAGE;
}

static void balloon_ack(struct virtqueue *vq)
{
	struct virtio_balloon *vb = vq->vdev->priv;

	wake_up(&vb->acked);
}

static void tell_host(struct virtio_balloon *vb, struct virtqueue *vq)
{
	struct scatterlist sg;
	unsigned int len;

	sg_init_one(&sg, vb->pfns, sizeof(vb->pfns[0]) * vb->num_pfns);

	/* We should always be able to add one buffer to an empty queue. */
	virtqueue_add_outbuf(vq, &sg, 1, vb, GFP_KERNEL);
	virtqueue_kick(vq);

	/* When host has read buffer, this completes via balloon_ack */
	wait_event(vb->acked, virtqueue_get_buf(vq, &len));

}


static void set_page_pfns(struct virtio_balloon *vb,
			  __virtio32 pfns[], struct page *page)
{
	unsigned int i;

	BUILD_BUG_ON(VIRTIO_BALLOON_PAGES_PER_PAGE > VIRTIO_BALLOON_ARRAY_PFNS_MAX);

	/*
	 * Set balloon pfns pointing at this page.
	 * Note that the first pfn points at start of the page.
	 */
	for (i = 0; i < VIRTIO_BALLOON_PAGES_PER_PAGE; i++)
		pfns[i] = cpu_to_virtio32(vb->vdev,
					  page_to_balloon_pfn(page) + i);
}

static unsigned int fill_balloon(struct virtio_balloon *vb, size_t num)
{
	unsigned int num_allocated_pages;
	unsigned int num_pfns;
	struct page *page;
	LIST_HEAD(pages);

	/* We can only do one array worth at a time. */
	num = min(num, ARRAY_SIZE(vb->pfns));

	for (num_pfns = 0; num_pfns < num;
	     num_pfns += VIRTIO_BALLOON_PAGES_PER_PAGE) {
		struct page *page = balloon_page_alloc();

		if (!page) {
			dev_info_ratelimited(&vb->vdev->dev,
					     "Out of puff! Can't get %u pages\n",
					     VIRTIO_BALLOON_PAGES_PER_PAGE);
			/* Sleep for at least 1/5 of a second before retry. */
			msleep(200);
			break;
		}

		balloon_page_push(&pages, page);
	}

	mutex_lock(&vb->balloon_lock);

	vb->num_pfns = 0;

	while ((page = balloon_page_pop(&pages))) {
		balloon_page_enqueue(&vb->vb_dev_info, page);

		set_page_pfns(vb, vb->pfns + vb->num_pfns, page);
		vb->num_pages += VIRTIO_BALLOON_PAGES_PER_PAGE;
		if (!virtio_has_feature(vb->vdev,
					VIRTIO_BALLOON_F_DEFLATE_ON_OOM))
			adjust_managed_page_count(page, -1);
		vb->num_pfns += VIRTIO_BALLOON_PAGES_PER_PAGE;
	}

	num_allocated_pages = vb->num_pfns;
	/* Did we get any? */
	if (vb->num_pfns != 0)
		tell_host(vb, vb->inflate_vq);
	mutex_unlock(&vb->balloon_lock);

	return num_allocated_pages;
}

static void release_pages_balloon(struct virtio_balloon *vb,
				 struct list_head *pages)
{
	struct page *page, *next;

	list_for_each_entry_safe(page, next, pages, lru) {
		if (!virtio_has_feature(vb->vdev,
					VIRTIO_BALLOON_F_DEFLATE_ON_OOM))
			adjust_managed_page_count(page, 1);
		list_del(&page->lru);
		put_page(page); /* balloon reference */
	}
}

static unsigned int leak_balloon(struct virtio_balloon *vb, size_t num)
{
	unsigned int num_freed_pages;
	struct page *page;
	struct balloon_dev_info *vb_dev_info = &vb->vb_dev_info;
	LIST_HEAD(pages);

	/* We can only do one array worth at a time. */
	num = min(num, ARRAY_SIZE(vb->pfns));

	mutex_lock(&vb->balloon_lock);
	/* We can't release more pages than taken */
	num = min(num, (size_t)vb->num_pages);
	for (vb->num_pfns = 0; vb->num_pfns < num;
	     vb->num_pfns += VIRTIO_BALLOON_PAGES_PER_PAGE) {
		page = balloon_page_dequeue(vb_dev_info);
		if (!page)
			break;
		set_page_pfns(vb, vb->pfns + vb->num_pfns, page);
		list_add(&page->lru, &pages);
		vb->num_pages -= VIRTIO_BALLOON_PAGES_PER_PAGE;
	}

	num_freed_pages = vb->num_pfns;
	/*
	 * Note that if
	 * virtio_has_feature(vdev, VIRTIO_BALLOON_F_MUST_TELL_HOST);
	 * is true, we *have* to do it in this order
	 */
	if (vb->num_pfns != 0)
		tell_host(vb, vb->deflate_vq);
	release_pages_balloon(vb, &pages);
	mutex_unlock(&vb->balloon_lock);
	return num_freed_pages;
}

static inline s64 towards_target(struct virtio_balloon *vb)
{
	s64 target;
	u32 num_pages;

	/* Legacy balloon config space is LE, unlike all other devices. */
	virtio_cread_le(vb->vdev, struct virtio_balloon_config, num_pages,
			&num_pages);

	target = num_pages;
	return target - vb->num_pages;
}

static void update_balloon_size(struct virtio_balloon *vb)
{
	u32 actual = vb->num_pages;

	/* Legacy balloon config space is LE, unlike all other devices. */
	virtio_cwrite_le(vb->vdev, struct virtio_balloon_config, actual,
			 &actual);
}

static void update_balloon_size_func(struct work_struct *work)
{
	struct virtio_balloon *vb;
	s64 diff;

	vb = container_of(work, struct virtio_balloon,
			  update_balloon_size_work);
	diff = towards_target(vb);

	if (!diff)
		return;

	if (diff > 0)
		diff -= fill_balloon(vb, diff);
	else
		diff += leak_balloon(vb, -diff);
	update_balloon_size(vb);

	if (diff)
		queue_work(system_freezable_wq, work);
}

static int init_vqs(struct virtio_balloon *vb)
{
	struct virtqueue *vqs[VIRTIO_BALLOON_VQ_MAX];
	vq_callback_t *callbacks[VIRTIO_BALLOON_VQ_MAX];
	const char *names[VIRTIO_BALLOON_VQ_MAX];
	int err;

	/*
	 * Inflateq and deflateq are used unconditionally. The names[]
	 * will be NULL if the related feature is not enabled, which will
	 * cause no allocation for the corresponding virtqueue in find_vqs.
	 */
	callbacks[VIRTIO_BALLOON_VQ_INFLATE] = balloon_ack;
	names[VIRTIO_BALLOON_VQ_INFLATE] = "inflate";
	callbacks[VIRTIO_BALLOON_VQ_DEFLATE] = balloon_ack;
	names[VIRTIO_BALLOON_VQ_DEFLATE] = "deflate";


	err = virtio_find_vqs(vb->vdev, VIRTIO_BALLOON_VQ_MAX, vqs,
			      callbacks, names, NULL);
	if (err)
		return err;

	vb->inflate_vq = vqs[VIRTIO_BALLOON_VQ_INFLATE];
	vb->deflate_vq = vqs[VIRTIO_BALLOON_VQ_DEFLATE];

	return 0;
}

static int virtballoon_probe(struct virtio_device *vdev)
{
	struct virtio_balloon *vb;
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

	INIT_WORK(&vb->update_balloon_size_work, update_balloon_size_func);
	spin_lock_init(&vb->stop_update_lock);
	mutex_init(&vb->balloon_lock);
	init_waitqueue_head(&vb->acked);
	vb->vdev = vdev;

	balloon_devinfo_init(&vb->vb_dev_info);

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

static void remove_common(struct virtio_balloon *vb)
{
	/* There might be pages left in the balloon: free them. */
	while (vb->num_pages)
		leak_balloon(vb, vb->num_pages);
	update_balloon_size(vb);

	/* Now we reset the device so we can clean up the queues. */
	virtio_reset_device(vb->vdev);

	vb->vdev->config->del_vqs(vb->vdev);
}

static void virtballoon_changed(struct virtio_device *vdev)
{
}

static void virtballoon_remove(struct virtio_device *vdev)
{
	struct virtio_balloon *vb = vdev->priv;

	spin_lock_irq(&vb->stop_update_lock);
	vb->stop_update = true;
	spin_unlock_irq(&vb->stop_update_lock);
	cancel_work_sync(&vb->update_balloon_size_work);
	cancel_work_sync(&vb->update_balloon_stats_work);

	remove_common(vb);
	kfree(vb);
}

static int virtballoon_validate(struct virtio_device *vdev)
{
	/*
	 * Inform the hypervisor that our pages are poisoned or
	 * initialized. If we cannot do that then we should disable
	 * page reporting as it could potentially change the contents
	 * of our free pages.
	 */
	if (!want_init_on_free() && !page_poisoning_enabled_static())
		__virtio_clear_bit(vdev, VIRTIO_BALLOON_F_PAGE_POISON);
	else if (!virtio_has_feature(vdev, VIRTIO_BALLOON_F_PAGE_POISON))
		__virtio_clear_bit(vdev, VIRTIO_BALLOON_F_REPORTING);

	__virtio_clear_bit(vdev, VIRTIO_F_ACCESS_PLATFORM);
	return 0;
}

static unsigned int features[] = {
	VIRTIO_BALLOON_F_MUST_TELL_HOST,
};

static struct virtio_driver virtio_balloon_driver = {
	.feature_table = features,
	.feature_table_size = ARRAY_SIZE(features),
	.driver.name =	KBUILD_MODNAME,
	.driver.owner =	THIS_MODULE,
	.id_table =	id_table,
	.validate =	virtballoon_validate,
	.probe =	virtballoon_probe,
	.remove =	virtballoon_remove,
	.config_changed = virtballoon_changed,
};

module_virtio_driver(virtio_balloon_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio balloon driver");
MODULE_LICENSE("GPL");
