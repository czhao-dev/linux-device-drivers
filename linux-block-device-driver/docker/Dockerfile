FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# build-essential + linux-headers-generic: compile vblk.ko against real kernel headers
# linux-image-generic: provides a bootable vmlinuz to run that kernel under QEMU
# busybox-static + cpio: build a minimal initramfs for the QEMU guest
# qemu-system-arm: provides qemu-system-aarch64
# kmod: insmod/rmmod/lsmod (also used by busybox fallback)
# busybox-static's built-in mke2fs applet formats /dev/vblk0 for the
# mkfs+mount capstone test, so no separate e2fsprogs package is needed
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        linux-headers-generic \
        linux-image-generic \
        kmod \
        qemu-system-arm \
        qemu-utils \
        busybox-static \
        cpio \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
