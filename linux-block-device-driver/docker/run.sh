#!/usr/bin/env bash
# Helper for building vblk.ko and running it under a real Linux kernel
# (via QEMU + a minimal busybox initramfs) from inside a Docker container,
# since macOS can't build/load kernel modules natively.
set -euo pipefail

IMAGE_NAME=vblk-dev
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CMD="${1:-}"

build_image() {
    docker build -t "$IMAGE_NAME" -f "$PROJECT_ROOT/docker/Dockerfile" "$PROJECT_ROOT"
}

BUILD_CMD='KVER=$(ls /lib/modules | grep -v linuxkit | head -n1) &&
           make clean KDIR=/lib/modules/$KVER/build &&
           make KDIR=/lib/modules/$KVER/build &&
           make -C tests clean &&
           make tests'

# Packages vblk.ko + test binaries + busybox into an initramfs and boots it
# with qemu-system-aarch64 against the installed kernel (docker/init.sh runs
# as PID 1 and drives the test sequence). busybox-static includes a built-in
# ext2 mke2fs applet, used for the mkfs+mount capstone test.
RUN_CMD='KVER=$(ls /lib/modules | grep -v linuxkit | head -n1) &&
         rm -rf /tmp/initramfs && mkdir -p /tmp/initramfs/bin &&
         cp /usr/bin/busybox /tmp/initramfs/bin/busybox &&
         cp /src/docker/init.sh /tmp/initramfs/init && chmod +x /tmp/initramfs/init &&
         cp /src/vblk.ko /src/tests/basic_test /src/tests/query_stats /src/tests/stress /tmp/initramfs/ &&
         ( cd /tmp/initramfs && find . | cpio -o -H newc 2>/dev/null | gzip > /tmp/initramfs.cpio.gz ) &&
         qemu-system-aarch64 -M virt -cpu max -smp 2 -m 512M -nodefaults \
             -kernel /boot/vmlinuz-$KVER -initrd /tmp/initramfs.cpio.gz \
             -append "console=ttyAMA0 panic=-1" -serial mon:stdio -nographic -no-reboot'

case "$CMD" in
  build)
    build_image
    docker run --rm -v "$PROJECT_ROOT":/src -w /src "$IMAGE_NAME" bash -c "$BUILD_CMD"
    ;;
  test)
    build_image
    docker run --rm --privileged -v "$PROJECT_ROOT":/src -w /src "$IMAGE_NAME" \
        bash -c "$BUILD_CMD && $RUN_CMD"
    ;;
  shell)
    build_image
    docker run --rm -it --privileged -v "$PROJECT_ROOT":/src -w /src "$IMAGE_NAME" bash
    ;;
  *)
    echo "usage: $0 {build|test|shell}"
    exit 1
    ;;
esac
