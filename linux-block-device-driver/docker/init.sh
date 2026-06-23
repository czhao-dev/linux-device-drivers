#!/bin/busybox sh
# PID 1 inside the QEMU guest (busybox initramfs). Mounts virtual filesystems,
# exercises vblk.ko, and powers off the VM with a clear pass/fail marker.
set -e
export PATH=/bin:/sbin:/usr/bin:/usr/sbin

/bin/busybox --install -s /bin

mkdir -p /proc /sys /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev

echo "== insmod (default disk_size_mb) =="
insmod /vblk.ko
lsmod | grep vblk
ls -l /dev/vblk0

echo "== basic_test =="
/basic_test /dev/vblk0

echo "== query_stats =="
/query_stats /dev/vblk0

echo "== stress =="
/stress /dev/vblk0 --writers=4 --duration=5

echo "== mkfs + mount + file I/O (capstone block device test) =="
mkdir -p /mnt/vblk
mke2fs -F /dev/vblk0
mount -t ext2 /dev/vblk0 /mnt/vblk
echo "hello from a real filesystem on a virtual block device" > /mnt/vblk/hello.txt
sync
content="$(cat /mnt/vblk/hello.txt)"
if [ "$content" = "hello from a real filesystem on a virtual block device" ]; then
	echo "filesystem roundtrip: PASS"
else
	echo "filesystem roundtrip: FAIL (got: $content)"
	exit 1
fi
umount /mnt/vblk

echo "== rmmod =="
rmmod vblk

echo "== reload with disk_size_mb=8 =="
insmod /vblk.ko disk_size_mb=8
/query_stats /dev/vblk0
rmmod vblk

echo "ALL TESTS PASSED"

poweroff -f
