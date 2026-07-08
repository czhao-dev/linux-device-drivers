# netdrv — Virtual Ethernet Driver Pair with NAPI

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](../LICENSE)
[![Language: C](https://img.shields.io/badge/Language-C-A8B9CC.svg)](src/)
![Platform: Linux Kernel Module](https://img.shields.io/badge/Platform-Linux%20Kernel%20Module-FCC624.svg?logo=linux&logoColor=black)
![Scripts: Bash](https://img.shields.io/badge/Scripts-Bash-4EAA25.svg?logo=gnubash&logoColor=white)
[![Tested with Docker + QEMU](https://img.shields.io/badge/Tested%20with-Docker%20%2B%20QEMU-2496ED.svg?logo=docker&logoColor=white)](docker/)

A Linux kernel network driver implementing a software point-to-point Ethernet
pair (`vnet0` ↔ `vnet1`), structured around the same core mechanisms used by
NIC drivers: descriptor rings for TX/RX, NAPI-based interrupt-mitigated packet
reception, and TX queue backpressure. The "wire" between the two interfaces is
software, while the driver-side mechanics follow the real networking stack
contracts.

---

## Verified

Run end-to-end via the [Docker + QEMU harness](#docker-end-to-end-test)
against a real, version-matched Ubuntu kernel (`6.8.0-124-generic`, aarch64)
— not just compiled, but actually `insmod`'d and exercised:

| Check | Result |
|---|---|
| Module load & interface registration (`vnet0`, `vnet1`) | ✅ |
| Carrier state on open/stop | ✅ |
| `ping` across the netns pair | ✅ 4/4, 0% loss |
| `tx_packets` / `rx_packets` symmetry | ✅ 11 == 11 |
| Module unload while interfaces are up, clean `dmesg` | ✅ |
| Flood ping with `ring_size=4` | ✅ 2000/2000, 0% loss |
| Backpressure under concurrent `iperf3` UDP load | ✅ `tx_queue_stops`/`wakes` = 2, 0 packets lost (59670 == 59670) |
| 10x load/unload loop, clean `dmesg` | ✅ |

```text
==============================================
NETDRV_E2E_RESULT: PASS
==============================================
```

Reproduce in one command (no Linux host or VM setup required):

```bash
docker build -f docker/Dockerfile -t netdrv-e2e . && docker run --rm netdrv-e2e
```

This is also how a real bug was caught during development: `IFF_NOARP` made
the driver's own ring counters look healthy while `ping` silently failed
100% of the time. See [Key Concepts §1](#1-net_device-and-net_device_ops) and
[Docker end-to-end test](#docker-end-to-end-test) for details.

---

## Repository Layout

```text
.
├── src/            # kernel module source — netdrv.c, netdrv.h
├── scripts/        # netns setup/teardown/smoke-test helpers (require root)
├── docker/         # Docker + QEMU end-to-end test harness
├── Makefile        # kbuild wrapper (make / make clean)
└── README.md
```

---

## Overview

Network drivers sit on top of the generic Linux device-driver model but plug
into a distinct kernel subsystem: the networking stack. That subsystem has its
own core object (`struct net_device`), its own packet buffer type
(`struct sk_buff`), and its own interrupt-mitigation strategy (NAPI). NAPI
exists because packet rates can be high enough that interrupt-per-packet would
saturate the CPU before any packet is actually processed.

`netdrv` creates two virtual Ethernet interfaces, `vnet0` and `vnet1`, that
behave like a back-to-back cable: a packet transmitted on one appears as
received on the other. Internally, each interface has its own TX and RX
descriptor rings and its own NAPI context — the same structures a real NIC
driver maintains for hardware descriptor rings — but the "DMA" between them is
just a pointer handoff in software. This isolates the driver-side logic (ring
management, NAPI polling, backpressure) from the hardware-transport details,
which keeps the focus on the kernel networking mechanics shared by Ethernet,
virtio-net, and accelerator NIC drivers.

### Why this problem

Networking has its own driver contracts and performance constraints.
`net_device`, `sk_buff`, queue state, carrier state, and NAPI all have specific
rules that differ from character devices or generic PCIe examples. This module
keeps the hardware transport intentionally simple so those networking-specific
contracts are visible and testable. It also provides a useful baseline for
understanding why user-space networking frameworks such as DPDK and AF_XDP
bypass parts of the kernel networking path.

---

## Architecture

```
         vnet0                                         vnet1
 ┌──────────────────────┐                      ┌──────────────────────┐
 │ Network Stack        │                      │ Network Stack        │
 │ (IP, sockets, etc.)  │                      │ (IP, sockets, etc.)  │
 └──────────┬───────────┘                      └──────────┬───────────┘
            │ ndo_start_xmit(skb)                         │ netif_receive_skb(skb)
            ▼                                             ▲
 ┌──────────────────────┐                      ┌──────────────────────┐
 │ TX ring (vnet0)      │                      │ RX ring (vnet1)      │
 │ [desc][desc][desc]...│── skb handoff ──►    │ [desc][desc][desc]...│
 └──────────────────────┘ (software "wire")    └──────────┬───────────┘
                                                          │ tasklet_schedule
                                                          ▼ (simulated IRQ)
                                               ┌──────────────────────┐
                                               │ NAPI poll (vnet1)    │
                                               │ drains RX ring,      │
                                               │ netif_receive_skb()  │
                                               └──────────────────────┘

 Symmetric in the other direction: vnet1's TX ring feeds vnet0's RX ring.
```

Each interface's `net_device_ops` and NAPI context are independent — `vnet0`
and `vnet1` are peers of each other via a stored pointer, exactly as `veth`
pairs work in the real kernel.

---

## Key Concepts

### 1. `net_device` and `net_device_ops`

`struct net_device` is the networking subsystem's equivalent of the `file`
abstraction — every network interface the kernel knows about (`eth0`, `lo`,
`wlan0`, `vnet0`) is one of these. `net_device_ops` is its vtable:

```c
static const struct net_device_ops netdrv_netdev_ops = {
    .ndo_open       = netdrv_open,
    .ndo_stop       = netdrv_stop,
    .ndo_start_xmit = netdrv_start_xmit,
    .ndo_get_stats64 = netdrv_get_stats64,
};

static void netdrv_setup(struct net_device *dev)
{
    ether_setup(dev);                      // standard Ethernet defaults
    dev->netdev_ops = &netdrv_netdev_ops;
    dev->mtu = ETH_DATA_LEN;
}
```

ARP is deliberately left enabled, even though this is a point-to-point pair.
`ether_setup` makes `vnet0`/`vnet1` full `ARPHRD_ETHER` devices with their own
random MAC (`eth_hw_addr_random`), so unicast IP delivery depends on the
kernel's neighbor table resolving the peer's real MAC the normal way, via ARP
request/reply frames carried over the same ring/NAPI path as any other
Ethernet frame. Setting `IFF_NOARP` here was tried during development and
breaks unicast routing: with no ARP resolution, the neighbour code can't fill
in a usable destination MAC, so outgoing IP packets reach the peer's RX ring
but `eth_type_trans()` classifies them `PACKET_OTHERHOST` and `ip_rcv()`
drops them silently — the driver's own counters look fine while `ping` fails
100% of the time. `veth` doesn't set `IFF_NOARP` for the same reason.

`ndo_open`/`ndo_stop` correspond to `ip link set dev up/down` and are where
NAPI is enabled/disabled (`napi_enable`/`napi_disable`) and the carrier state is
set (`netif_carrier_on`/`off`).

### 2. `sk_buff` — the Packet Buffer

Every packet in the Linux networking stack is an `sk_buff` ("skb") — a buffer
with headroom and tailroom for protocol headers to be added/removed as the
packet moves through layers, plus metadata (protocol type, device, checksum
status, reference count). Drivers don't allocate the initial skb for TX — the
stack does — but RX-side drivers allocate skbs to hand received data up:

```c
struct sk_buff *skb = netdev_alloc_skb(dev, length + NET_IP_ALIGN);
skb_reserve(skb, NET_IP_ALIGN);   // align IP header for performance
skb_put(skb, length);             // mark `length` bytes as containing data
memcpy(skb->data, payload, length);
skb->protocol = eth_type_trans(skb, dev);  // parse Ethernet header, set protocol
```

`NET_IP_ALIGN` exists because some architectures fault or take a performance
penalty on misaligned access to the IP header that follows the 14-byte Ethernet
header.

### 3. TX Path and Queue Backpressure

`ndo_start_xmit` is called by the stack to hand a packet to the driver. In
`netdrv`, it places the skb into the peer's RX ring:

```c
static netdev_tx_t netdrv_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
    struct netdrv_priv *priv = netdev_priv(dev);
    struct netdrv_priv *peer = netdev_priv(priv->peer);

    if (ring_full(peer->rx_head, peer->rx_tail)) {
        netif_stop_queue(dev);     // tell the stack: don't send more yet
        return NETDEV_TX_BUSY;
    }

    struct sk_buff *rx_skb = skb_clone(skb, GFP_ATOMIC);
    peer->rx_ring[peer->rx_head % RING_SIZE] = rx_skb;
    peer->rx_head++;

    priv->stats.tx_packets++;
    priv->stats.tx_bytes += skb->len;
    dev_kfree_skb(skb);

    tasklet_schedule(&peer->rx_tasklet);   // simulate "packet arrived" IRQ
    return NETDEV_TX_OK;
}
```

**Backpressure** (`netif_stop_queue` / `netif_wake_queue`) is the mechanism by
which a driver tells the stack "my TX ring is full, stop sending until I say
otherwise." Without this, a slow or full hardware ring would force the driver
to either drop packets silently or block in `ndo_start_xmit` (which is not
allowed — it runs with interrupts effectively disabled from the stack's
perspective). The wake call happens later, from the NAPI poll function once the
peer's ring has drained.

### 4. NAPI — Interrupt-Mitigated RX

A real NIC raises an interrupt for each received packet (or batch). At high
packet rates, interrupt-per-packet overhead alone can consume all available CPU
— a phenomenon called **receive livelock**, where the system spends 100% of its
time handling interrupts and never makes progress on actually processing
packets. NAPI solves this: the interrupt handler does the minimum possible
(disable further RX interrupts, schedule a poll), and a `poll()` function —
running in softirq context, not hardirq — drains packets in a bounded batch
("budget"). If the ring is drained within budget, interrupts are re-enabled; if
not, polling continues without re-enabling interrupts, naturally adapting to
load.

```c
static int netdrv_poll(struct napi_struct *napi, int budget)
{
    struct netdrv_priv *priv = container_of(napi, struct netdrv_priv, napi);
    int work_done = 0;

    while (work_done < budget && priv->rx_tail != priv->rx_head) {
        struct sk_buff *skb = priv->rx_ring[priv->rx_tail % RING_SIZE];
        priv->rx_tail++;

        skb->protocol = eth_type_trans(skb, priv->dev);
        priv->stats.rx_packets++;
        priv->stats.rx_bytes += skb->len;
        netif_receive_skb(skb);
        work_done++;
    }

    if (work_done < budget)
        napi_complete_done(napi, work_done);  // ring empty: re-enable "interrupts"

    if (netif_queue_stopped(priv->peer))       // peer's TX was paused on us — resume it
        netif_wake_queue(priv->peer);

    return work_done;
}
```

The "simulated interrupt" — a tasklet scheduled from the peer's TX path —
calls `napi_schedule()`, which is the software equivalent of a real driver's
hardirq handler calling `napi_schedule()` after acking the hardware interrupt.
Everything downstream of that call is identical to a real driver.

### 5. Carrier State

`netif_carrier_on(dev)` / `netif_carrier_off(dev)` tell the stack whether the
link is "up" at the physical layer — distinct from the administrative
up/down state (`ifconfig up/down`). A real NIC driver calls this based on PHY
link-detect signals; `netdrv` calls `carrier_on` in `ndo_open` (both ends of
the virtual cable are always "connected") and `carrier_off` in `ndo_stop`.

---

## Design Decisions and Tradeoffs

**`skb_clone` vs. direct ownership transfer**

When `vnet0`'s TX hands a packet to `vnet1`'s RX ring, `netdrv` clones the skb
rather than transferring the original. Cloning is slightly more expensive (an
extra allocation for the skb header, though the data buffer itself is
reference-counted and shared) but keeps ownership unambiguous — `vnet0`
immediately frees its copy via `dev_kfree_skb`, and `vnet1` owns the clone
independently. Transferring ownership directly would avoid the clone but
requires careful auditing that no code path on the TX side touches the skb
after handoff — a correctness hazard for a relatively small performance gain in
a driver that isn't on a real performance-critical path. Real NIC drivers don't
face this choice the same way, since TX and RX sides are physically separate
hardware rings with their own buffers; this tradeoff is specific to the
software-pair design.

**Tasklet vs. workqueue for the simulated interrupt**

`tasklet_schedule` runs in softirq context, closely mirroring how a real
hardirq handler's `napi_schedule()` call leads into softirq-context polling. A
workqueue would run in process context, which is a looser approximation of the
real interrupt-to-poll transition. Tasklets are used here specifically to keep
the context model faithful to real hardware drivers, even though tasklets are
considered somewhat legacy in newer kernel code (threaded IRQs and workqueues
are often preferred for new drivers).

**Fixed-size ring buffers vs. dynamic queueing**

Both TX and RX rings are fixed-size circular buffers (`RING_SIZE` descriptors),
matching how real NIC descriptor rings work — hardware rings are physically
fixed in size. An alternative would be an unbounded software queue (e.g., a
linked list), which would never need backpressure — but that would mean the
driver doesn't exercise `netif_stop_queue`/`wake_queue` at all, which is one of
the more important and frequently-misunderstood mechanisms in driver-stack
interaction. The fixed ring is deliberately chosen to force this mechanism to
matter.

**No checksum or segmentation offload (initially)**

Modern NICs offload checksum calculation (`NETIF_F_HW_CSUM`) and segmentation
(`NETIF_F_TSO`, GSO/GRO) to hardware, advertised via `dev->features`. `netdrv`
advertises none of these initially — the stack computes checksums and segments
packets in software, which is correct but slower. This keeps the initial
implementation focused on the ring/NAPI/backpressure mechanics; offload flags
are a natural, well-scoped extension once the base driver works (see Future
Extensions).

---

## Building and Testing

### Build and load

```bash
make
insmod src/netdrv.ko
ip link show               # vnet0 and vnet1 should appear, state DOWN
dmesg | tail                # confirm both interfaces registered
```

### Bring up and connect

```bash
# Put vnet1 in its own network namespace, like a container's interface
ip netns add ns1
ip link set vnet1 netns ns1

ip addr add 10.0.0.1/24 dev vnet0
ip link set vnet0 up

ip netns exec ns1 ip addr add 10.0.0.2/24 dev vnet1
ip netns exec ns1 ip link set vnet1 up
```

### Basic connectivity

```bash
ping -c 4 10.0.0.2
```

A successful ping confirms the full path: `ndo_start_xmit` on `vnet0` →
ring handoff → tasklet → NAPI schedule on `vnet1` → `netdrv_poll` →
`netif_receive_skb` → IP/ICMP stack in `ns1` → reply along the reverse path.

### Throughput and NAPI behavior

```bash
# In ns1:
ip netns exec ns1 iperf3 -s &
# On host:
iperf3 -c 10.0.0.2

# Observe NAPI poll activity:
cat /proc/net/softnet_stat
```

### Backpressure

Shrinking `ring_size` to a small value (e.g., 4) makes `netif_stop_queue`/
`wake_queue` transitions observable via a debug counter (exposed through
`ethtool -S vnet0`) — confirming the stack correctly pauses and resumes
transmission as the ring fills and drains.

A single-threaded `ping -f` is not enough to trigger this in practice: on
this loopback-style virtual link the round trip is fast enough that NAPI
drains the ring before the next packet is even sent, so the ring rarely
builds depth and `tx_queue_stops` stays at 0. Backpressure needs concurrent
producers that can outrun the consumer — e.g. `iperf3` with several parallel
UDP streams at an unbounded rate:

```bash
ip netns exec ns1 iperf3 -s -D
iperf3 -c 10.0.0.2 -u -b 0 -P 4 -t 5
ethtool -S vnet0   # tx_queue_stops / tx_queue_wakes should be > 0
```

### Custom statistics

```bash
ethtool -S vnet0
#   tx_packets: 1042
#   rx_packets: 1042
#   tx_queue_stops: 3
```

### Helper scripts

`scripts/` automates the manual steps above (all require root, for
`insmod`/`ip netns`):

```bash
sudo scripts/setup_netns.sh [ring_size]      # load module, create ns1,
                                              # move vnet1 in, assign IPs, bring up
sudo scripts/teardown_netns.sh [--unload]    # delete ns1 (vnet1 moves back
                                              # to init netns automatically),
                                              # optionally rmmod
sudo scripts/smoke_test.sh [ring_size]       # build, load, configure,
                                              # ping, print stats, tear down
```

`smoke_test.sh` exits non-zero if the build, load, or ping fails, and
always tears down (module + namespace) on exit via a trap, making it
suitable for repeated load/unload-loop testing:

```bash
for i in $(seq 1 20); do
  sudo scripts/smoke_test.sh || { echo "FAILED on iteration $i"; break; }
done
```

### Docker end-to-end test

Loading a kernel module requires a matching real kernel, which a plain Docker
container does not provide — containers share the host kernel, and on macOS
that's Docker Desktop's LinuxKit VM kernel, for which no headers package
exists. `docker/` works around this by building the module against a real,
version-matched Ubuntu kernel and booting that exact kernel under QEMU
*inside* the container. This still boots a VM (QEMU), but it's a self-contained
one the Dockerfile builds and runs for you — no separate VM, kernel build, or
host setup beyond Docker itself:

```bash
docker build -f docker/Dockerfile -t netdrv-e2e .
docker run --rm netdrv-e2e
```

`docker/init.sh` runs as PID 1 in the guest and exercises the driver for
real: module load and interface registration, carrier state on open/stop,
`ping` across the `vnet0`/`vnet1` netns pair, `tx_packets`/`rx_packets`
symmetry, backpressure under concurrent `iperf3` UDP load (verifying
`tx_queue_stops` increments and no packets are lost), and a 10x load/unload
loop checked against `dmesg` for warnings. The container exits 0 if every
check passes, 1 if any check fails, 2 if the guest never reported a result
(boot failure, crash, or timeout).

This is how the `IFF_NOARP` bug described above was actually caught: the
driver's own ring counters looked healthy, but real `ping` traffic through a
real kernel failed 100% of the time, which a build-only check would never
have revealed. As of this writing, a clean run passes all checks.

---

## Testing Strategy

**Interface registration** — confirm `vnet0`/`vnet1` appear correctly with
`ip link show` and the correct MTU.

**Connectivity** — `ping` across the pair, in both directions, confirms the
full TX→ring→NAPI→RX path works symmetrically.

**Throughput correctness** — `iperf3` over the pair; verify reported throughput
is sane and that `tx_packets`/`rx_packets` on both sides match (no silent
drops under normal load).

**Backpressure correctness** — with a small ring size and concurrent traffic
(e.g. `iperf3 -u -b 0 -P 4`; a single-stream `ping -f` round-trips too fast
to build ring depth on this link), verify via `ethtool -S` that
`tx_queue_stops` increments and that no packets are lost (`tx_packets` on one
side still equals `rx_packets` on the other, just with pauses) — this is the
test that actually exercises `netif_stop_queue`/`wake_queue`.

**Module unload safety** — bring interfaces down, `rmmod`, confirm via `dmesg`
that `ndo_stop` and NAPI teardown (`napi_disable`) ran cleanly, and that no
tasklets or skbs are leaked (checkable by tracking allocation/free counts and
asserting they match at unload).

---

## Relationship to Broader Work

This project extends the driver-development theme into the networking
subsystem specifically — a different core object model (`net_device`/`sk_buff`)
from the character device and PCIe projects, but built on the same underlying
discipline: understand the contract between the kernel and the driver, and get
the context rules right (NAPI's softirq-context poll function has the same
"what can and can't happen here" reasoning as the PCIe driver's hardirq
handler, just with different rules).

The ring buffer design is also a producer/consumer structure. Here the
producers and consumers are kernel subsystems (the stack and the driver) rather
than application threads, and synchronization is driven by NAPI scheduling plus
queue stop/wake instead of atomics.

The project also demonstrates the kernel-level mechanisms that user-space
networking frameworks like DPDK and AF_XDP are designed to bypass. Understanding
the kernel path makes it easier to reason about what those frameworks remove,
what costs they avoid, and what responsibilities they move into user space.

---

## Future Extensions

- **Checksum/segmentation offload flags** — advertise `NETIF_F_HW_CSUM` and
  implement the corresponding skb field handling, and `NETIF_F_TSO`/GSO for
  large-packet segmentation
- **Multi-queue support** — multiple TX/RX ring pairs with multiple NAPI
  contexts, modeling how modern multi-core NICs distribute load (`ndo_select_queue`,
  per-CPU NAPI instances)
- **XDP hook** — implement a minimal `ndo_bpf` to demonstrate where an XDP
  program would attach in the RX path, even without full XDP semantics
- **GRO** — replace `netif_receive_skb` with `napi_gro_receive` and coalesce
  consecutive small packets, measuring the throughput effect
- **Backing with real DMA** — connect the ring "wire" to the PCIe `edu` device's
  DMA engine from the PCIe driver project, so packet data actually moves through
  a (emulated) hardware DMA path rather than a software pointer handoff

---

## References

- Rosen, R. *Linux Kernel Networking: Implementation and Theory* — the standard
  reference for `net_device`, `sk_buff`, and the RX/TX paths
- Mogul, J. & Ramakrishnan, K.K. *Eliminating Receive Livelock in an
  Interrupt-Driven Kernel* (USENIX 1996) — the paper motivating NAPI's design
- `Documentation/networking/napi.rst` in the Linux kernel source
- `drivers/net/dummy.c` and `drivers/net/veth.c` in the Linux kernel source —
  small, readable reference drivers; `veth.c` in particular is the real-world
  version of the paired-interface design used here
