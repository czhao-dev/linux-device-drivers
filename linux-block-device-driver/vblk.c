// SPDX-License-Identifier: GPL-2.0
/*
 * vblk - virtual RAM-backed block device
 *
 * Registers /dev/vblk0 as a block device backed by a single vmalloc'd
 * buffer. Demonstrates the blk-mq request-queue contract (gendisk,
 * block_device_operations, struct request/bio segment iteration) without
 * any real hardware dependency — the block-layer analogue of circbuf's
 * character-device contract.
 *
 * Verified against Linux 6.8.0-124-generic (Ubuntu 24.04), which uses
 * blk_mq_alloc_disk() and the blk_mode_t-based block_device_operations
 * signatures. Building against a kernel where these APIs differ will
 * require updating open()/ioctl() signatures and disk allocation calls;
 * see Documentation/block/ and block/blk-mq.c in that kernel's source.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/errno.h>

#include "vblk.h"

#define VBLK_SECTOR_SIZE  512
#define VBLK_SECTOR_SHIFT 9

static unsigned long disk_size_mb = 16;
module_param(disk_size_mb, ulong, 0444);
MODULE_PARM_DESC(disk_size_mb, "Size of the RAM-backed disk in megabytes (default 16)");

struct vblk_dev {
	struct gendisk *disk;
	struct blk_mq_tag_set tag_set;

	void *data;        /* vmalloc'd backing store */
	u64 size_bytes;

	spinlock_t lock;   /* protects data region + stats counters */

	u64 reads;
	u64 writes;
	u64 read_sectors;
	u64 write_sectors;
};

static int vblk_major;
static struct vblk_dev *vblk_device;

static blk_status_t vblk_queue_rq(struct blk_mq_hw_ctx *hctx,
				   const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;
	struct vblk_dev *dev = hctx->queue->queuedata;
	struct bio_vec bvec;
	struct req_iterator iter;
	loff_t pos = (loff_t)blk_rq_pos(rq) << VBLK_SECTOR_SHIFT;
	bool write = rq_data_dir(rq) == WRITE;
	unsigned long flags;

	blk_mq_start_request(rq);

	if (pos + blk_rq_bytes(rq) > dev->size_bytes) {
		blk_mq_end_request(rq, BLK_STS_IOERR);
		return BLK_STS_OK;
	}

	spin_lock_irqsave(&dev->lock, flags);

	rq_for_each_segment(bvec, rq, iter) {
		void *buf = page_address(bvec.bv_page) + bvec.bv_offset;
		size_t len = bvec.bv_len;

		if (write)
			memcpy(dev->data + pos, buf, len);
		else
			memcpy(buf, dev->data + pos, len);

		pos += len;
	}

	if (write) {
		dev->writes++;
		dev->write_sectors += blk_rq_sectors(rq);
	} else {
		dev->reads++;
		dev->read_sectors += blk_rq_sectors(rq);
	}

	spin_unlock_irqrestore(&dev->lock, flags);

	blk_mq_end_request(rq, BLK_STS_OK);
	return BLK_STS_OK;
}

static const struct blk_mq_ops vblk_mq_ops = {
	.queue_rq = vblk_queue_rq,
};

static int vblk_open(struct gendisk *disk, blk_mode_t mode)
{
	return 0;
}

static void vblk_release(struct gendisk *disk)
{
}

static int vblk_ioctl(struct block_device *bdev, blk_mode_t mode,
		       unsigned int cmd, unsigned long arg)
{
	struct vblk_dev *dev = bdev->bd_disk->private_data;
	struct vblk_stats stats;
	unsigned long flags;

	switch (cmd) {
	case VBLK_GET_STATS:
		spin_lock_irqsave(&dev->lock, flags);
		stats.capacity_bytes = dev->size_bytes;
		stats.sectors = dev->size_bytes >> VBLK_SECTOR_SHIFT;
		stats.reads = dev->reads;
		stats.writes = dev->writes;
		stats.read_sectors = dev->read_sectors;
		stats.write_sectors = dev->write_sectors;
		spin_unlock_irqrestore(&dev->lock, flags);

		if (copy_to_user((struct vblk_stats __user *)arg, &stats,
				  sizeof(stats)))
			return -EFAULT;
		return 0;

	default:
		return -ENOTTY;
	}
}

