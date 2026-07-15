# pcie-edu-driver — Linux PCIe Driver

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](../LICENSE)
![Language: C](https://img.shields.io/badge/language-C-555.svg)

A Linux kernel module implementing a PCIe device driver for QEMU's "edu"
educational PCI device — covering the full driver lifecycle: PCI device
enumeration, BAR-based memory-mapped I/O, MSI interrupt handling, and DMA
buffer management. Fully reproducible without physical hardware or a Linux
host: `docker/run.sh test` builds the module, boots it under QEMU, and runs
the test suite end to end. See [Test Results](#test-results) for the last
verified run.

---

## Overview

Unlike a purely virtual kernel interface, this driver targets a real PCI device
— enumerated over the PCIe bus, with memory-mapped registers, hardware
interrupts, and DMA — that just happens to be emulated by QEMU rather than
soldered to a board. The driver code, and everything it has to get right, is the
same as for a real NIC, storage controller, or accelerator card.

`pcie-edu-driver` binds to QEMU's `edu` device (vendor ID `0x1234`, device ID
`0x11e8`), a PCI device built into QEMU specifically for driver-writing
practice. The device exposes a register-mapped interface supporting an
identification register, a "compute" register that triggers an asynchronous
factorial calculation and raises an interrupt on completion, and a small DMA
engine that can copy data between host memory and the device.

---

## Repository Layout

```text
.
├── pcie_edu.c  # driver: probe/remove, BAR mapping, MSI, DMA
├── pcie_edu.h  # register offsets, ioctl command definitions
├── Makefile    # kernel module build (kbuild)
├── docker/
│   ├── Dockerfile  # QEMU + kernel headers dev/test environment
│   ├── init.sh     # guest-side: insmod, run tests, report results
│   └── run.sh      # host-side: build image, boot QEMU (-device edu), capture output
├── docs/
│   └── edu-register-map.md  # register offsets, DMA addressing convention
└── tests/
    └── pcie_edu_test.c  # identification, compute+interrupt, DMA round-trip
```

---

## Architecture

```
 User Space
 ┌────────────────────────────────────────────┐
 │ /dev/pcie_edu                              │
 │ - read()/write(): liveness register access │
 │ - ioctl(PCIE_EDU_COMPUTE, &n)              │
 │ - ioctl(PCIE_EDU_DMA_TRANSFER, &xfer)      │
 └────────────────────────────────────────────┘
                         │
Kernel Space             ▼
 ┌──────────────────────────────────────────────────┐
 │ pcie_edu driver                                  │
 │ ┌─────────────────────────────────────────┐      │
 │ │ probe()/remove() — PCI driver lifecycle │      │
 │ │   pci_enable_device()                   │      │
 │ │   pci_iomap(BAR0) → MMIO base           │      │
 │ │   pci_alloc_irq_vectors() → MSI         │      │
 │ │   dma_alloc_coherent() → DMA buffer     │      │
 │ └─────────────────────────────────────────┘      │
 │ ┌──────────────────────────────────────────┐     │
 │ │ MMIO register access: ioread32/iowrite32 │     │
 │ └──────────────────────────────────────────┘     │
 │ ┌────────────────────────────────────────┐       │
 │ │ IRQ handler: ack status reg, wake_up() │       │
 │ └────────────────────────────────────────┘       │
 │ ┌──────────────────────────────────────────────┐ │
 │ │ misc char device interface (file_operations) │ │
 │ └──────────────────────────────────────────────┘ │
 └──────────────────────────────────────────────────┘
                         │ PCIe: config space, BAR0 MMIO, MSI, DMA
                         ▼
 ┌─────────────────────────────────────────────────────────┐
 │ QEMU "edu" PCI device  (1234:11e8)                      │
 │ reg 0x00  IDENT        (read-only ID value)             │
 │ reg 0x04  LIVENESS     (read returns ~written)          │
 │ reg 0x08  FACTORIAL    (write N, triggers compute)      │
 │ reg 0x20  STATUS       (bit0 computing, bit7 irq-en)    │
 │ reg 0x24  IRQ_STATUS   (pending interrupt bitmask)      │
 │ reg 0x64  IRQ_ACK      (write-1-to-clear)               │
 │ reg 0x80  DMA_SRC                                       │
 │ reg 0x88  DMA_DST                                       │
 │ reg 0x90  DMA_CNT                                       │
 │ reg 0x98  DMA_CMD      (bit0 start, bit1 dir, bit2 irq) │
 └─────────────────────────────────────────────────────────┘
```

The full register semantics (DMA addressing convention, 64-bit register
write order, etc.) are documented in
[docs/edu-register-map.md](docs/edu-register-map.md).

---

## Key Concepts

### 1. PCI Configuration Space vs. BARs

Every PCI device exposes a small **configuration space** — a standardized
region containing vendor ID, device ID, class code, and, critically, a set of
**Base Address Registers (BARs)**. Each BAR describes a region of the device's
memory or I/O space and, after the kernel assigns it an address during bus
enumeration, tells the CPU where that device's registers appear in the system's
physical address space.

The driver doesn't read config space manually for this — the PCI subsystem does
it during enumeration and matches the device against the driver's `id_table`:

```c
static const struct pci_device_id pcie_edu_ids[] = {
    { PCI_DEVICE(0x1234, 0x11e8) },
    { 0, }
};
MODULE_DEVICE_TABLE(pci, pcie_edu_ids);

static struct pci_driver pcie_edu_driver = {
    .name     = "pcie_edu",
    .id_table = pcie_edu_ids,
    .probe    = pcie_edu_probe,
    .remove   = pcie_edu_remove,
};
```

When the kernel finds a device matching `id_table`, it calls `probe()` — this
is the driver's entry point, analogous to `open()` in the character device
project but triggered by hardware enumeration rather than a user `open(2)`
call.

### 2. Memory-Mapped I/O (MMIO)

Inside `probe()`, the driver maps BAR0 into kernel virtual address space:

```c
static int pcie_edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    int ret;
    struct pcie_edu_dev *edu;

    ret = pci_enable_device(pdev);
    if (ret) return ret;

    ret = pci_request_regions(pdev, "pcie_edu");
    if (ret) goto err_disable;

    edu->mmio = pci_iomap(pdev, 0, 0);  // map BAR0
    if (!edu->mmio) { ret = -ENOMEM; goto err_release; }

    pci_set_master(pdev);  // allow device to initiate DMA
    ...
}
```

Once mapped, device registers are accessed with `ioread32`/`iowrite32`, never
through a raw pointer dereference:

```c
u32 ident = ioread32(edu->mmio + REG_IDENT);
iowrite32(20, edu->mmio + REG_FACTORIAL);   // ask device to compute 20!
```

**Why not just dereference the pointer?** Two reasons. First, portability:
`ioread32`/`iowrite32` handle architecture-specific details (some architectures
require special instructions for I/O-space access, and byte-order handling
differs across buses). Second, and more important, ordering: the compiler and
CPU are free to reorder, cache, or coalesce ordinary memory accesses, but device
registers often have side effects on read or write — reading a status register
might clear it, and writes might need to happen in a specific order to take
effect correctly. The `io*` accessors include the necessary barriers; a plain
`*(volatile u32*)` does not reliably provide the same guarantees across
architectures, even though `volatile` alone is a common (and insufficient)
intuition.

### 3. MSI Interrupts

The `edu` device raises an interrupt when an asynchronous operation (like the
factorial computation) completes. The driver requests an MSI vector rather than
relying on legacy line-based (INTx) interrupts:

```c
ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
if (ret < 0) goto err_unmap;

ret = request_irq(pci_irq_vector(pdev, 0), pcie_edu_irq_handler,
                   0, "pcie_edu", edu);
```

The handler runs in hardware interrupt context — strict constraints apply: no
sleeping, no taking mutexes, minimal work only.

```c
static irqreturn_t pcie_edu_irq_handler(int irq, void *data)
{
    struct pcie_edu_dev *edu = data;
    u32 status = ioread32(edu->mmio + REG_IRQ_STATUS);

    if (!status)
        return IRQ_NONE;          // not ours (shouldn't happen with MSI)

    iowrite32(status, edu->mmio + REG_IRQ_ACK);  // ack: write-1-to-clear
    edu->result_ready = true;
    wake_up_interruptible(&edu->wait_queue);

    return IRQ_HANDLED;
}
```

**Why MSI over INTx**: legacy INTx interrupts are level-triggered and often
shared across multiple devices on the same line — every handler on that line
runs for every interrupt, checking "is this mine?" MSI delivers a dedicated
message per device (and MSI-X allows multiple independent vectors per device),
eliminating the shared-line overhead and ambiguity. Virtually all modern PCIe
devices use MSI or MSI-X.

### 4. DMA Buffer Management

To move data without CPU-mediated copies, the driver allocates a
**DMA-coherent buffer** — memory the device can write to directly, with the
kernel handling any cache-coherency requirements:

```c
edu->dma_buf = dma_alloc_coherent(&pdev->dev, DMA_BUF_SIZE,
                                   &edu->dma_handle, GFP_KERNEL);
if (!edu->dma_buf) { ret = -ENOMEM; goto err_free_irq; }
```

`dma_alloc_coherent` returns two addresses for the *same* memory: `dma_buf`, a
kernel virtual address the CPU uses, and `dma_handle`, a **bus address** the
device uses. These are not the same number — on systems with an IOMMU, the bus
address may bear no resemblance to the physical address. The driver programs
the device's DMA registers with `dma_handle`, never with a CPU pointer:

```c
edu_write64(edu->mmio, REG_DMA_DST, edu->dma_handle);
edu_write64(edu->mmio, REG_DMA_CNT, transfer_len);
iowrite32(PCIE_EDU_DMA_RUN, edu->mmio + REG_DMA_CMD);
```

**Coherent vs. streaming mappings**: `dma_alloc_coherent` allocates a
long-lived buffer with consistent CPU/device views — appropriate here because
the buffer is allocated once at `probe()` time and reused for the driver's
lifetime. For one-shot transfers of buffers that originate in user space or
elsewhere (e.g., a network packet), the streaming API (`dma_map_single` /
`dma_unmap_single`, or `dma_map_sg` for scatter-gather) is more appropriate —
it maps an existing buffer for the duration of a single transfer and requires
explicit `dma_sync_*` calls around CPU access, in exchange for not needing a
dedicated long-lived allocation. This project uses the coherent API for
simplicity and discusses the streaming alternative under Future Extensions.

The driver's DMA test path is a full round trip: it fills its internal buffer
with a pattern, transfers host → device, overwrites the host buffer with the
pattern's complement (so a no-op transfer can't accidentally "pass"), transfers
device → host, then verifies the buffer matches the original pattern again.

