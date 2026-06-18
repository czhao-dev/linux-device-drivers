# circbuf — Linux Character Device Driver

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Language: C](https://img.shields.io/badge/language-C-00599C.svg?logo=c&logoColor=white)](circbuf.c)
[![Platform: Linux Kernel Module](https://img.shields.io/badge/platform-Linux%20Kernel%20Module-FCC624.svg?logo=linux&logoColor=black)](circbuf.c)
[![Dev env: Docker](https://img.shields.io/badge/dev%20env-Docker-2496ED.svg?logo=docker&logoColor=white)](docker/Dockerfile)
[![Tested under: QEMU](https://img.shields.io/badge/tested%20under-QEMU-FF6600.svg?logo=qemu&logoColor=white)](docker/run.sh)

A Linux kernel module implementing a virtual circular buffer device, exposing a
concurrent, blocking I/O interface through the standard Unix file abstraction.
It covers kernel/user-space boundary management, synchronization in kernel
context, and the driver architecture patterns found in real character device
drivers.

---

## Overview

`circbuf` registers a character device at `/dev/circbuf` that behaves like a
bounded pipe: writers produce bytes into a fixed-size circular buffer, readers
consume them in FIFO order. When the buffer is full, writers block until space
is available; when it is empty, readers block until data arrives. Multiple
processes can open the device concurrently and are safely arbitrated through
kernel synchronization primitives.

The project has no hardware dependency — it is a virtual device, making it
reproducible on any Linux system. The focus is on correctness at the
kernel/user boundary, not on device-specific register programming.

---

## Architecture

```
User Space                         Kernel Space
──────────────────────────────────────────────────────────────
open("/dev/circbuf", ...)   →   circbuf_open()
read(fd, buf, n)            →   circbuf_read()    ─┐
write(fd, buf, n)           →   circbuf_write()   ─┤─→  Circular Buffer
ioctl(fd, CMD, &arg)        →   circbuf_ioctl()   ─┘    (kernel memory)
close(fd)                   →   circbuf_release()

                                 Synchronization:
                                   mutex          (buffer access)
                                   wait_queue     (blocking I/O)
```

The driver registers a `file_operations` struct — a vtable of function pointers
that the kernel calls when user processes invoke system calls on the device file.
This is C-based polymorphism: the VFS layer holds a generic interface, and
`circbuf` provides the concrete implementation.

---

## Key Concepts

### 1. Kernel / User Space Boundary

User-space virtual addresses are not directly accessible from kernel space. Even
if a user pointer appears valid, dereferencing it from the kernel is unsafe: the
user's page tables may not be mapped at the time of access, and a fault would
crash the kernel rather than raise a SIGSEGV.

`copy_to_user()` and `copy_from_user()` solve this by:
- Validating that the user pointer falls within a legal user-space range
- Handling page faults gracefully during the copy
- Returning the number of bytes not transferred, enabling partial-transfer
  recovery

```c
// Transferring data from kernel buffer to user buffer — never dereference directly
if (copy_to_user(user_buf, dev->buf + dev->read_pos, to_copy)) {
    ret = -EFAULT;
    goto out;
}
```

This boundary is fundamental to the kernel's memory protection model and appears
in every production driver.

### 2. Mutex vs Spinlock — Choosing the Right Primitive

The circular buffer is protected by a `struct mutex`, not a spinlock. The choice
is deliberate:

| Primitive  | Blocking behavior       | Safe to call schedule()? | Use case                        |
|------------|-------------------------|--------------------------|---------------------------------|
| `mutex`    | Sleeps (yields CPU)     | Yes                      | Locks held across sleeps        |
| `spinlock` | Busy-waits (burns CPU)  | No                       | IRQ handlers, very short holds  |

Because `circbuf_read()` and `circbuf_write()` call `copy_to_user()` /
`copy_from_user()`, which can trigger page faults and invoke the scheduler, the
lock must be a mutex. Holding a spinlock across a potential sleep is a kernel
invariant violation and causes deadlock or data corruption in production systems.

### 3. Blocking I/O with Wait Queues

Rather than returning `EAGAIN` when the buffer is empty or full (non-blocking
behavior), `circbuf` suspends the calling process and wakes it when the
condition changes — matching the semantics of `read(2)` on a pipe.

```c
// Reader waits until data is available or device is closed
wait_event_interruptible(dev->read_queue,
    circbuf_data_available(dev) || dev->closed);
```

`wait_event_interruptible()` adds the process to a wait queue and calls
`schedule()`, yielding the CPU. When a writer deposits data and calls
`wake_up_interruptible(&dev->read_queue)`, the kernel reschedules the waiting
reader. This is the same mechanism underlying `epoll`, pipe semantics, and
blocking socket I/O throughout the Linux kernel.

### 4. Kernel Memory Management

User-space `malloc` is not available in kernel context. Kernel allocations use:

- `kmalloc(size, GFP_KERNEL)` — physically contiguous allocation, may sleep,
  used for the circular buffer backing store
- `kzalloc` — `kmalloc` with zero-initialization, used for the device struct
- Corresponding `kfree()` in cleanup paths

`GFP_KERNEL` permits the allocator to sleep if memory pressure requires
reclamation — only safe in process context, not in interrupt handlers (which
must use `GFP_ATOMIC`).

### 5. Module Parameters and Runtime Configuration

Buffer capacity is configurable at load time without recompiling:

```bash
sudo insmod circbuf.ko buffer_size=65536
```

Implemented via `module_param()`, which registers a sysfs entry and handles
type-safe parsing. This is how production drivers expose tunable parameters
(queue depths, timeout values, DMA buffer sizes) without hardcoding policy.

### 6. ioctl Interface Design

Beyond `read`/`write`, drivers often need out-of-band control operations.
`circbuf` implements an `ioctl` interface for querying buffer state:

```c
struct circbuf_stats {
    size_t capacity;     // total buffer size in bytes
    size_t used;         // bytes currently in buffer
    size_t available;    // bytes of free space
    unsigned long reads; // total read operations since open
    unsigned long writes;
};

ioctl(fd, CIRCBUF_GET_STATS, &stats);
```

The ioctl number encodes direction (read/write), type, command, and size using
the `_IOR`/`_IOW` macros — a convention that prevents ioctl number collisions
between drivers.

---

## Design Decisions and Tradeoffs

**Single lock over the entire buffer vs reader/writer lock**

A `rwlock` would permit concurrent readers, but a circular buffer has a single
read pointer mutated by readers — concurrent readers would race on the read
pointer itself. A single mutex is correct and simpler. If throughput became a
bottleneck, the appropriate optimization would be a lock-free ring buffer using
atomic operations on head/tail indices, at the cost of significantly higher
implementation complexity.

**Blocking by default vs O_NONBLOCK**

The driver respects `O_NONBLOCK` in the `open` flags: if set, `read` and
`write` return `EAGAIN` rather than sleeping. This matches standard POSIX file
semantics and allows the device to be used with `select`/`poll`/`epoll` in
event-driven programs.

**Fixed buffer size vs dynamic resizing**

The buffer is allocated once at `open` time and freed at final `release`. Dynamic
resizing would require careful coordination with concurrent readers and writers
and is not implemented. For a production driver, the appropriate model depends on
whether the device has fixed hardware FIFO depth (fixed size correct) or is
purely software (dynamic is feasible).

---

## Building and Loading

### Prerequisites

- Linux kernel headers for your running kernel (`linux-headers-$(uname -r)`)
- `make`, `gcc`

### Build

```bash
make
```

### Load / unload

```bash
sudo insmod circbuf.ko                    # load with default 4096-byte buffer
sudo insmod circbuf.ko buffer_size=16384  # load with custom buffer size
lsmod | grep circbuf                      # verify loaded
sudo rmmod circbuf                        # unload
dmesg | tail -20                          # inspect kernel log output
```

### Device permissions

```bash
sudo chmod 666 /dev/circbuf   # allow unprivileged read/write for testing
```

---

## Usage Examples

### Shell

```bash
# Terminal 1 — write into the device
echo "hello from user space" > /dev/circbuf

# Terminal 2 — read from the device
cat /dev/circbuf

# Query buffer statistics via ioctl
./query_stats /dev/circbuf
```

### C client

```c
int fd = open("/dev/circbuf", O_RDWR);

// Write
const char *msg = "kernel boundary test";
write(fd, msg, strlen(msg));

// Read
char buf[64] = {0};
read(fd, buf, sizeof(buf) - 1);
printf("received: %s\n", buf);

// Stats
struct circbuf_stats stats;
ioctl(fd, CIRCBUF_GET_STATS, &stats);
printf("used: %zu / %zu bytes\n", stats.used, stats.capacity);

close(fd);
```

---

## Testing

macOS (the development machine for this project) can't build or load Linux
kernel modules natively, so the test harness in [`docker/run.sh`](docker/run.sh)
builds `circbuf.ko` against real kernel headers inside a Docker container, then
boots a stock Linux kernel under QEMU with a minimal busybox initramfs
([`docker/init.sh`](docker/init.sh)) to load the module and drive the test
suite end-to-end — `insmod` → tests → `rmmod`, on a real kernel, not a mock.

```bash
./docker/run.sh test
```

### Concurrent stress test

`tests/stress.c` spawns N writer threads and M reader threads operating
simultaneously on the device, verifying:

- No bytes are lost or duplicated
- No kernel panics or lockups occur
- Total bytes read equals total bytes written after all threads complete

```bash
./stress /dev/circbuf --writers=4 --readers=4 --duration=10
```

### Results — last verified run

`Linux 6.8.0-124-generic` (aarch64, QEMU `virt` machine, Ubuntu 24.04 kernel headers)

| Check | Outcome |
|---|---|
| `insmod` / `rmmod` (default 4096-byte buffer) | ✅ Pass — clean load/unload, no kernel warnings beyond the expected out-of-tree taint notice |
| `basic_test` — open → write → read → `ioctl` roundtrip | ✅ Pass — bytes written matched bytes read back exactly |
| `query_stats` — `ioctl` buffer introspection | ✅ Pass — `capacity=4096 used=0 available=4096` |
| `stress` — 4 writers / 4 readers, 5s concurrent load | ✅ Pass — `total_written=13682240` = `total_read=13682240`, zero bytes lost or duplicated |
| Reload with `buffer_size=16384` module parameter | ✅ Pass — `capacity=16384` confirmed via `ioctl` after reload |
| Kernel log (`dmesg`) | No panics, no lockups, no hangs |

```
== stress ==
device=/dev/circbuf writers=4 readers=4 duration=5s
total_written=13682240 total_read=13682240
stress: PASS
...
ALL TESTS PASSED
```

### Kernel sanitizers

Build with `CONFIG_KASAN=y` (Kernel Address Sanitizer) and
`CONFIG_LOCKDEP=y` to catch use-after-free, out-of-bounds access, and lock
ordering violations at runtime during testing.

---

## Future Extensions

- **`poll`/`select` support** — implement `circbuf_poll()` to integrate with
  event-driven I/O multiplexing
- **DMA simulation** — add a scatter-gather transfer mode to demonstrate
  `dma_alloc_coherent` and DMA mapping APIs
- **`/proc` entry** — expose buffer statistics through procfs as an alternative
  to ioctl
- **Lock-free variant** — replace mutex with atomic head/tail indices for a
  single-producer/single-consumer fast path, and benchmark the throughput
  improvement

---

## References

- *Linux Device Drivers, 3rd Edition* — Corbet, Rubini, Kroah-Hartman (free at lwn.net)
- *Linux Kernel Development, 3rd Edition* — Robert Love
- `Documentation/driver-api/` in the Linux kernel source tree
- `kernel/locking/mutex.c` — mutex implementation in the kernel source