static const struct block_device_operations vblk_bdev_ops = {
	.owner   = THIS_MODULE,
	.open    = vblk_open,
	.release = vblk_release,
	.ioctl   = vblk_ioctl,
};

static int __init vblk_init(void)
{
	struct gendisk *disk;
	int ret;

	if (disk_size_mb == 0) {
		pr_err("vblk: disk_size_mb must be greater than 0\n");
		return -EINVAL;
	}

	vblk_device = kzalloc(sizeof(*vblk_device), GFP_KERNEL);
	if (!vblk_device)
		return -ENOMEM;

	vblk_device->size_bytes = (u64)disk_size_mb << 20;
	spin_lock_init(&vblk_device->lock);

	vblk_device->data = vmalloc(vblk_device->size_bytes);
	if (!vblk_device->data) {
		ret = -ENOMEM;
		goto out_free_dev;
	}

	vblk_major = register_blkdev(0, VBLK_DEVICE_NAME);
	if (vblk_major < 0) {
		ret = vblk_major;
		goto out_free_data;
	}

	memset(&vblk_device->tag_set, 0, sizeof(vblk_device->tag_set));
	vblk_device->tag_set.ops = &vblk_mq_ops;
	vblk_device->tag_set.nr_hw_queues = 1;
	vblk_device->tag_set.queue_depth = 128;
	vblk_device->tag_set.numa_node = NUMA_NO_NODE;
	vblk_device->tag_set.flags = BLK_MQ_F_SHOULD_MERGE;

	ret = blk_mq_alloc_tag_set(&vblk_device->tag_set);
	if (ret)
		goto out_unregister;

	disk = blk_mq_alloc_disk(&vblk_device->tag_set, vblk_device);
	if (IS_ERR(disk)) {
		ret = PTR_ERR(disk);
		goto out_free_tags;
	}

	disk->major = vblk_major;
	disk->first_minor = 0;
	disk->minors = 1;
	disk->flags |= GENHD_FL_NO_PART;
	disk->fops = &vblk_bdev_ops;
	disk->private_data = vblk_device;
	snprintf(disk->disk_name, DISK_NAME_LEN, "vblk0");

	blk_queue_logical_block_size(disk->queue, VBLK_SECTOR_SIZE);
	blk_queue_physical_block_size(disk->queue, VBLK_SECTOR_SIZE);
	set_capacity(disk, vblk_device->size_bytes >> VBLK_SECTOR_SHIFT);

	vblk_device->disk = disk;

	ret = add_disk(disk);
	if (ret)
		goto out_cleanup_disk;

	pr_info("vblk: loaded, disk_size_mb=%lu, /dev/%s0 ready\n",
		disk_size_mb, VBLK_DEVICE_NAME);
	return 0;

out_cleanup_disk:
	put_disk(disk);
out_free_tags:
	blk_mq_free_tag_set(&vblk_device->tag_set);
out_unregister:
	unregister_blkdev(vblk_major, VBLK_DEVICE_NAME);
out_free_data:
	vfree(vblk_device->data);
out_free_dev:
	kfree(vblk_device);
	return ret;
}

static void __exit vblk_exit(void)
{
	del_gendisk(vblk_device->disk);
	put_disk(vblk_device->disk);
	blk_mq_free_tag_set(&vblk_device->tag_set);
	unregister_blkdev(vblk_major, VBLK_DEVICE_NAME);
	vfree(vblk_device->data);
	kfree(vblk_device);
	pr_info("vblk: unloaded\n");
}

module_init(vblk_init);
module_exit(vblk_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("vblk project");
MODULE_DESCRIPTION("Virtual RAM-backed block device");