### 5. Blocking User-Space Interface

A user-space program triggers a computation and blocks until the interrupt
handler signals completion — using the standard wait-queue pattern, here driven
by a hardware interrupt rather than another process:

```c
static long pcie_edu_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct pcie_edu_dev *edu = f->private_data;
    u32 n;

    switch (cmd) {
    case PCIE_EDU_COMPUTE:
        if (copy_from_user(&n, (void __user *)arg, sizeof(n)))
            return -EFAULT;

        edu->result_ready = false;
        iowrite32(n, edu->mmio + REG_FACTORIAL);  // device computes async

        wait_event_interruptible(edu->wait_queue, edu->result_ready);

        n = ioread32(edu->mmio + REG_FACTORIAL);  // read result
        return copy_to_user((void __user *)arg, &n, sizeof(n)) ? -EFAULT : 0;
    ...
    }
}
```

The structure is identical to the character device project's blocking read —
the only difference is *what* calls `wake_up_interruptible()`: there, another
process; here, an interrupt handler.

---

## Design Decisions and Tradeoffs

**MSI vs. INTx**

Covered above — MSI chosen for the standard reasons (no shared-line ambiguity,
better scalability). The `edu` device supports both; using MSI also exercises
`pci_alloc_irq_vectors`, the modern API that abstracts over INTx/MSI/MSI-X
uniformly, which is what production drivers use.

