# Linux Kernel Drivers

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Language: C](https://img.shields.io/badge/language-C-00599C.svg?logo=c&logoColor=white)](.)
[![Platform: Linux Kernel Module](https://img.shields.io/badge/platform-Linux%20Kernel%20Module-FCC624.svg?logo=linux&logoColor=black)](.)
[![Dev env: Docker](https://img.shields.io/badge/dev%20env-Docker-2496ED.svg?logo=docker&logoColor=white)](.)
[![Tested under: QEMU](https://img.shields.io/badge/tested%20under-QEMU-FF6600.svg?logo=qemu&logoColor=white)](.)

Four Linux kernel modules, each working through a different driver subsystem
and the contract it imposes between kernel and user space, hardware, or the
networking stack. None require physical hardware: every driver builds and
runs end-to-end in Docker + QEMU against a real kernel, on Linux, macOS, or
Windows.

| Driver | Subsystem | Core mechanisms |
|---|---|---|
| [`linux-character-device-driver`](linux-character-device-driver/) | Character device | `file_operations`, mutex, wait queues, blocking I/O, `ioctl` |
| [`linux-block-device-driver`](linux-block-device-driver/) | Block I/O (`gendisk`/blk-mq) | `bio`/`request` segment iteration, spinlock-protected memcpy, page cache vs. `O_DIRECT` |
| [`linux-network-device-driver`](linux-network-device-driver/) | Networking (`net_device`) | `sk_buff`, descriptor rings, NAPI, TX queue backpressure |
| [`linux-pcie-device-driver`](linux-pcie-device-driver/) | PCI / PCIe | BAR-mapped MMIO, MSI interrupts, DMA buffer management |

---

## Overview

Each project targets a different kernel subsystem on purpose, so that
together they cover most of the contracts a driver author has to get right:

- **[`circbuf`](linux-character-device-driver/)** — a virtual circular-buffer
  character device. No hardware dependency, so it isolates the
  kernel/user-space boundary itself: `copy_to_user`/`copy_from_user`, mutex
  vs. spinlock choice, wait-queue-based blocking I/O, and `ioctl` design.
- **[`vblk`](linux-block-device-driver/)** — a virtual RAM-backed block
  device exposed as `/dev/vblk0`. Covers the block layer's `gendisk`/blk-mq
  contract: request dispatch, `bio`/`request` segment iteration, the
  spinlock-vs-mutex tradeoff once user-copies are off the table, and the
  page-cache-vs-`O_DIRECT` distinction that governs when I/O actually
  reaches the driver.
- **[`netdrv`](linux-network-device-driver/)** — a software point-to-point
  Ethernet pair (`vnet0` ↔ `vnet1`) built around the same structures a real
  NIC driver uses: TX/RX descriptor rings, NAPI interrupt-mitigated
  reception, and `netif_stop_queue`/`wake_queue` backpressure.
- **[`pcie-edu-driver`](linux-pcie-device-driver/)** — a driver for QEMU's
  `edu` educational PCI device, covering the full PCI driver lifecycle:
  bus enumeration, BAR-mapped MMIO, MSI interrupt handling, and
  `dma_alloc_coherent` DMA buffer management.

The character device project establishes the kernel/user-space boundary and
synchronization primitives; the block device project moves that contract
into the block layer's request/bio model; the networking and PCIe projects
each build further on that foundation while introducing their own
subsystem-specific object model (`net_device`/`sk_buff` vs. `pci_dev`/MMIO/MSI)
and context rules (softirq NAPI polling vs. hardirq interrupt handling).

---

## Repository Layout

```text
.
├── linux-character-device-driver/   # circbuf — virtual character device
├── linux-block-device-driver/       # vblk    — virtual RAM-backed block device
├── linux-network-device-driver/     # netdrv  — virtual Ethernet driver pair
├── linux-pcie-device-driver/        # pcie-edu-driver — QEMU "edu" PCIe driver
└── LICENSE                          # MIT
```

Each subdirectory is self-contained, with its own `Makefile`, `docker/`
test harness, and `README.md` covering architecture, key concepts, build/test
instructions, and verified test results.

---

## Building and Testing

Every driver follows the same pattern: build against real kernel headers in
Docker, then boot a real kernel under QEMU to load and exercise the module —
not a build-only check. From within any of the three subdirectories:

```bash
./docker/run.sh test     # circbuf, vblk, pcie-edu-driver
```

```bash
docker build -f docker/Dockerfile -t netdrv-e2e . && docker run --rm netdrv-e2e   # netdrv
```

See each project's README for native (non-Docker) build instructions,
architecture diagrams, design tradeoffs, and the last verified test run.

---

## References

- *Linux Device Drivers, 3rd Edition* — Corbet, Rubini, Kroah-Hartman (free at lwn.net)
- *Linux Kernel Development, 3rd Edition* — Robert Love
- Rosen, R. *Linux Kernel Networking: Implementation and Theory*
- `Documentation/driver-api/`, `Documentation/block/`, `Documentation/networking/napi.rst`, `Documentation/PCI/pci.rst` in the Linux kernel source tree

## License

MIT — see [LICENSE](LICENSE).
