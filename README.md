# Linux Device Drivers

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Language: C](https://img.shields.io/badge/language-C-00599C.svg?logo=c&logoColor=white)](.)
[![Platform: Linux Kernel Module](https://img.shields.io/badge/platform-Linux%20Kernel%20Module-FCC624.svg?logo=linux&logoColor=black)](.)
[![Dev env: Docker](https://img.shields.io/badge/dev%20env-Docker-2496ED.svg?logo=docker&logoColor=white)](.)
[![Tested under: QEMU](https://img.shields.io/badge/tested%20under-QEMU-FF6600.svg?logo=qemu&logoColor=white)](.)

Four Linux kernel modules, each targeting a different driver subsystem and the
contract it imposes between kernel and user space, hardware, or the networking
stack. None require physical hardware: every driver builds and runs end-to-end
in Docker + QEMU against a real kernel, on Linux, macOS, or Windows.

| Driver | Subsystem | Core mechanisms |
|---|---|---|
| [`linux-character-device-driver`](linux-character-device-driver/) | Character device | `file_operations`, mutex, wait queues, blocking I/O, `ioctl` |
| [`linux-block-device-driver`](linux-block-device-driver/) | Block I/O (`gendisk`/blk-mq) | `bio`/`request` segment iteration, spinlock-protected memcpy, page cache vs. `O_DIRECT` |
| [`linux-network-device-driver`](linux-network-device-driver/) | Networking (`net_device`) | `sk_buff`, descriptor rings, NAPI, TX queue backpressure |
| [`linux-pcie-device-driver`](linux-pcie-device-driver/) | PCI / PCIe | BAR-mapped MMIO, MSI interrupts, DMA buffer management |

---

## Overview

Each project targets a different kernel subsystem on purpose, so that together
they cover most of the contracts a driver author has to get right.

### [`circbuf`](linux-character-device-driver/) — Virtual Character Device

A kernel module that registers `/dev/circbuf` as a bounded circular-buffer
device. Writers produce bytes into a fixed-size buffer; readers consume them in
FIFO order. When the buffer is full, writers block; when it is empty, readers
block. Multiple processes can open the device concurrently and are safely
arbitrated through kernel synchronization primitives.

**What it covers:** `file_operations` vtable (C-level polymorphism between VFS
and the driver); `copy_to_user`/`copy_from_user` for crossing the kernel/user
boundary safely; `mutex` for sleeping critical sections vs. `spinlock` for
atomic ones; `wait_queue_head_t` for blocking I/O; and `ioctl` design for
out-of-band device control. Because there is no hardware dependency, the focus
stays entirely on getting these contracts right.

**Key object:** `struct file_operations` — a vtable of function pointers the
kernel calls when user space invokes `open`, `read`, `write`, `ioctl`, or
`release` on the device file.

---

### [`vblk`](linux-block-device-driver/) — Virtual RAM-backed Block Device

A kernel module that registers `/dev/vblk0` backed by a `vmalloc`'d region
of kernel memory. It behaves like a real disk: `dd`, `mkfs`, `mount`, and
arbitrary `pread`/`pwrite` all work against it because it implements the same
`gendisk` / `block_device_operations` / `blk_mq_ops` contract every real block
driver implements. Only the "hardware" — a contiguous kernel buffer instead of
a physical device — differs.

**What it covers:** `gendisk` + blk-mq registration (`blk_mq_alloc_disk`,
`add_disk`); `bio`/`request` segment iteration inside `queue_rq`; why a
`spinlock` is the right primitive once `copy_to/from_user` is off the table;
and the page-cache vs. `O_DIRECT` distinction that governs whether I/O ever
reaches the driver at all. The test suite validates all three paths: buffered
I/O through the page cache, `O_DIRECT` bypassing it, and a full
`mke2fs` + `mount` + file I/O + `umount` round trip through ext2.

**Key object:** `struct gendisk` paired with a `struct blk_mq_tag_set` — the
block-layer analogue of `file_operations`.

---

### [`netdrv`](linux-network-device-driver/) — Virtual Ethernet Driver Pair with NAPI

A kernel module that creates a software point-to-point Ethernet pair (`vnet0`
↔ `vnet1`) built around the same structures a real NIC driver uses: TX/RX
descriptor rings, NAPI interrupt-mitigated packet reception, and
`netif_stop_queue`/`netif_wake_queue` TX backpressure. A packet transmitted
on one interface appears as received on the other; the "wire" is software,
but the driver-side mechanics follow the real networking stack contracts.

**What it covers:** `net_device` registration and `net_device_ops`; `sk_buff`
lifecycle (allocation, headroom, push/pull); NAPI (why per-packet interrupts
would saturate the CPU and how poll-based batching avoids it); TX queue
backpressure under load; and network namespace isolation for testing. The
project includes a real end-to-end `ping` test that caught an actual bug during
development — `IFF_NOARP` made ring counters look healthy while `ping` silently
failed 100% of the time.

**Key object:** `struct net_device` — the networking stack's equivalent of
`gendisk`, paired with a `struct napi_struct` per interface.

---

### [`pcie-edu-driver`](linux-pcie-device-driver/) — PCIe Driver for QEMU's `edu` Device

A kernel module that drives QEMU's built-in `edu` educational PCI device
(vendor `0x1234`, device `0x11e8`). Unlike the virtual drivers above, this
targets a device that is enumerated over the PCIe bus, has BAR-mapped memory
registers, raises hardware interrupts, and exposes a DMA engine — it just
happens to be emulated by QEMU rather than soldered to a board. The driver
code, and everything it has to get right, is identical to what a real NIC,
storage controller, or accelerator driver requires.

**What it covers:** the full PCI driver lifecycle (`pci_enable_device`,
`pci_iomap` for BAR0 MMIO, `pci_alloc_irq_vectors` for MSI,
`dma_alloc_coherent` for DMA buffers, `pci_unregister_driver` cleanup);
`ioread32`/`iowrite32` for MMIO register access; an MSI interrupt handler that
wakes a wait queue to unblock a user-space `ioctl`; and a DMA round-trip test
(host → device → host). The driver also exposes a `misc` character device at
`/dev/pcie_edu` so user space can trigger factorial computations and DMA
transfers via `ioctl`.

**Key object:** `struct pci_dev` — the bus-enumerated device handle, paired
with BAR-mapped MMIO, an MSI interrupt vector, and a `dma_alloc_coherent`
buffer.

---

### How the Four Projects Relate

The character device establishes the kernel/user-space boundary and
synchronization primitives. The block device moves that same contract into the
block layer's request/bio model, and introduces the page-cache visibility
problem. The networking and PCIe drivers build further on that foundation while
introducing their own subsystem-specific object models (`net_device`/`sk_buff`
vs. `pci_dev`/MMIO/MSI) and execution-context rules (softirq NAPI polling vs.
hardirq MSI handling).

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
test harness, and `README.md` covering architecture, key concepts,
build/test instructions, design tradeoffs, and verified test results.

---

## Building and Testing

### Prerequisites

| Tool | Minimum version | Notes |
|---|---|---|
| Docker | 20.10+ | Required for the recommended Docker + QEMU path |
| QEMU | 6.0+ | Embedded inside the Docker image; no host install needed |
| make / gcc | any recent | Only needed for native (non-Docker) builds |
| Linux host with matching kernel headers | — | Alternative to Docker; see each project's README |

No physical hardware, no Linux host, and no QEMU pre-install are required for
the default Docker path. Docker pulls the matching kernel headers and QEMU
image automatically.

### Quick start

Every driver follows the same pattern: build against real kernel headers inside
Docker, boot a stock kernel under QEMU, then load and exercise the module
end-to-end — not a build-only check.

```bash
# circbuf, vblk, pcie-edu-driver
cd <project-directory>
./docker/run.sh test
```

```bash
# netdrv (self-contained Docker image)
cd linux-network-device-driver
docker build -f docker/Dockerfile -t netdrv-e2e . && docker run --rm netdrv-e2e
```

### What the harness does

`docker/run.sh test` (and the equivalent `netdrv` command):

1. Builds the `.ko` against real kernel headers inside a Docker container.
2. Boots a stock `6.8.0-124-generic` kernel under QEMU with a minimal busybox
   initramfs.
3. `insmod`s the module, runs the test suite (basic I/O, stress, filesystem
   mount where applicable, ioctl round-trips), and `rmmod`s the module.
4. Checks `dmesg` for panics, lockups, or unexpected warnings.
5. Exits non-zero on any failure.

### Native (non-Docker) builds

Each project's README documents a native build path for environments where
Docker is unavailable. The general pattern:

```bash
make                  # build the .ko against the running kernel's headers
sudo insmod <mod>.ko
dmesg | tail -20      # confirm probe / registration
sudo ./<test_binary>
sudo rmmod <mod>
```

This requires a Linux host (or VM) with matching kernel headers and — for the
PCIe driver — a QEMU session started with `-device edu`.

---

## Verified Test Results

All four drivers pass their full test suites against kernel
`6.8.0-124-generic` (Ubuntu 24.04, aarch64 / QEMU `virt` or `q35`).

### GCP validation run — 2026-07-12 (three runs per project)

The following is a separate, reproducible GCP run against source commit
`2e90b55e06973bf40916cb3552fc289c128738d9`. The character, block, and network
harnesses ran on an Ubuntu 24.04 `t2a-standard-4` (aarch64) VM in
`us-central1-a`; the PCIe harness ran on an Ubuntu 24.04 `e2-standard-4`
(x86_64) VM in `us-west1-b`, matching its x86 QEMU guest. Both runners used
host kernel `6.17.0-1020-gcp`, Docker 29.1.3, container kernel
`6.8.0-134-generic`, and QEMU 8.2.2.

| Driver | Run 1 | Run 2 | Run 3 | Workload result |
|---|---:|---:|---:|---|
| `circbuf` | ✅ Pass | ✅ Pass | ✅ Pass | Stress throughput: 0.699 / 0.775 / 0.707 MiB/s |
| `vblk` | ❌ Fail | ❌ Fail | ❌ Fail | `basic_test` passed; stress final verification reported corruption in region 0 in every run. The observed 3.762 / 3.662 / 3.605 MiB/s rates are **not valid benchmark results**. |
| `netdrv` | ✅ Pass | ✅ Pass | ✅ Pass | UDP sender: 27.0 / 27.7 / 26.9 Mbit/s; TX queue stops: 13 / 13 / 18 |
| `pcie-edu-driver` | ✅ Pass | ✅ Pass | ✅ Pass | Enumeration, MSI, factorial, DMA round-trip, and clean removal passed |

The block-driver failure stops its guest init sequence before its filesystem
capstone and reload checks, so those checks were not represented as passed in
this run. The network UDP workload intentionally saturates a small ring; the
reported sender rate and driver packet counters are the comparable metrics,
while receiver UDP loss is expected under that backpressure test.

![Three-run GCP validation matrix](docs/test-results/2026-07-12/pass-matrix.png)

![GCP stress and network workload measurements](docs/test-results/2026-07-12/workload-results.png)

The complete machine-readable measurements are available as
[JSON](docs/test-results/2026-07-12/gcp-validation.json) and
[CSV](docs/test-results/2026-07-12/gcp-validation.csv). The plots are
regenerated from the JSON with `scripts/plot_gcp_results.py`.

### circbuf

| Check | Result |
|---|---|
| `insmod` / `rmmod` (default 4096-byte buffer) | ✅ Pass |
| `basic_test` — write → read → `ioctl` roundtrip | ✅ Pass |
| `query_stats` — `ioctl` buffer introspection | ✅ `capacity=4096 used=0 available=4096` |
| `stress` — 4 writers / 4 readers, 5 s | ✅ `total_written=13682240 == total_read=13682240` |
| Reload with `buffer_size=16384` | ✅ `capacity=16384` confirmed |
| `dmesg` | No panics, no lockups |

### vblk

| Check | Result |
|---|---|
| `insmod` / `rmmod` (default 16 MiB) | ✅ Pass |
| `basic_test` — `O_DIRECT` `pwrite`/`pread`/`pwritev` + `ioctl` | ✅ `capacity_bytes=16777216 reads=3 writes=3` |
| `stress` — 4 threads, disjoint regions, 5 s | ✅ `total_written=222857728`, zero corruption |
| `mke2fs` + `mount -t ext2` + file I/O + `umount` | ✅ Pass |
| Reload with `disk_size_mb=8` | ✅ `capacity_bytes=8388608` confirmed |
| `dmesg` | No panics, no lockups |

### netdrv

| Check | Result |
|---|---|
| Module load — `vnet0` / `vnet1` registered | ✅ |
| Carrier state on `open` / `stop` | ✅ |
| `ping` across netns pair | ✅ 4/4 packets, 0% loss |
| `tx_packets` / `rx_packets` symmetry | ✅ 11 == 11 |
| Flood ping with `ring_size=4` | ✅ 2000/2000, 0% loss |
| TX backpressure under `iperf3` UDP load | ✅ `tx_queue_stops/wakes = 2`, 0 packets lost (59670 == 59670) |
| 10× load/unload loop | ✅ clean `dmesg` throughout |

### pcie-edu-driver

| Check | Result |
|---|---|
| `edu` device enumerated (`lspci -d 1234:11e8`) | ✅ Pass |
| `probe()` — BAR0 mapped, ident register read, DMA buffer allocated | ✅ Pass |
| MSI vector registered (`/proc/interrupts`) | ✅ Pass |
| `/dev/pcie_edu` opens successfully | ✅ Pass |
| Liveness register round-trip (`write_val` → `~write_val`) | ✅ Pass |
| `PCIE_EDU_COMPUTE` — 0!, 1!, 5!, 10!, 12! via MSI | ✅ Pass (5/5) |
| `PCIE_EDU_DMA_TRANSFER` — 4096-byte host→device→host | ✅ Pass |
| `remove()` / `rmmod` — no leaked MMIO or IRQ | ✅ Pass |

---

## References

- *Linux Device Drivers, 3rd Edition* — Corbet, Rubini, Kroah-Hartman (free at lwn.net)
- *Linux Kernel Development, 3rd Edition* — Robert Love
- Rosen, R. *Linux Kernel Networking: Implementation and Theory*
- `Documentation/driver-api/`, `Documentation/block/`, `Documentation/networking/napi.rst`, `Documentation/PCI/pci.rst` in the Linux kernel source tree

---

## License

MIT — see [LICENSE](LICENSE).