**Coherent DMA mapping vs. streaming**

Discussed above. Coherent is the right choice for this driver's long-lived
internal buffer; streaming would be the right choice if the driver needed to
DMA directly from user-supplied buffers, which is listed as a future extension.

**Non-threaded IRQ handler**

The handler does minimal, bounded work (read one register, write one register,
call `wake_up_interruptible`, which is safe in hardirq context) — no need for a
threaded IRQ (`request_threaded_irq`) or workqueue deferral. If the completion
handling required, for example, copying a large buffer or taking a mutex, the
work would need to move to a threaded handler or a tasklet/workqueue, since
hardirq context cannot sleep.

**Single character device vs. multiple device files**

All operations (register peek/poke, compute, DMA) are exposed through one
`/dev/pcie_edu` with `ioctl` for structured operations. An alternative is
splitting concerns across `sysfs` attributes (for simple register exposure) and
a character device (for DMA/compute operations) — `sysfs` is generally
preferred for simple configuration/status in production drivers, with the
character device reserved for data-path operations. This is noted as a future
extension.

---

## Quick Start

The whole build/boot/test cycle runs inside Docker, so it works the same on
Linux, macOS, or Windows — no native kernel module toolchain or VM image setup
required. The container provides the kernel headers, a bootable kernel, and
QEMU; [docker/run.sh](docker/run.sh) drives the rest.

