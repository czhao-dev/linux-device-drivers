#ifndef VBLK_H
#define VBLK_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define VBLK_DEVICE_NAME "vblk"

struct vblk_stats {
	__u64 capacity_bytes;   /* total backing store size */
	__u64 sectors;          /* capacity_bytes / 512 */
	__u64 reads;             /* completed read requests */
	__u64 writes;            /* completed write requests */
	__u64 read_sectors;      /* cumulative sectors read */
	__u64 write_sectors;     /* cumulative sectors written */
};

#define VBLK_MAGIC 0xB1

/* Query device capacity and cumulative I/O counters */
#define VBLK_GET_STATS _IOR(VBLK_MAGIC, 1, struct vblk_stats)

#endif /* VBLK_H */
