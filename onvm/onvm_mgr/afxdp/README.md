# AF_XDP Manager

## Table of Contents
1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Dependencies](#dependencies)
4. [File Structure and Workflow](#file-structure-and-workflow)
5. [Function Reference](#function-reference)
6. [Configuration Options](#configuration-options)
7. [Building and Compilation](#building-and-compilation)
8. [Usage Examples](#usage-examples)
9. [Testing Methods](#testing-methods)

---

## Overview

The AF_XDP (Address Family XDP) manager is a high-performance packet I/O implementation for the openNetVM NF Manager. It replaces the traditional DPDK datapath with a Linux kernel-native AF_XDP socket interface, providing:

- **Direct NIC-to-userspace packet transfer** via shared UMEM, bypassing the kernel network stack
- **XDP (eXpress Data Path)** eBPF program for packet steering at the NIC driver level
- **Decoupled multi-threaded pipeline**: dedicated RX, TX, and per-NF threads each pinned to their own CPU core
- **Lock-free inter-thread communication** via DPDK `rte_ring` (MPSC) queues
- **Hugepage-backed UMEM** for maximizing memory bus efficiency

### Key Concept
The manager is designed exclusively around the **Chained NF mode**, where packets flow through a pipeline of NF handlers before being transmitted back out. An optional legacy bounce mode exists purely for debugging the base AF_XDP socket path.

**End-to-End Chained NF Architecture Flow:**
```
        ┌─────────────────────────────────────────────────────────────────┐
        │                     UMEM (Hugepage-backed)                      │
        │            65536 frames × 4096 bytes = 256 MB total             │
        └──────────────────────────────────────────────┬──────────────────┘
                                                       │ zero-copy frame references
  NIC                                      Kernel / Userspace Boundary
  ───                                      ─────────────────────────────
  [NIC RX Queue] ──→ [XDP eBPF Program] ──→ [AF_XDP RX Ring (8192)]
                      (af_xdp_kern.o)                  │
                                                       │ RX Thread (Core 1)
                                                       │ afxdp_handle_receive()
                                                       │ alloc pkt_holder
                                                       ▼
                                          [NF 0 RX rte_ring (8192)]
                                                       │
                                             NF 0 Thread (Core 3)
                                             simple_forward → ACTION_NEXT
                                                       │
                                          [NF 0 TX rte_ring (8192)]
                                                       │
                                              TX Thread (Core 2)
                                              routes to next NF
                                                       │
                                          [NF 1 RX rte_ring (8192)]
                                                       │
                                             NF 1 Thread (Core 4)
                                             simple_forward → ACTION_NEXT
                                                       │
                                          [NF 1 TX rte_ring (8192)]
                                                       │
                                              TX Thread (Core 2)
                                              afxdp_submit_egress()
                                                       │
                                            [AF_XDP TX Ring (8192)]
                                                       │
                                            [NIC TX] → back to wire
```

**Packet Holder Structure (metadata wrapper):**
```c
struct afxdp_pkt_holder {
    struct afxdp_nf_desc  desc;   // umem_addr (UMEM offset) + len
    struct afxdp_pkt_meta meta;   // action, chain_index, destination, flags
};
```

**NF Actions:** `DROP` (0), `NEXT` (1), `TONF` (2), `OUT` (3)

---

## Architecture

### Threading Model

The manager runs **6 concurrent threads** (for a 2-NF chain), each pinned to a dedicated CPU core using `pthread_setaffinity_np()`:

| Thread | CPU Core | Role |
|--------|----------|------|
| **RX Thread** | Core 1 | Polls AF_XDP RX ring, allocates UMEM frames, wraps packets in `afxdp_pkt_holder`, enqueues to NF 0's RX ring |
| **TX Thread** | Core 2 | Drains each NF's TX ring in chain order, routes by action, submits egress to AF_XDP TX ring, drains Completion ring |
| **NF 0 Thread** | Core 3 | Dequeues from NF 0 RX ring, runs NF handler (`simple_forward`), enqueues to NF 0 TX ring |
| **NF 1 Thread** | Core 4 | Dequeues from NF 1 RX ring, runs NF handler (`simple_forward`), enqueues to NF 1 TX ring |
| **Manager Thread** | (floats) | Prints periodic statistics, handles TTL/pkt-limit shutdown |
| **Wakeup Thread** | (floats) | Ensures graceful SIGINT/SIGTERM response even during busy-wait |

Thread affinity is set immediately at thread startup (before the main loop), ensuring no scheduler migrations after initialization.

### UMEM Architecture

The UMEM is a single contiguous hugepage-backed memory region allocated via `mmap(MAP_HUGETLB)`:

```
 UMEM Buffer Layout (264192 KB total)
 ┌──────────────────────────────────────────────────────┐
 │  UMEM Frames                                         │
 │  65536 × 4096 bytes = 262144 KB                      │
 │  (each frame holds one packet, max MTU 4096 bytes)   │
 ├──────────────────────────────────────────────────────┤
 │  Holder Pool (embedded, tail of UMEM buffer)         │
 │  65536 × 24 bytes = 1536 KB                          │
 │  (one afxdp_pkt_holder metadata struct per frame)    │
 └──────────────────────────────────────────────────────┘
```

**Frame Allocator**: A thread-safe DPDK `rte_ring` (`RING_F_SC_DEQ`) free-list stores UMEM frame addresses. Only the RX thread dequeues (single-consumer); both the RX thread and TX thread enqueue freed frames back (multi-producer).

**Holder Pool Free-list**: Also a `rte_ring` (`RING_F_SC_DEQ`) — the RX thread allocates holders; the TX thread and NF threads return them.

### Inter-NF Ring Backend

The default and actively used ring backend is **DPDK `rte_ring`** (`AFXDP_DEFAULT_RING_BACKEND = AFXDP_RING_BACKEND_RTE`). Each NF has two dedicated `rte_ring` queues:

- **`nf<N>_rx`** (`RING_F_SC_DEQ`): Single-consumer (the NF thread dequeues)
- **`nf<N>_tx`** (`RING_F_SC_DEQ | RING_F_SP_ENQ`): The NF thread enqueues; the TX thread dequeues

A custom lockfree SPSC ring implementation exists in `onvm_afxdp_ring.h/c` as a fallback (`AFXDP_RING_BACKEND_CUSTOM`), but is not used in the default production configuration.

### AF_XDP Ring Structure (Kernel ↔ Userspace)

Four rings form the kernel-userspace interface per AF_XDP socket:

| Ring | Direction | Purpose |
|------|-----------|---------|
| **Fill Ring** | User → Kernel | Userspace provides empty UMEM frames for the kernel to fill with received packets |
| **RX Ring** | Kernel → User | Kernel deposits received packet descriptors here |
| **TX Ring** | User → Kernel | Userspace deposits outgoing packet descriptors here |
| **Completion Ring** | Kernel → User | Kernel notifies userspace when TX frames have been transmitted |

All four rings are sized at **8192** descriptors.

### Components

1. **XDP Kernel Program (`af_xdp_kern.c`)**
   - eBPF program loaded onto the NIC's XDP hook
   - Inspects RX queue index and redirects packets to the registered AF_XDP socket via `bpf_redirect_map()`
   - Falls through to `XDP_PASS` if no socket is registered for a given queue

2. **Userspace Manager (`onvm_afxdp.c`)**
   - Configures UMEM (hugepage-backed shared packet buffer)
   - Creates and manages AF_XDP sockets and all four rings
   - Launches and coordinates all worker threads
   - Implements the RX polling loop (`afxdp_handle_receive`) and TX egress path (`afxdp_submit_egress`)

3. **Chain Manager (`onvm_afxdp_chain.h/c`)**
   - Initializes per-NF `rte_ring` queues and the holder pool
   - Loads NF handlers from the NF registry by name string (e.g., `"simple_forward"`)
   - Manages chain teardown

4. **NF Registry (`onvm_afxdp_nf_registry.h/c`)**
   - Maintains a table of registered NF types by name
   - `afxdp_chain_init_from_spec("simple_forward,simple_forward", ...)` looks up handlers here

5. **Type Definitions (`onvm_afxdp_types.h`)**
   - Data structures for UMEM, sockets, stats, configuration, NF chain, and packet holders

6. **Configuration (`onvm_afxdp_config.h`)**
   - All tunable parameters (ring sizes, UMEM frames, batch sizes, thread counts, etc.)

### Key Data Structures

#### UMEM
```c
struct afxdp_umem_info {
    struct xsk_ring_prod fq;      // Fill ring  (user → kernel: empty frames)
    struct xsk_ring_cons cq;      // Completion ring (kernel → user: done TX frames)
    struct xsk_umem *umem;        // libxdp UMEM handle
    void *buffer;                 // Raw mmap(MAP_HUGETLB) memory region
};
```

#### XSK Socket
```c
struct afxdp_socket_info {
    struct xsk_ring_cons rx;         // RX ring (kernel → user)
    struct xsk_ring_prod tx;         // TX ring (user → kernel)
    struct afxdp_umem_info *umem;    // Shared UMEM reference
    struct xsk_socket *xsk;          // libxdp socket handle
    struct rte_ring *umem_frame_ring;// Thread-safe UMEM frame free-list (rte_ring)
    uint32_t outstanding_tx;         // Pending TX completions
    struct afxdp_stats_record stats; // Live statistics (rx_pkts, tx_pkts, rx_dropped, tx_dropped)
};
```

#### Manager Context
```c
struct afxdp_manager_ctx {
    struct afxdp_config cfg;              // Runtime configuration (parsed from CLI)
    struct afxdp_umem_info *umem;         // UMEM region
    struct afxdp_socket_info *xsk_socket; // Primary AF_XDP socket
    struct xdp_program *xdp_prog;         // XDP program handle
    int xsk_map_fd;                       // XSKMAP file descriptor
    struct afxdp_chain_ctx *chain;        // NF chain context (rings, holders, NF threads)
    volatile bool global_exit;            // Shutdown flag (set by SIGINT/SIGTERM)
    bool hugepage_preallocated;           // Whether UMEM buffer was pre-allocated
};
```

---

## Dependencies

### System Requirements
- **Linux Kernel**: >= 5.3 (recommended >= 5.11 for improved AF_XDP support)
- **CPU**: x86_64 with at least 5 available CPU cores (1 per datapath thread + OS)
- **RAM**: >= 1 GB hugepages configured (150 × 2MB pages recommended for testing)
- **NIC**: Any XDP-capable NIC; `virtio_net` works via XDP generic/copy mode

### Required Libraries

1. **DPDK** (>= 21.x)
   - Required for `rte_ring`, `rte_eal`, and hugepage management
   - Build DPDK and export `RTE_SDK` and `RTE_TARGET`

2. **libbpf** (>= 0.3.0)
   - BPF program loading and management
   - Install: `sudo apt-get install libbpf-dev`

3. **libxdp** (>= 1.0.0)
   - High-level AF_XDP socket API
   - Install: `sudo apt-get install libxdp-dev`
   - Source: https://github.com/xdp-project/xdp-tools

4. **Linux Kernel Headers**
   - Required for BPF and XDP definitions
   - Install: `sudo apt-get install linux-headers-$(uname -r)`

5. **clang** (>= 10.0)
   - BPF bytecode compiler for `af_xdp_kern.c`
   - Install: `sudo apt-get install clang llvm`

### Kernel Configuration
Ensure the following kernel config options are enabled:
```bash
CONFIG_BPF=y
CONFIG_BPF_SYSCALL=y
CONFIG_XDP_SOCKETS=y
CONFIG_BPF_JIT=y
CONFIG_HUGETLBFS=y
```

Verify with:
```bash
zgrep -E 'CONFIG_BPF|CONFIG_XDP|CONFIG_HUGE' /proc/config.gz
```

---

## File Structure and Workflow

### Files Overview

| File | Purpose | Compiled |
|------|---------|----------|
| `af_xdp_kern.c` | XDP eBPF kernel program (packet steering) | Yes (BPF bytecode) |
| `onvm_afxdp.c` | Main AF_XDP manager: UMEM, sockets, RX/TX loops, thread launch | Yes (userspace) |
| `onvm_afxdp.h` | Public API declarations | No (header) |
| `onvm_afxdp_types.h` | Type definitions (UMEM, sockets, chain, stats) | No (header) |
| `onvm_afxdp_config.h` | All tunable constants (ring sizes, pool sizes, thread counts) | No (header) |
| `onvm_afxdp_ring.h/c` | Custom lockfree SPSC ring (backup; not used by default) | Yes (userspace) |
| `onvm_afxdp_chain.h/c` | NF chain management (ring init, packet routing, teardown) | Yes (userspace) |
| `onvm_afxdp_nf_registry.h/c` | NF type registry (maps name string → handler struct) | Yes (userspace) |
| `nfs/afxdp_simple_forward.h/c` | `simple_forward` NF: sets `ACTION_NEXT` on every packet | Yes (userspace) |
| `Makefile` | Builds `af_xdp_kern.o` (eBPF bytecode) | No |

### Workflow by File

#### 1. `af_xdp_kern.c` - XDP Kernel Program

**Workflow:**
```
1. Packet arrives at NIC
2. NIC driver invokes XDP hook
3. xdp_sock_prog(ctx) is called with packet context
4. Extract RX queue index from ctx->rx_queue_index
5. Lookup socket FD in xsks_map[queue_index]
6. If socket exists:
   → bpf_redirect_map() → AF_XDP socket (packet deposited in RX ring)
7. Else:
   → XDP_PASS (continue to normal kernel stack)
```

**BPF Maps:**
```c
// XSKMAP: RX queue index → AF_XDP socket fd
xsks_map: BPF_MAP_TYPE_XSKMAP[64]

// Statistics: RX queue index → packet count (per-CPU)
xdp_stats_map: BPF_MAP_TYPE_PERCPU_ARRAY[64]
```

---

#### 2. `onvm_afxdp.c` - Userspace Manager

**Initialization Workflow (`afxdp_init`):**
```
1.  Parse CLI arguments (interface, queue, XDP mode, NF chain spec)
2.  Install signal handlers (SIGINT, SIGTERM → set global_exit)
3.  Pre-allocate UMEM hugepage buffer via mmap(MAP_HUGETLB)
4.  Initialize DPDK EAL (rte_eal_init) for rte_ring support
5.  Load XDP kernel program from af_xdp_kern.o (libxdp)
6.  Attach XDP program to network interface
7.  Find and open xsks_map BPF map
8.  Configure UMEM with Fill/Completion rings (xsk_umem__create)
9.  Create AF_XDP socket bound to (interface, queue)
10. Insert socket FD into xsks_map[queue]
11. Initialize UMEM frame free-list (rte_ring)
12. Pre-fill Fill ring with empty UMEM frames
13. Initialize NF chain (afxdp_chain_init_from_spec):
    - Create per-NF rte_ring queues (rx_ring, tx_ring)
    - Embed holder pool in tail of UMEM buffer
    - Look up NF handlers from NF registry
14. Launch all worker threads (with CPU core affinity):
    - RX thread → Core 1
    - TX thread → Core 2
    - NF 0 thread → Core 3
    - NF N thread → Core 3+N
    - Manager/Stats thread
    - Wakeup thread
```

**RX Thread Runtime (`afxdp_handle_receive` — Core 1):**
```
Per polling iteration:
1. Peek XSK RX ring for arrived packet descriptors (burst up to 256)
2. Refill Fill ring with free UMEM frames (from frame free-list)
3. For each received packet descriptor:
   a. Allocate one afxdp_pkt_holder from holder pool
   b. Populate holder: desc.umem_addr, desc.len, meta.action = NEXT
   c. rte_ring_enqueue() → NF 0's rx_ring
   d. If ring full: free holder + UMEM frame (ingress drop, stats.rx_dropped++)
4. xsk_ring_cons__release() on RX ring
5. Update stats.rx_packets
```

**TX Thread Runtime (`afxdp_tx_thread_main` — Core 2):**
```
Per polling iteration:
1. afxdp_drain_cq(): Drain Completion ring to reclaim TX UMEM frames (no syscall)
2. For each NF in chain order:
   a. rte_ring_dequeue_burst() up to 256 holders from nf<N>_tx
   b. For each holder:
      - ACTION_NEXT: route to next NF's rx_ring
      - ACTION_OUT: collect in egress batch
      - ACTION_DROP: free holder + UMEM frame
3. afxdp_submit_egress(): Write egress batch to XSK TX ring
4. sendto() syscall to kick kernel TX processing
5. If TX ring full: retry with afxdp_complete_tx(); drop overflow (stats.tx_dropped++)
```

**NF Thread Runtime (Core 3+N):**
```
Per polling iteration:
1. rte_ring_dequeue_burst() up to 256 holders from nf<N>_rx
2. For each holder:
   a. stats.rx_packets++, stats.rx_bytes += len
   b. Call nf->function_table->pkt_handler(pkt, nf)
      [simple_forward: sets pkt->meta.action = ACTION_NEXT]
   c. rte_ring_enqueue() → nf<N>_tx
   d. If TX ring full: stats.dropped++, free holder + UMEM frame
```

**Cleanup Workflow:**
```
1. Set global_exit = true (signal threads to stop)
2. Join all worker threads
3. Print final NF chain statistics
4. afxdp_chain_teardown(): destroy per-NF rte_rings
5. Detach XDP program from interface
6. xsk_socket__delete() + xsk_umem__delete()
7. munmap() UMEM hugepage buffer
```

---

#### 3. `onvm_afxdp_chain.h/c` - NF Chain Manager

**Responsibilities:**
- `afxdp_chain_init_from_spec("nf0,nf1", ctx)`: Parses chain spec string, looks up handlers in NF registry, creates per-NF `rte_ring` queues, embeds holder pool in UMEM buffer tail
- Routes packets between NFs: TX thread calls chain logic to determine next hop based on `pkt->meta.action`
- `afxdp_chain_teardown()`: Frees all ring resources

---

#### 4. `onvm_afxdp_ring.h/c` - Custom SPSC Ring (Backup)

A custom lockfree Single-Producer Single-Consumer (SPSC) ring is implemented as a fallback ring backend (`AFXDP_RING_BACKEND_CUSTOM`). It is **not used** in the default production configuration, which uses DPDK `rte_ring`. It can be activated by setting `AFXDP_DEFAULT_RING_BACKEND = AFXDP_RING_BACKEND_CUSTOM` in `onvm_afxdp_config.h`.

---

## Configuration Options

### Command-Line Flags

| Flag | Argument | Description | Default |
|------|----------|-------------|---------|
| `-d` | `<ifname>` | Network interface name **(required)** | `eth0` |
| `-Q` | `<queue_id>` | RX queue to bind (0-63) | `0` |
| `-S` | - | Use SKB (generic) XDP mode | Native |
| `-N` | - | Use native (driver) XDP mode | Auto |
| `-c` | - | Force copy mode | Try zero-copy |
| `-z` | - | Force zero-copy mode | Try zero-copy |
| `-p` | - | Use `poll()` instead of busy-wait | Busy-wait |
| `-C` | `<spec>` | NF chain spec (comma-separated NF names) | Required for chain mode |
| `-N` | - | Use real NF threads (vs. dummy) | Dummy NF threads |
| `-f` | `<file.o>` | Custom XDP kernel object file | `afxdp/af_xdp_kern.o` |
| `-P` | `<section>` | XDP program section name | `xdp_sock_prog` |
| `-v` | - | Enable verbose statistics output | Disabled |
| `-t` | `<seconds>` | Auto-shutdown after N seconds | Disabled |
| `-l` | `<packets>` | Auto-shutdown after N packets | Disabled |
| `-h` | - | Show help and exit | - |

### Key Compile-Time Constants (`onvm_afxdp_config.h`)

| Constant | Value | Description |
|----------|-------|-------------|
| `AFXDP_NUM_FRAMES` | `65536` | UMEM frames (must be power of 2) |
| `AFXDP_FRAME_SIZE` | `4096` | Bytes per UMEM frame (one page) |
| `AFXDP_RX_RING_SIZE` | `8192` | XSK RX ring descriptors |
| `AFXDP_TX_RING_SIZE` | `8192` | XSK TX ring descriptors |
| `AFXDP_FILL_RING_SIZE` | `8192` | Fill ring descriptors |
| `AFXDP_COMP_RING_SIZE` | `8192` | Completion ring descriptors |
| `AFXDP_RX_BATCH_SIZE` | `256` | Max packets per RX batch |
| `AFXDP_TX_BATCH_SIZE` | `256` | Max packets per TX batch |
| `AFXDP_NF_RING_SIZE` | `8192` | Per-NF rte_ring capacity |
| `AFXDP_NF_RING_BURST` | `256` | Max dequeue burst per NF |
| `AFXDP_PKT_HOLDER_POOL_SIZE` | `65536` | Total packet holder wrappers |
| `AFXDP_MAX_CHAIN_LENGTH` | `8` | Max NFs in a single chain |
| `AFXDP_STATS_INTERVAL` | `2` | Seconds between stats prints |
| `AFXDP_DEFAULT_RING_BACKEND` | `AFXDP_RING_BACKEND_RTE` | Active ring backend |

### XDP Attachment Modes

1. **Native Mode** (`-N`): XDP runs in NIC driver. Best performance. Requires driver support.
2. **SKB Mode** (`-S`): XDP runs in generic kernel layer. Works on all NICs (including `virtio_net`). Copy mode only.
3. **Auto Mode** (default): Tries native first, falls back to SKB.

---

## Building and Compilation

### Step 1: Allocate Hugepages

AF_XDP UMEM requires hugepage memory. Configure and mount hugepages before running:

```bash
# Check current hugepage status
grep HugePages /proc/meminfo

# Allocate 150 × 2MB hugepages (= 300 MB, enough for testing)
echo 150 | sudo tee /proc/sys/vm/nr_hugepages

# Verify allocation
grep HugePages_Free /proc/meminfo
# Expected: HugePages_Free: 150

# If using 1GB hugepages instead, mount the hugetlbfs
# sudo mount -t hugetlbfs -o pagesize=1G none /mnt/huge
```

### Step 2: Install Dependencies

```bash
# Install libbpf, libxdp, clang, and kernel headers (Ubuntu/Debian)
sudo apt-get update
sudo apt-get install -y \
    clang llvm \
    libbpf-dev libxdp-dev \
    linux-headers-$(uname -r) \
    build-essential

# Verify clang version (need >= 10)
clang --version
```

### Step 3: Build the eBPF Kernel Object

```bash
cd ~/Afxdp_path_manager/onvm/onvm_mgr/afxdp
make
# Output: af_xdp_kern.o (BPF bytecode)

# Verify:
file af_xdp_kern.o
# Should show: ELF 64-bit LSB relocatable, eBPF, version 1 (SYSV)
```

### Step 4: Build the AF_XDP Manager Binary

```bash
cd ~/Afxdp_path_manager/onvm/onvm_mgr

# Full clean build (recommended after any code change)
make MODE=AFXDP clean && make MODE=AFXDP
```

**What this produces:**
- Compiles `af_xdp_kern.o` (eBPF via clang)
- Compiles `onvm_afxdp.c`, `onvm_afxdp_chain.c`, `onvm_afxdp_ring.c`, `onvm_afxdp_nf_registry.c`, and NF sources
- Links against `-lxdp -lbpf -lelf -lz -lpthread` and DPDK libraries
- Produces `onvm_mgr_afxdp` binary

### Step 5: Verify Build

```bash
# Check the binary exists and is x86-64 ELF
file onvm_mgr_afxdp
# Expected: ELF 64-bit LSB executable, x86-64, dynamically linked

# Check linked libraries
ldd onvm_mgr_afxdp | grep -E 'libbpf|libxdp|librte'
```

---

## Usage Examples

> **Important:** The manager uses DPDK EAL (Environment Abstraction Layer) for hugepage and lcore management. DPDK EAL arguments come **before** `--`, and manager arguments come **after** `--`.
>
> The canonical form is:
> ```bash
> sudo ./onvm_mgr_afxdp [EAL args] -- [manager args]
> ```

---

### Example 1: 2-NF Chain — Busy-Wait Mode (Maximum Throughput)

This is the primary production mode. Both NF threads run `simple_forward`, which sets `ACTION_NEXT` on every packet, forwarding it through the full chain before egress.

```bash
sudo ./onvm_mgr_afxdp -- -d enp1s0 -N -Q 1 -C simple_forward,simple_forward -v
```

- `-d enp1s0`: Bind to interface `enp1s0`
- `-N`: Native XDP mode (use virtio driver's XDP support)
- `-Q 1`: Bind to RX queue index 1
- `-C simple_forward,simple_forward`: 2-NF chain with `simple_forward` at each stage
- `-v`: Print verbose per-NF statistics every 2 seconds

**Expected Initialization Output:**
```
[AFXDP INFO] Configuration:
[AFXDP INFO]   Interface:   enp1s0 (index 2)
[AFXDP INFO]   RX Queue:    1
[AFXDP INFO]   XDP Object:  afxdp/af_xdp_kern.o
[AFXDP INFO]   XDP Prog:    xdp_sock_prog
[AFXDP INFO]   Poll Mode:   no
[AFXDP INFO]   Verbose:     yes
[AFXDP INFO]   NF Chain:    simple_forward,simple_forward
[AFXDP INFO] XDP program attached to enp1s0
[AFXDP INFO] UMEM frames: 65536 × 4096 bytes = 262144 KB total
[AFXDP INFO] Chain: holder pool embedded in UMEM buffer (65536 holders, 24 bytes each)
[AFXDP INFO] Chain: NF 0 initialized
[AFXDP INFO] Chain: NF 1 initialized
[AFXDP INFO] ========================================
[AFXDP INFO]   NF Chain Initialized: 2 NFs
[AFXDP INFO]   Ring backend: DPDK rte_ring
[AFXDP INFO]   Ring size: 8192  Burst: 256
[AFXDP INFO] ========================================
[AFXDP INFO] Launching worker threads: RX=1  TX=1  Mgr=1  Wakeup=1
[AFXDP INFO] Launching 2 real NF threads
[AFXDP INFO] Thread pinned to core 1   (RX thread)
[AFXDP INFO] Thread pinned to core 2   (TX thread)
[AFXDP INFO] Thread pinned to core 3   (NF 0)
[AFXDP INFO] Thread pinned to core 4   (NF 1)
[AFXDP INFO] Entering main polling loop (mode: busy-wait)
```

**Expected Per-Interval Stats (with traffic):**
```
AF_XDP RX:   7,999,581 pkts (1,502,986 pps)   4,095,785 Kbytes (6156 Mbits/s) period:2.000124
       TX:   2,714,326 pkts (   512,669 pps)   1,389,734 Kbytes (2100 Mbits/s) period:2.000124

--- NF Chain Statistics ---
  NF 0: RX 7999645 pkts (4095818240 B)  TX 7999645 pkts (4095818240 B)  Dropped 0
  NF 1: RX 7999645 pkts (4095818240 B)  TX 2714432 pkts (1389789184 B)  Dropped 0
---
```

---

### Example 2: SKB (Generic) XDP Mode

Use this when the NIC does not support native XDP (e.g., older NICs, or when `-N` causes driver errors):

```bash
sudo ./onvm_mgr_afxdp -- -d enp1s0 -S -Q 1 -C simple_forward,simple_forward -v
```

---

### Example 3: Poll Mode (CPU-Friendly, Lower Throughput)

Poll mode uses `poll()` to sleep until packets arrive, saving CPU cycles when idle. This is suitable for low-traffic environments but **significantly reduces throughput** (~10x) under heavy load compared to busy-wait:

```bash
sudo ./onvm_mgr_afxdp -- -d enp1s0 -N -p -Q 1 -C simple_forward,simple_forward -v
```

> **Note:** For maximum throughput testing, always omit `-p` and use busy-wait mode.

---

### Example 4: Auto-Shutdown After a Fixed Duration

Useful for automated benchmarking:

```bash
# Run for exactly 30 seconds then cleanly exit
sudo ./onvm_mgr_afxdp -- -d enp1s0 -N -Q 1 -C simple_forward,simple_forward -v -t 30
```

---

### Example 5: Check EAL Options (DPDK Lcore Pinning)

To pin DPDK's main thread to a specific core (e.g., core 0) and limit DPDK to specific cores, pass EAL arguments before `--`:

```bash
# Use cores 0-4 for DPDK, bind AF_XDP threads to cores 1-4
sudo ./onvm_mgr_afxdp -l 0-4 -- -d enp1s0 -N -Q 1 -C simple_forward,simple_forward -v
```

To see all available DPDK EAL options:
```bash
sudo ./onvm_mgr_afxdp --help
```

---

## Testing Methods

### Test Environment Setup (2-VM Configuration)

All testing was performed on a **2-VM setup** running on the same QEMU/KVM host:

| VM | Role | Interface | IP |
|----|------|-----------|-----|
| **vm1** | Traffic Generator (pktgen) | `enp1s0` | 192.168.100.1 |
| **vm2** | AF_XDP Manager (DUT) | `enp1s0` | 192.168.100.2 |

Both VMs use `virtio_net` (driver: `virtio-pci`, device `1af4:1041`).

> **Note on virtio_net XDP:** When using native XDP (`-N`) with `virtio_net`, the driver may print:
> ```
> virtio_net virtio1 enp1s0: XDP request 13 queues but max is 4.
> XDP_TX and XDP_REDIRECT will operate in a slower locked tx mode.
> ```
> This is a **harmless warning**. The AF_XDP userspace path is unaffected by locked tx mode.

---

### Test 1: Allocate Hugepages (vm2 — DUT)

```bash
# On vm2 before running the manager
echo 150 | sudo tee /proc/sys/vm/nr_hugepages

# Verify
grep HugePages_Free /proc/meminfo
# Expected: HugePages_Free: 150

# If using DPDK EAL: ensure hugepages are also visible to DPDK
sudo dpdk-hugepages.py --setup 300M
```

---

### Test 2: Configure pktgen Traffic Generator (vm1)

```bash
# On vm1 — load kernel pktgen module
sudo modprobe pktgen

PGDEV=/proc/net/pktgen
IFACE=enp1s0
DST_MAC=<mac address of vm2 enp1s0>
DST_IP=192.168.100.2
PKT_SIZE=512   # bytes (adjust for different test conditions)

# Configure pktgen thread
echo "rem_device_all" | sudo tee $PGDEV/kpktgend_0
echo "add_device $IFACE" | sudo tee $PGDEV/kpktgend_0

# Configure device parameters
echo "pkt_size $PKT_SIZE"   | sudo tee $PGDEV/$IFACE
echo "dst_mac $DST_MAC"     | sudo tee $PGDEV/$IFACE
echo "dst $DST_IP"          | sudo tee $PGDEV/$IFACE
echo "count 0"              | sudo tee $PGDEV/$IFACE   # 0 = infinite
echo "clone_skb 0"          | sudo tee $PGDEV/$IFACE

# Start traffic (runs until Ctrl-C)
echo "start" | sudo tee $PGDEV/pgctrl
```

---

### Test 3: Run the 2-NF Chain Stress Test (vm2)

```bash
# On vm2 — must run as root
cd ~/Afxdp_path_manager/onvm/onvm_mgr

sudo ./onvm_mgr_afxdp -- -d enp1s0 -N -Q 1 -C simple_forward,simple_forward -v
```

Observe the per-NF statistics printed every 2 seconds. Press `Ctrl-C` to stop.

**Expected behavior:**
- `[AFXDP INFO] Thread pinned to core N` messages confirm CPU affinity is active
- Both `NF 0 Dropped` and `NF 1 Dropped` should be **0** (zero internal ring drops)
- `AF_XDP TX` drops are the expected egress drops caused by virtio TX backpressure — these are graceful and do not indicate a bug

---

### Test 4: Verify Thread CPU Affinity

On vm2, while the manager is running, check thread pinning:

```bash
# In a second terminal on vm2
ps -eLf | grep onvm_mgr_afxdp | awk '{print $4}' | while read tid; do
    echo -n "TID $tid → core: "
    cat /proc/$tid/status | grep Cpus_allowed_list
done

# Or use taskset
for tid in $(ps -eLf | grep onvm_mgr | awk '{print $4}'); do
    echo "TID $tid on core: $(taskset -cp $tid 2>/dev/null | awk '{print $NF}')"
done
```

**Expected:** RX, TX, and NF threads each show a single dedicated core.

---

### Benchmark Results (2-VM / virtio_net / 512B packets / 2-NF chain)

These results were obtained with the full thread affinity implementation and the optimized UMEM pool size (65536 frames):

| Metric | Measured Value | Notes |
|--------|---------------|-------|
| **Peak Ingress (Wire→Userspace)** | **~1.51 Million pps** | AF_XDP RX ring burst |
| **Peak Ingress Bandwidth** | **~6.2 Gbps** | @ 512B packets |
| **Internal NF Throughput** | **~1.51 Million pps** | 0 internal drops (NF 0 TX == NF 0 RX) |
| **Peak Egress (Userspace→Wire)** | **~512,000 pps** | Limited by virtio TX locked mode |
| **Peak Egress Bandwidth** | **~2.1 Gbps** | @ 512B packets |
| **NF 0 Internal Drops** | **0** | All packets passed NF 0 without loss |
| **NF 1 Internal Drops** | **0** | All packets passed NF 1 without loss |

**Bottleneck Analysis:**
The ~3x gap between ingress (1.51M pps) and egress (512k pps) is a **NIC driver limitation** of the emulated `virtio_net` adapter (locked TX mode) — not a software bottleneck. The AF_XDP userspace pipeline processed all packets correctly; the excess packets are gracefully dropped at the egress edge by the TX thread.

**Sample log at peak throughput:**
```
AF_XDP RX:  11,030,844 pkts (1,515,506 pps)  5,647,792 Kbytes (6208 Mbits/s) period:2.000123
       TX:   3,722,304 pkts (  503,905 pps)  1,905,819 Kbytes (2064 Mbits/s) period:2.000123

--- NF Chain Statistics ---
  NF 0: RX 11030972 pkts (5647857664 B)  TX 11030908 pkts (5647824896 B)  Dropped 0
  NF 1: RX 11030780 pkts (5647759360 B)  TX 3722304 pkts (1905819648 B)  Dropped 0
---
```

---

### Test 5: Functional Ring Validation (Zero-Drop Invariant)

Under any traffic load, the key correctness invariant is:

```
NF 0 TX packets == NF 1 RX packets  (no drops between stages)
NF 1 dropped == 0                   (last NF never drops to its own TX ring)
```

To verify after a test run, look at the Final Statistics block:

```bash
# Expected output on Ctrl-C:
--- NF Chain Statistics ---
  NF 0: RX 12973899 pkts (6642636288 B)  TX 12973899 pkts (6642636288 B)  Dropped 0
  NF 1: RX 12973899 pkts (6642636288 B)  TX 4385226 pkts (2245235712 B)  Dropped 0
---
```

If `NF 0 Dropped > 0`, the NF 1 RX ring was full — typically indicates NF 1 fell behind NF 0 due to CPU scheduling contention. Verify thread affinity is active (Test 4).

---

### Test 6: BPF Map Inspection (Debug)

While the manager is running, inspect the XSKMAP to confirm the socket is registered:

```bash
sudo bpftool map show
# Find the xsks_map entry

sudo bpftool map dump name xsks_map
# Should show: key 0x01 (queue 1) → value <fd>
```

Confirm XDP is attached to the interface:

```bash
sudo ip link show enp1s0 | grep xdp
# Expected: xdp/id:<prog_id>
```

---