```bash
git clone https://github.com/czhao-dev/linux-pcie-device-driver.git
cd linux-pcie-device-driver

./docker/run.sh build   # compile pcie_edu.ko and the test binary only
./docker/run.sh test    # build, boot QEMU with -device edu, run the test suite
./docker/run.sh shell   # drop into the dev container for interactive debugging
```

`docker/run.sh test` builds the module and test program against the
container's real kernel headers, packs them with busybox into a minimal
initramfs, and boots `qemu-system-x86_64 -device edu` with that initramfs.
[docker/init.sh](docker/init.sh), running as PID 1 inside the guest, loads the
module, runs [tests/pcie_edu_test.c](tests/pcie_edu_test.c), unloads the
module, and reports pass/fail for every check before powering off. See
[Test Results](#test-results) below for a real run's output.

### Running on a native Linux host instead

If you have a Linux machine (or VM) with QEMU and matching kernel headers
already set up, you don't need Docker:

```bash
make                          # build pcie_edu.ko against the running kernel
make tests                    # build tests/pcie_edu_test
sudo insmod pcie_edu.ko
dmesg | tail                  # confirm probe() succeeded, BAR mapped, IRQ registered
sudo ./tests/pcie_edu_test
sudo rmmod pcie_edu
```

This assumes the kernel you're running already has the `edu` device attached
(i.e., you're inside a VM started with `qemu-system-x86_64 ... -device edu`),
since `edu` is a QEMU-only device with no physical equivalent.

---

## Testing Strategy

**Probe verification** — confirm via `dmesg` that `probe()` runs, BAR0 maps
successfully, the MSI vector is allocated, and the DMA buffer is allocated with
a valid bus address.

**Register correctness** — read the identification register and verify it
matches the known value for the `edu` device; write to the liveness register
and confirm the readback matches the documented transform (bitwise complement).

**Interrupt-driven compute** — issue `PCIE_EDU_COMPUTE` for several values and
verify results, confirming the full path: ioctl → MMIO write → device computes
asynchronously → MSI fires → handler acks and wakes → ioctl returns correct
result.

**DMA correctness** — write a known pattern into the DMA buffer, transfer it
host → device → host, and verify the round-tripped data matches, confirming
the bus address programmed into the device's DMA registers correctly
corresponds to the buffer the CPU sees.

**Module unload safety** — `rmmod` while no operations are in flight, and
confirm `remove()` correctly frees the IRQ, unmaps BAR0, frees the DMA buffer,
and that no resources leak (checked via `/proc/iomem` and `/proc/interrupts`
before/after).

Every check above maps directly to one of the PASS/FAIL lines in
[docker/init.sh](docker/init.sh) and [tests/pcie_edu_test.c](tests/pcie_edu_test.c).

---

## Test Results

Last verified run: **2026-06-17**, kernel `6.8.0-124-generic` (Ubuntu 24.04
container), QEMU `q35` machine, via `./docker/run.sh test`. All checks passed.

| # | Check | Result |
| --- | --- | --- |
| 1 | `edu` device enumerated on the PCIe bus (`lspci -d 1234:11e8`) | PASS |
| 2 | `probe()` maps BAR0, reads ident register, allocates DMA buffer | PASS |
| 3 | MSI vector registered (`pcie_edu` visible in `/proc/interrupts`) | PASS |
| 4 | `/dev/pcie_edu` created and opens successfully | PASS |
| 5 | Liveness register round-trip (`write_val` → `~write_val`) | PASS |
| 6 | `PCIE_EDU_COMPUTE`: 0!, 1!, 5!, 10!, 12! via blocking ioctl + MSI | PASS (5/5) |
| 7 | `PCIE_EDU_DMA_TRANSFER`: 4096-byte host→device→host round trip | PASS |
| 8 | `remove()` / `rmmod`: no leaked MMIO region (`/proc/iomem` diff) | PASS |
| 9 | `remove()` / `rmmod`: no leaked IRQ line (`/proc/interrupts` diff) | PASS |

Compute results, confirming the full ioctl → MMIO write → async device
compute → MSI → wake → result-read path for each case:

| Input | Expected | Got |
| --- | --- | --- |
| `0!` | 1 | 1 |
| `1!` | 1 | 1 |
| `5!` | 120 | 120 |
| `10!` | 3,628,800 | 3,628,800 |
| `12!` | 479,001,600 | 479,001,600 |

<details>
<summary>Full annotated run log (click to expand)</summary>

```
=== lspci: edu device ===
00:01.0 Class 00ff: Device 1234:11e8 (rev 10)
        Subsystem: Device 1af4:1100
        Flags: fast devsel, IRQ 10
        Memory at fea00000 (32-bit, non-prefetchable) [size=1M]
        Capabilities: [40] MSI: Enable- Count=1/1 Maskable- 64bit+

=== insmod pcie_edu.ko ===
[    6.169200] pcie_edu: loading out-of-tree module taints kernel.
[    6.170559] pcie_edu: module verification failed: signature and/or required key missing - tainting kernel
[    6.245243] pcie_edu 0000:00:01.0: BAR0 start=0x00000000fea00000 len=0x100000
[    6.249620] pcie_edu 0000:00:01.0: ident: 0x010000ed
[    6.251853] pcie_edu 0000:00:01.0: DMA buffer: cpu=00000000708ae4db bus=0x000000000274d000
[    6.275663] pcie_edu 0000:00:01.0: /dev/pcie_edu ready (irq=24 dma=0x000000000274d000)
pcie_edu               16384  0
crw-rw-rw-    1 0        0          10, 261 Jun 18 00:04 /dev/pcie_edu

=== /proc/interrupts while loaded ===
 24:          0  PCI-MSI-0000:00:01.0   0-edge      pcie_edu
PASS: pcie_edu MSI interrupt registered

=== pcie_edu_test ===
PASS: open /dev/pcie_edu
PASS: liveness register: wrote 0x12345678, read back 0xedcba987 (~write_val)
PASS: 0! = 1
PASS: 1! = 1
PASS: 5! = 120
PASS: 10! = 3628800
PASS: 12! = 479001600
PASS: DMA round-trip (pattern=0xab len=4096)
ALL TESTS PASSED

=== rmmod pcie_edu ===
[    6.872354] pcie_edu 0000:00:01.0: removed

=== /proc/iomem delta after rmmod (expect empty) ===
PASS: /proc/iomem unchanged after rmmod
PASS: no pcie_edu entry in /proc/interrupts after rmmod
ALL TESTS PASSED
```

Notes on a couple of lines that look alarming but are expected:
- `module verification failed: ... tainting kernel` — expected for any
  out-of-tree module that isn't signed with the distro's kernel key; harmless
  for a development/test build.
- `lspci` shows `MSI: Enable-` because that snapshot is taken *before*
  `insmod`: the capability exists in config space, but the enable bit is only
  set once the driver allocates the vector via `pci_alloc_irq_vectors()`. The
  `/proc/interrupts` entry captured after `insmod` is the real confirmation
  that the MSI vector is live.

</details>

You can reproduce this run yourself with `./docker/run.sh test` — it rebuilds
the module against the container's kernel headers and re-runs every check
above from scratch.

### GCP Benchmark Results — 2026-07-12 (three runs)

Independent reproducibility check on an Ubuntu 24.04 `e2-standard-4`
(x86_64) GCP VM in `us-west1-b` — x86_64 rather than aarch64 like the other
three drivers, matching this project's x86 `q35`/`-device edu` QEMU guest.
Three independent runs of the full Docker/QEMU end-to-end harness (source
commit `2e90b55e`, host kernel `6.17.0-1020-gcp`, container kernel
`6.8.0-134-generic`, QEMU `8.2.2`).

![pcie-edu-driver GCP benchmark results](docs/test-results/2026-07-12/pcie-edu-driver-results.png)

All three runs passed every check identically — MSI registration, the
factorial ioctl compute path, the DMA round-trip, and clean `remove()` with
no leaked BAR or IRQ resources. Unlike the other three drivers, this
workload has no throughput metric to chart: `-device edu` is a fixed
synchronous compute/DMA exerciser (five factorial cases, one 4096-byte DMA
transfer), so a chart here shows the same boolean checklist passing on each
run rather than a magnitude that varies — which is itself the interesting
result: zero flakiness in interrupt registration or resource cleanup across
independent QEMU boots.

Machine-readable data: [JSON](docs/test-results/2026-07-12/pcie-edu-driver-results.json) ·
[CSV](docs/test-results/2026-07-12/pcie-edu-driver-results.csv). Regenerate the plot
with `python3 ../scripts/plot_gcp_results.py docs/test-results/2026-07-12/pcie-edu-driver-results.json`.

---

## Future Extensions

- **MSI-X with multiple vectors** — separate interrupts for compute-complete
  vs. DMA-complete, demonstrating per-event interrupt routing
- **Streaming DMA** — use `dma_map_single`/`dma_map_sg` to transfer directly
  from user-supplied buffers, with explicit `dma_sync_*` calls, and compare to
  the coherent-buffer approach
- **sysfs attributes** — expose simple register reads/writes via `sysfs` rather
  than `ioctl`, following the convention that configuration/status belongs in
  `sysfs` and data-path operations belong on the character device
- **Power management** — implement `suspend`/`resume` PCI driver callbacks
- **Real FPGA target** — port the driver to a PCIe-connected FPGA development
  board running custom RTL (potentially the mini-GPU project), closing the loop
  from RTL design to host software

---

## References

- *Linux Device Drivers, 3rd Edition* — Corbet, Rubini, Kroah-Hartman (free at
  lwn.net) — Chapter 12 covers PCI driver structure in depth
- Linux kernel documentation: `Documentation/PCI/pci.rst`,
  `Documentation/core-api/dma-api.rst`
- QEMU source tree: `hw/misc/edu.c` and `docs/specs/edu.txt` — the `edu`
  device's register map and intended use as a driver-writing exercise
- `drivers/pci/` in the Linux kernel source — real-world examples of
  `pci_driver` registration, `probe`/`remove`, and IRQ/DMA setup patterns
- [docs/edu-register-map.md](docs/edu-register-map.md) — this project's own
  verified register map and DMA addressing convention for the `edu` device
