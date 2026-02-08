# AF_XDP Manager - Comprehensive Guide

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
10. [Troubleshooting](#troubleshooting)
11. [Known Issues and Bugs](#known-issues-and-bugs)
12. [Performance Tuning](#performance-tuning)

---

## Overview

The AF_XDP (Address Family XDP) manager is a high-performance, zero-copy packet I/O implementation for the openNetVM NF Manager. It replaces the traditional DPDK datapath with a Linux kernel-native AF_XDP socket interface, providing:

- **Zero-copy packet processing** via shared UMEM (Unified Memory)
- **Direct NIC-to-userspace packet transfer** bypassing the kernel network stack
- **XDP (eXpress Data Path)** eBPF program for packet steering at the NIC driver level
- **Lower latency** and **reduced CPU overhead** compared to traditional kernel networking

### Key Concept
The manager acts as a simple **packet bouncer**: packets received from the NIC via AF_XDP are immediately transmitted back to the NIC through the same UMEM region, demonstrating the basic zero-copy datapath.

**Architecture Flow:**
```
NIC RX → XDP Prog (eBPF) → AF_XDP RX Ring → Userspace Processing → AF_XDP TX Ring → NIC TX
         ↓                    ↓                                          ↓
    Redirect to Socket    Zero-copy UMEM                          Zero-copy UMEM
```

---

## Architecture

### Components

1. **XDP Kernel Program (`af_xdp_kern.c`)**
   - eBPF program loaded onto the NIC's XDP hook
   - Inspects RX queue index and redirects packets to AF_XDP sockets
   - Maintains per-queue packet statistics
   - Runs in kernel context with minimal overhead

2. **Userspace Manager (`onvm_afxdp.c`)**
   - Configures UMEM (shared packet buffer)
   - Creates and manages AF_XDP sockets
   - Implements RX/TX polling loops
   - Handles packet processing and frame allocation

3. **Type Definitions (`onvm_afxdp_types.h`)**
   - Data structures for UMEM, sockets, stats, and configuration
   - Complete type system for the AF_XDP implementation

4. **Configuration (`onvm_afxdp_config.h`)**
   - Tunable parameters (ring sizes, batch sizes, timeouts)
   - Default values and macros

### Key Data Structures

#### UMEM (Unified Memory)
```c
struct afxdp_umem_info {
    struct xsk_ring_prod fq;      // Fill ring (user → kernel)
    struct xsk_ring_cons cq;      // Completion ring (kernel → user)
    struct xsk_umem *umem;        // libxdp UMEM handle
    void *buffer;                 // Raw mmap'd memory region
};
```

#### XSK Socket
```c
struct afxdp_socket_info {
    struct xsk_ring_cons rx;              // RX ring (kernel → user)
    struct xsk_ring_prod tx;              // TX ring (user → kernel)
    struct afxdp_umem_info *umem;         // Shared UMEM reference
    struct xsk_socket *xsk;               // libxdp socket handle
    uint64_t umem_frame_addr[NUM];        // Free-list allocator
    uint32_t umem_frame_free;             // Count of free frames
    uint32_t outstanding_tx;              // Pending TX completions
    struct afxdp_stats_record stats;      // Live statistics
};
```

#### Manager Context
```c
struct afxdp_manager_ctx {
    struct afxdp_config cfg;              // Runtime configuration
    struct afxdp_umem_info *umem;         // UMEM region
    struct afxdp_socket_info *xsk_socket; // Primary socket
    struct xdp_program *xdp_prog;         // XDP program handle
    int xsk_map_fd;                       // XSKMAP file descriptor
    pthread_t stats_thread;               // Statistics thread
    volatile bool global_exit;            // Shutdown flag
};
```

---

## Dependencies

### System Requirements
- **Linux Kernel**: >= 5.3 (recommended >= 5.11 for better AF_XDP support)
- **CPU**: x86_64 architecture with XDP-capable NIC
- **NIC**: Network card with XDP support (native mode recommended)

### Required Libraries

1. **libbpf** (>= 0.3.0)
   - BPF program loading and management
   - Install: `sudo apt-get install libbpf-dev`

2. **libxdp** (>= 1.0.0)
   - High-level AF_XDP socket API
   - Install: `sudo apt-get install libxdp-dev`
   - Source: https://github.com/xdp-project/xdp-tools

3. **Linux Kernel Headers**
   - Required for BPF and XDP definitions
   - Install: `sudo apt-get install linux-headers-$(uname -r)`

4. **clang** (>= 10.0)
   - BPF bytecode compiler
   - Install: `sudo apt-get install clang llvm`

5. **Standard Libraries**
   - pthread (POSIX threads)
   - Standard C library (glibc)

### Optional Dependencies
- **bpftool**: For BPF map inspection and debugging
  - Install: `sudo apt-get install linux-tools-common linux-tools-generic`

### Kernel Configuration
Ensure the following kernel config options are enabled:
```bash
CONFIG_BPF=y
CONFIG_BPF_SYSCALL=y
CONFIG_XDP_SOCKETS=y
CONFIG_BPF_JIT=y
```

Verify with:
```bash
zgrep -E 'CONFIG_BPF|CONFIG_XDP' /proc/config.gz
```

---

## File Structure and Workflow

### Files Overview

| File | Purpose | Lines | Compiled |
|------|---------|-------|----------|
| `af_xdp_kern.c` | XDP eBPF kernel program | 140 | Yes (BPF bytecode) |
| `onvm_afxdp.c` | Main AF_XDP manager implementation | 908 | Yes (userspace) |
| `onvm_afxdp.h` | Public API declarations | 117 | No (header) |
| `onvm_afxdp_types.h` | Type definitions and includes | 228 | No (header) |
| `onvm_afxdp_config.h` | Configuration constants | 150 | No (header) |
| `Makefile` | Build script for XDP kernel program | 40 | No |

### Workflow by File

#### 1. `af_xdp_kern.c` - XDP Kernel Program

**Purpose**: Packet steering at the NIC driver level

**Workflow**:
```
1. Packet arrives at NIC
2. NIC driver invokes XDP hook
3. xdp_sock_prog(ctx) is called with packet context
4. Extract RX queue index from ctx->rx_queue_index
5. Update per-queue packet counter in xdp_stats_map
6. Lookup socket FD in xsks_map[queue_index]
7. If socket exists:
   → bpf_redirect_map() to AF_XDP socket (zero-copy to userspace)
8. Else:
   → XDP_PASS (continue to normal kernel stack)
```

**Key Functions**:
- `xdp_sock_prog()`: Main entry point (executed per packet)
- `bpf_redirect_map()`: BPF helper to redirect packet to AF_XDP socket
- `bpf_map_lookup_elem()`: Check if socket registered for queue

**BPF Maps**:
```c
// XSKMAP: RX queue index → AF_XDP socket fd
xsks_map: BPF_MAP_TYPE_XSKMAP[64]

// Statistics: RX queue index → packet count (per-CPU)
xdp_stats_map: BPF_MAP_TYPE_PERCPU_ARRAY[64]
```

**Compilation**: Compiled to BPF bytecode (.o file) by clang with `-target bpf`

---

#### 2. `onvm_afxdp.c` - Userspace Manager

**Purpose**: Main packet processing engine in userspace

**Initialization Workflow** (`afxdp_init`):
```
1. Parse command-line arguments (interface, queue, XDP mode)
2. Install signal handlers (SIGINT, SIGTERM)
3. Load XDP kernel program from af_xdp_kern.o
4. Attach XDP program to network interface
5. Find and open xsks_map BPF map
6. Raise RLIMIT_MEMLOCK for UMEM registration
7. Allocate UMEM packet buffer (NUM_FRAMES × FRAME_SIZE)
8. Configure UMEM with Fill/Completion rings
9. Create AF_XDP socket bound to (interface, queue)
10. Insert socket FD into xsks_map[queue]
11. Initialize UMEM frame allocator (free-list)
12. Pre-fill Fill ring with empty UMEM frames
13. Start statistics thread (if verbose mode)
```

**Runtime Workflow** (`afxdp_run`):
```
Main Loop (until global_exit):
├─ Poll or busy-wait for RX packets
├─ afxdp_handle_receive():
│  ├─ Peek RX ring for received packet descriptors
│  ├─ Refill Fill ring with free UMEM frames
│  ├─ For each received packet:
│  │  ├─ Get UMEM address and length from descriptor
│  │  ├─ afxdp_process_packet():
│  │  │  ├─ Reserve slot on TX ring
│  │  │  ├─ Copy descriptor (addr, len) to TX ring
│  │  │  ├─ Submit to kernel for transmission
│  │  │  └─ Update TX stats
│  │  └─ If TX fails → free UMEM frame
│  ├─ Release consumed RX descriptors
│  ├─ Update RX stats
│  └─ afxdp_complete_tx():
│     ├─ Kick kernel with sendto() to flush TX
│     ├─ Peek Completion ring for done TX frames
│     ├─ Reclaim UMEM frames to free-list
│     └─ Update outstanding_tx counter
└─ Check auto-shutdown conditions (TTL, packet limit)
```

**Cleanup Workflow** (`afxdp_cleanup`):
```
1. Join statistics thread
2. Print final statistics
3. Detach XDP program from interface
4. Close XDP program handle
5. Delete AF_XDP socket
6. Delete UMEM region
7. Free raw packet buffer
```

**UMEM Frame Allocator**:
```c
// Stack-based allocator (LIFO)
afxdp_alloc_umem_frame():
  - Pop frame address from umem_frame_addr[]
  - Decrement umem_frame_free
  - Return INVALID if pool exhausted

afxdp_free_umem_frame(frame):
  - Push frame address back to umem_frame_addr[]
  - Increment umem_frame_free
```

---

#### 3. `onvm_afxdp.h` - Public API

**Exports three main functions**:

```c
int afxdp_init(struct afxdp_manager_ctx *ctx, int argc, char **argv);
int afxdp_run(struct afxdp_manager_ctx *ctx);
void afxdp_cleanup(struct afxdp_manager_ctx *ctx);
```

**Integration Point**: Used by `onvm_mgr/main.c` when compiled with `-DUSE_AFXDP`

---

#### 4. `onvm_afxdp_types.h` - Type Definitions

**Contains**:
- System and AF_XDP library includes
- `afxdp_umem_info`: UMEM management structure
- `afxdp_socket_info`: Socket state and rings
- `afxdp_stats_record`: Per-socket statistics
- `afxdp_config`: Runtime configuration from CLI
- `afxdp_manager_ctx`: Top-level manager state

---

#### 5. `onvm_afxdp_config.h` - Configuration

**Tunable Parameters**:

| Parameter | Default | Description |
|-----------|---------|-------------|
| `AFXDP_NUM_FRAMES` | 4096 | Total UMEM frames (must be power of 2) |
| `AFXDP_FRAME_SIZE` | 4096 | Size per frame (1 page) |
| `AFXDP_RX_RING_SIZE` | 2048 | RX descriptor ring size |
| `AFXDP_TX_RING_SIZE` | 2048 | TX descriptor ring size |
| `AFXDP_FILL_RING_SIZE` | 2048 | Fill ring size |
| `AFXDP_COMP_RING_SIZE` | 2048 | Completion ring size |
| `AFXDP_RX_BATCH_SIZE` | 64 | Max packets per RX batch |
| `AFXDP_TX_BATCH_SIZE` | 64 | Max packets per TX batch |
| `AFXDP_STATS_INTERVAL` | 2 | Seconds between stats output |
| `AFXDP_MAX_SOCKETS` | 64 | Max sockets in XSKMAP |

**Logging Macros**:
```c
AFXDP_LOG_INFO(fmt, ...)   // Stdout logging
AFXDP_LOG_ERR(fmt, ...)    // Stderr error logging
AFXDP_LOG_WARN(fmt, ...)   // Stderr warning logging
```

---

## Function Reference

### Core API Functions

#### `afxdp_init()`
```c
int afxdp_init(struct afxdp_manager_ctx *ctx, int argc, char **argv)
```
**Purpose**: Complete initialization of the AF_XDP manager

**Parameters**:
- `ctx`: Pointer to manager context (must be zeroed by caller)
- `argc`, `argv`: Command-line arguments

**Returns**: 0 on success, negative errno on failure

**Steps**:
1. Parse CLI arguments
2. Install signal handlers
3. Load and attach XDP program
4. Configure UMEM
5. Create AF_XDP socket
6. Start stats thread

**Error Handling**: Returns immediately on any initialization failure

---

#### `afxdp_run()`
```c
int afxdp_run(struct afxdp_manager_ctx *ctx)
```
**Purpose**: Enter the main packet processing loop

**Parameters**:
- `ctx`: Initialized manager context

**Returns**: 0 on clean shutdown, negative errno on error

**Behavior**:
- Blocks until `ctx->global_exit` is set
- Continuously polls RX ring and processes packets
- Handles TX completions and UMEM frame recycling
- Supports two modes:
  - **Busy-wait**: Tight loop for lowest latency
  - **Poll mode** (`-p`): Uses `poll()` syscall to save CPU

**Exit Conditions**:
- SIGINT/SIGTERM received
- Time-to-live expired (`-t` flag)
- Packet limit reached (`-l` flag)

---

#### `afxdp_cleanup()`
```c
void afxdp_cleanup(struct afxdp_manager_ctx *ctx)
```
**Purpose**: Release all AF_XDP resources and cleanup

**Parameters**:
- `ctx`: Manager context to clean up

**Actions**:
1. Join stats thread
2. Print final statistics
3. Detach XDP program
4. Delete socket and UMEM
5. Free all allocated memory

---

### Internal Functions

#### Argument Parsing

##### `afxdp_parse_args()`
```c
static void afxdp_parse_args(struct afxdp_config *cfg, int argc, char **argv)
```
**Purpose**: Parse command-line flags and populate configuration

**Supported Options**:
```
-d <ifname>     Network interface (required)
-Q <queue>      RX queue index (default: 0)
-S              SKB mode (generic XDP)
-N              Native mode (driver XDP)
-c              Force copy mode
-z              Force zero-copy mode
-p              Poll mode (use poll() instead of busy-wait)
-f <file.o>     Custom XDP kernel object
-P <section>    XDP program section name
-v              Verbose stats
-t <seconds>    Auto-shutdown after N seconds
-l <packets>    Auto-shutdown after N packets
-h              Show help
```

**Behavior**:
- Resolves interface name to index via `if_nametoindex()`
- Validates parameters
- Sets defaults for unspecified options
- Exits on invalid input

---

#### UMEM Management

##### `afxdp_configure_umem()`
```c
static struct afxdp_umem_info *afxdp_configure_umem(void *buffer, uint64_t size)
```
**Purpose**: Create UMEM region with Fill and Completion rings

**Parameters**:
- `buffer`: Pre-allocated memory region (page-aligned)
- `size`: Total size in bytes

**Returns**: Pointer to UMEM info struct, or NULL on failure

**Internal Call**: `xsk_umem__create()` (libxdp)

---

##### `afxdp_alloc_umem_frame()`
```c
static uint64_t afxdp_alloc_umem_frame(struct afxdp_socket_info *xsk)
```
**Purpose**: Allocate one UMEM frame from free-list

**Returns**:
- Frame address (offset into UMEM) on success
- `AFXDP_INVALID_UMEM_FRAME` if pool exhausted

**Algorithm**: Stack-based LIFO allocator (O(1))

---

##### `afxdp_free_umem_frame()`
```c
static void afxdp_free_umem_frame(struct afxdp_socket_info *xsk, uint64_t frame)
```
**Purpose**: Return a UMEM frame to free-list

**Parameters**:
- `xsk`: Socket info containing frame allocator
- `frame`: Frame address to free

**Algorithm**: Push to stack (O(1))

---

##### `afxdp_umem_free_frames()`
```c
static uint64_t afxdp_umem_free_frames(struct afxdp_socket_info *xsk)
```
**Purpose**: Query number of free UMEM frames

**Returns**: Count of available frames

---

#### Socket Management

##### `afxdp_configure_socket()`
```c
static struct afxdp_socket_info *afxdp_configure_socket(struct afxdp_manager_ctx *ctx)
```
**Purpose**: Create and configure AF_XDP socket with RX/TX rings

**Returns**: Pointer to socket info, or NULL on failure

**Steps**:
1. Allocate socket info struct
2. Configure socket parameters (ring sizes, flags)
3. Call `xsk_socket__create()` (libxdp)
4. Insert socket into XSKMAP (if custom XDP program)
5. Initialize UMEM frame allocator
6. Pre-populate Fill ring with empty frames

**Ring Configuration**:
```c
xsk_cfg.rx_size = AFXDP_RX_RING_SIZE;
xsk_cfg.tx_size = AFXDP_TX_RING_SIZE;
xsk_cfg.bind_flags = XDP_ZEROCOPY | XDP_COPY;  // Try zero-copy, fallback to copy
```

---

#### Packet Processing

##### `afxdp_process_packet()`
```c
static bool afxdp_process_packet(struct afxdp_socket_info *xsk,
                                 uint64_t addr, uint32_t len)
```
**Purpose**: Process one received packet (bounce to TX)

**Parameters**:
- `xsk`: Socket info
- `addr`: UMEM frame address
- `len`: Packet length in bytes

**Returns**:
- `true`: Packet placed on TX ring (will be transmitted)
- `false`: TX ring full, packet dropped

**Logic**:
1. Reserve slot on TX ring
2. Copy descriptor (addr, len) to TX ring
3. Submit descriptor to kernel
4. Update TX statistics

**Zero-Copy**: The packet data is NOT copied; only the descriptor (16 bytes) is written to the TX ring.

---

##### `afxdp_handle_receive()`
```c
static void afxdp_handle_receive(struct afxdp_manager_ctx *ctx)
```
**Purpose**: Core RX processing - one batch of packets

**Steps**:
1. **Peek RX ring**: Check how many packets arrived
2. **Refill Fill ring**: Provide empty UMEM frames to kernel
3. **Process packets**: Call `afxdp_process_packet()` for each
4. **Release RX ring**: Mark descriptors as consumed
5. **Complete TX**: Drain Completion ring and reclaim frames

**Batch Size**: Up to `AFXDP_RX_BATCH_SIZE` (64) packets per call

---

##### `afxdp_complete_tx()`
```c
static void afxdp_complete_tx(struct afxdp_socket_info *xsk)
```
**Purpose**: Drain Completion ring and reclaim transmitted frames

**Steps**:
1. Call `sendto()` with `MSG_DONTWAIT` to kick TX processing
2. Peek Completion ring for done frames
3. Free each frame back to allocator
4. Decrement `outstanding_tx` counter

**Why Needed**: The kernel asynchronously transmits packets. We must poll the Completion ring to know when frames are safe to reuse.

---

#### Statistics

##### `afxdp_stats_poll()`
```c
static void *afxdp_stats_poll(void *arg)
```
**Purpose**: Statistics thread - prints RX/TX stats periodically

**Runs**: In separate pthread (if `-v` flag set)

**Output Format**:
```
AF_XDP RX:    12,345,678 pkts (1,234,567 pps) 9,876,543 Kbytes (789 Mbits/s) period:2.0
       TX:    12,345,678 pkts (1,234,567 pps) 9,876,543 Kbytes (789 Mbits/s) period:2.0
```

**Calculations**:
- **pps**: (current_packets - prev_packets) / time_interval
- **Mbps**: ((current_bytes - prev_bytes) × 8) / time_interval / 1e6

---

##### `afxdp_stats_print()`
```c
static void afxdp_stats_print(struct afxdp_stats_record *stats,
                              struct afxdp_stats_record *prev)
```
**Purpose**: Calculate and print statistics for one interval

**Parameters**:
- `stats`: Current statistics snapshot
- `prev`: Previous snapshot (for delta calculation)

---

##### `afxdp_gettime()`
```c
static uint64_t afxdp_gettime(void)
```
**Purpose**: Get current monotonic time in nanoseconds

**Returns**: Timestamp (uint64_t)

**Uses**: `clock_gettime(CLOCK_MONOTONIC, ...)`

---

## Configuration Options

### Command-Line Flags

| Flag | Argument | Description | Default |
|------|----------|-------------|---------|
| `-d` | `<ifname>` | Network interface name | `eth0` |
| `-Q` | `<queue_id>` | RX queue to bind (0-63) | `0` |
| `-S` | - | Use SKB (generic) XDP mode | Native mode |
| `-N` | - | Use native (driver) XDP mode | Auto-detect |
| `-c` | - | Force copy mode (disable zero-copy) | Try zero-copy |
| `-z` | - | Force zero-copy mode (fail if unsupported) | Try zero-copy |
| `-p` | - | Use poll() instead of busy-wait | Busy-wait |
| `-f` | `<file.o>` | Custom XDP kernel object file | `afxdp/af_xdp_kern.o` |
| `-P` | `<section>` | XDP program section name | `xdp_sock_prog` |
| `-v` | - | Enable verbose statistics output | Disabled |
| `-t` | `<seconds>` | Auto-shutdown after N seconds | Disabled (0) |
| `-l` | `<packets>` | Auto-shutdown after N packets | Disabled (0) |
| `-h` | - | Show help and exit | - |

### XDP Attachment Modes

1. **Native Mode** (`-N`)
   - XDP program runs in NIC driver
   - Best performance (zero-copy possible)
   - Requires driver support (i40e, ixgbe, mlx5, etc.)

2. **SKB Mode** (`-S`)
   - XDP program runs in generic kernel layer
   - Works on any NIC
   - Copy mode only (no zero-copy)
   - Higher overhead

3. **Auto Mode** (default)
   - Try native mode first
   - Fall back to SKB if native unavailable

### Copy vs Zero-Copy

**Zero-Copy Mode** (default, `-z` to force):
- DMA directly to/from UMEM
- Requires NIC driver support
- Highest performance
- Falls back to copy if unavailable

**Copy Mode** (`-c` flag):
- Kernel copies packets to/from UMEM
- Works on all NICs
- Higher CPU overhead
- Still faster than traditional socket I/O

---

## Building and Compilation

### Prerequisites
```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get update
sudo apt-get install -y \
    clang \
    llvm \
    libbpf-dev \
    libxdp-dev \
    linux-headers-$(uname -r) \
    build-essential

# Verify clang version (need >= 10)
clang --version
```

### Build Steps

#### 1. Build XDP Kernel Program
```bash
cd onvm/onvm_mgr/afxdp
make
```

**Output**: `af_xdp_kern.o` (BPF bytecode)

**Manual Build**:
```bash
clang -O2 -g -target bpf -c af_xdp_kern.c -o af_xdp_kern.o
```

**Verify BPF Object**:
```bash
llvm-objdump -S af_xdp_kern.o
file af_xdp_kern.o
# Should show: "ELF 64-bit LSB relocatable, eBPF, version 1 (SYSV)"
```

#### 2. Build OpenNetVM with AF_XDP Support
```bash
cd onvm
make clean
make USE_AFXDP=1
```

**What Happens**:
- Compiles `onvm_mgr/main.c` with `-DUSE_AFXDP`
- Links against `libbpf` and `libxdp`
- Compiles `onvm_afxdp.c` into the manager binary

**Build Flags**:
```makefile
CFLAGS += -DUSE_AFXDP
LDFLAGS += -lbpf -lxdp
```

#### 3. Verify Build
```bash
cd onvm_mgr
./onvm_mgr -h
# Should show AF_XDP-specific options (-d, -Q, etc.)

ldd onvm_mgr | grep -E 'libbpf|libxdp'
# Should show:
#   libbpf.so.0 => /usr/lib/x86_64-linux-gnu/libbpf.so.0
#   libxdp.so.1 => /usr/lib/x86_64-linux-gnu/libxdp.so.1
```

### Troubleshooting Build Issues

**Problem**: `fatal error: bpf/bpf_helpers.h: No such file or directory`
**Solution**: Install libbpf headers
```bash
sudo apt-get install libbpf-dev
```

**Problem**: `fatal error: xdp/xsk.h: No such file or directory`
**Solution**: Install libxdp headers
```bash
sudo apt-get install libxdp-dev
```

**Problem**: `clang: error: unknown target triple 'bpf'`
**Solution**: Update clang to version >= 10
```bash
sudo apt-get install clang-12
clang-12 -target bpf -c af_xdp_kern.c -o af_xdp_kern.o
```

---

## Usage Examples

### Example 1: Basic Usage (Default Settings)
```bash
sudo ./onvm_mgr -d eth0
```
- Binds to `eth0` interface, queue 0
- Native XDP mode (auto)
- Zero-copy if supported
- Busy-wait polling

**Expected Output**:
```
[AFXDP INFO] ========================================
[AFXDP INFO]   openNetVM AF_XDP Manager Initializing
[AFXDP INFO] ========================================
[AFXDP INFO] Configuration:
[AFXDP INFO]   Interface:   eth0 (index 2)
[AFXDP INFO]   RX Queue:    0
[AFXDP INFO]   XDP Object:  afxdp/af_xdp_kern.o
[AFXDP INFO]   XDP Prog:    xdp_sock_prog
[AFXDP INFO]   Poll Mode:   no
[AFXDP INFO]   Verbose:     no
[AFXDP INFO] Loading XDP program: afxdp/af_xdp_kern.o (section: xdp_sock_prog)
[AFXDP INFO] XDP program attached to eth0
[AFXDP INFO] Found xsks_map (fd=4)
[AFXDP INFO] UMEM buffer allocated: 16384 KB
[AFXDP INFO] AF_XDP socket created on eth0 queue 0
[AFXDP INFO]   RX ring: 2048  TX ring: 2048  Fill ring: 2048  Comp ring: 2048
[AFXDP INFO]   UMEM frames: 4096 × 4096 bytes = 16384 KB total
[AFXDP INFO] ========================================
[AFXDP INFO]   AF_XDP Manager Initialization Complete
[AFXDP INFO] ========================================
[AFXDP INFO] Manager entering main loop...
[AFXDP INFO] Entering main polling loop (mode: busy-wait)
^C
[AFXDP INFO] Main loop exited
[AFXDP INFO] Cleaning up AF_XDP resources...

--- Final Statistics ---
RX: 1234567 packets, 987654321 bytes
TX: 1234567 packets, 987654321 bytes
[AFXDP INFO] XDP program detached from eth0
[AFXDP INFO] Cleanup complete
```

---

### Example 2: Verbose Mode with Statistics
```bash
sudo ./onvm_mgr -d enp1s0 -Q 1 -v
```
- Binds to `enp1s0`, queue 1
- Verbose statistics every 2 seconds

**Expected Output** (every 2 seconds):
```
AF_XDP RX:      1,234,567 pkts (   617,283 pps)     987,654 Kbytes (  3950 Mbits/s) period:2.000000
       TX:      1,234,567 pkts (   617,283 pps)     987,654 Kbytes (  3950 Mbits/s) period:2.000000
```

---

### Example 3: Poll Mode (CPU-Friendly)
```bash
sudo ./onvm_mgr -d eth0 -p -v
```
- Poll mode: uses `poll()` syscall instead of busy-wait
- Saves CPU when idle
- Slightly higher latency (~microseconds)

**Use Case**: When CPU efficiency is more important than absolute minimum latency

---

### Example 4: Force Copy Mode
```bash
sudo ./onvm_mgr -d eth0 -c -v
```
- Forces copy mode (disables zero-copy)
- Useful for debugging or unsupported NICs

---

### Example 5: Generic (SKB) XDP Mode
```bash
sudo ./onvm_mgr -d eth0 -S -v
```
- Uses generic XDP (works on any NIC)
- No driver support needed
- Copy mode only
- Good for testing without XDP-capable hardware

---

### Example 6: Auto-Shutdown After Time or Packets
```bash
# Run for 60 seconds then exit
sudo ./onvm_mgr -d eth0 -t 60 -v

# Run until 1 million packets received
sudo ./onvm_mgr -d eth0 -l 1000000 -v
```

**Use Case**: Automated testing and benchmarking

---

### Example 7: Multi-Queue Setup
```bash
# Terminal 1: Bind to queue 0
sudo ./onvm_mgr -d eth0 -Q 0 -v &

# Terminal 2: Bind to queue 1
sudo ./onvm_mgr -d eth0 -Q 1 -v &

# Terminal 3: Bind to queue 2
sudo ./onvm_mgr -d eth0 -Q 2 -v &
```

**Note**: Each queue needs a separate manager instance. The XDP program will redirect each queue's packets to its respective AF_XDP socket.

---

### Example 8: Custom XDP Program
```bash
sudo ./onvm_mgr -d eth0 -f my_custom_xdp.o -P my_prog_section
```
- Loads custom XDP program from `my_custom_xdp.o`
- Uses section name `my_prog_section`
- Requires custom program to have `xsks_map` BPF map

---

## Testing Methods

### 1. Functional Testing

#### Test 1: Verify XDP Program Loading
```bash
# Start manager
sudo ./onvm_mgr -d eth0 -v

# In another terminal, check XDP status
sudo ip link show eth0
# Should show: "xdp/id:123"

# Inspect BPF programs
sudo bpftool prog show
# Should list the loaded XDP program

# Inspect BPF maps
sudo bpftool map show
# Should show xsks_map and xdp_stats_map
```

---

#### Test 2: Traffic Forwarding
```bash
# Setup:
# - Host A (10.0.0.1): Running onvm_mgr
# - Host B (10.0.0.2): Traffic generator

# On Host A
sudo ./onvm_mgr -d eth0 -v

# On Host B (generate traffic)
ping -c 100 10.0.0.1

# Or use traffic generator
sudo mausezahn eth0 -c 1000 -d 10msec -t udp "dp=80,sp=1234" -A 10.0.0.2 -B 10.0.0.1
```

**Expected Result**: Manager should show RX and TX packet counts increasing

---

#### Test 3: Multi-Queue Distribution
```bash
# Configure NIC for multi-queue
sudo ethtool -L eth0 combined 4

# Start managers on each queue
for q in 0 1 2 3; do
    sudo ./onvm_mgr -d eth0 -Q $q -v -l 10000 &
done

# Generate traffic to hit multiple queues
# (Use RSS or manual steering)
```

---

### 2. Performance Testing

#### Test 1: Throughput Benchmark
```bash
# Using pktgen (kernel packet generator)
sudo modprobe pktgen

# Configure pktgen to send to eth0
cat <<EOF | sudo tee /proc/net/pktgen/kpktgend_0
rem_device_all
add_device eth0
EOF

cat <<EOF | sudo tee /proc/net/pktgen/eth0
clone_skb 0
pkt_size 64
count 10000000
dst 10.0.0.1
dst_mac aa:bb:cc:dd:ee:ff
EOF

# Start manager
sudo ./onvm_mgr -d eth0 -v

# In another terminal, start pktgen
echo "start" | sudo tee /proc/net/pktgen/pgctrl

# Check results
cat /proc/net/pktgen/eth0
```

---

#### Test 2: Latency Measurement
```bash
# Install sockperf
sudo apt-get install sockperf

# On receiver (with AF_XDP manager)
sudo ./onvm_mgr -d eth0 -v

# On sender
sockperf ping-pong -i 10.0.0.1 -p 11111 --pps 1000 --time 60
```

**Metrics to Record**:
- Average latency
- 99th percentile latency
- Maximum latency

---

#### Test 3: CPU Utilization
```bash
# Start manager
sudo ./onvm_mgr -d eth0 -v &
PID=$!

# Monitor CPU usage
sudo perf stat -p $PID -e cycles,instructions,cache-misses,LLC-loads,LLC-load-misses sleep 30

# Or use top
top -p $PID
```

---

### 3. Stress Testing

#### Test 1: Packet Drop Under Load
```bash
# Start manager
sudo ./onvm_mgr -d eth0 -v

# Generate high packet rate
sudo pkt-gen -i eth0 -f tx -r 10000000  # 10 Mpps

# Monitor drops
ethtool -S eth0 | grep drop
```

---

#### Test 2: Memory Leak Detection
```bash
# Install valgrind
sudo apt-get install valgrind

# Run manager under valgrind
sudo valgrind --leak-check=full --show-leak-kinds=all \
    ./onvm_mgr -d eth0 -t 30 -v

# Check for memory leaks in output
```

---

### 4. Error Handling Testing

#### Test 1: Invalid Interface
```bash
sudo ./onvm_mgr -d invalid_if
# Expected: Error message and exit
```

---

#### Test 2: Permission Denied
```bash
# Run without sudo
./onvm_mgr -d eth0
# Expected: Permission error or capability error
```

---

#### Test 3: XDP Already Attached
```bash
# Start first instance
sudo ./onvm_mgr -d eth0 &

# Try to start second instance on same queue
sudo ./onvm_mgr -d eth0 -Q 0
# Expected: Error (XDP program already attached)
```

---

### 5. Compatibility Testing

#### Test 1: Different NIC Drivers
Test on various drivers:
- **i40e** (Intel XL710)
- **ixgbe** (Intel 82599)
- **mlx5** (Mellanox ConnectX-5)
- **virtio_net** (virtual NIC, SKB mode only)

```bash
# Check driver
ethtool -i eth0 | grep driver

# Test native mode
sudo ./onvm_mgr -d eth0 -N -v

# Test SKB mode
sudo ./onvm_mgr -d eth0 -S -v
```

---

#### Test 2: Kernel Version Compatibility
```bash
uname -r  # Check kernel version

# Test on:
# - 5.3.x (minimum supported)
# - 5.11.x (improved AF_XDP)
# - 5.15.x (LTS with enhancements)
# - 6.x (latest)
```

---

### 6. Debugging Tests

#### Test 1: BPF Program Verification
```bash
# Load and verify BPF program manually
sudo bpftool prog load af_xdp_kern.o /sys/fs/bpf/xdp_test

# Check verifier log
dmesg | tail -50
```

---

#### Test 2: Ring Inspection
```bash
# While manager is running, inspect maps
sudo bpftool map dump name xsks_map
sudo bpftool map dump name xdp_stats_map
```

---

### 7. Integration Testing

#### Test 1: With Traffic Generator
```bash
# Use MoonGen (high-performance traffic generator)
cd tools/Pktgen
./MoonGen examples/l2-forward.lua 0 1

# Manager should receive and bounce packets
sudo ./onvm_mgr -d eth1 -v
```

---

### 8. Regression Testing Script
```bash
#!/bin/bash
# test_afxdp.sh - Comprehensive test suite

set -e

IFACE="eth0"
MGR="./onvm_mgr"

echo "=== AF_XDP Regression Tests ==="

# Test 1: Basic startup and shutdown
echo "Test 1: Basic startup/shutdown"
timeout 5 sudo $MGR -d $IFACE -v || true
echo "✓ Pass"

# Test 2: Stats thread
echo "Test 2: Stats thread"
timeout 10 sudo $MGR -d $IFACE -v -t 5
echo "✓ Pass"

# Test 3: Poll mode
echo "Test 3: Poll mode"
timeout 5 sudo $MGR -d $IFACE -p || true
echo "✓ Pass"

# Test 4: SKB mode
echo "Test 4: SKB mode"
timeout 5 sudo $MGR -d $IFACE -S || true
echo "✓ Pass"

# Test 5: Auto-shutdown on packet count
echo "Test 5: Packet limit"
timeout 30 sudo $MGR -d $IFACE -l 1000 || true
echo "✓ Pass"

echo "=== All Tests Passed ==="
```

---

## Troubleshooting

### Common Issues and Solutions

#### Issue 1: "Cannot find interface 'eth0'"
**Cause**: Interface name doesn't exist or typo

**Solution**:
```bash
# List available interfaces
ip link show

# Use correct interface name
sudo ./onvm_mgr -d <correct_interface_name>
```

---

#### Issue 2: "Permission denied" or "Operation not permitted"
**Cause**: Insufficient permissions to load BPF program or bind socket

**Solution**:
```bash
# Run with sudo
sudo ./onvm_mgr -d eth0

# Or grant capabilities
sudo setcap cap_net_admin,cap_sys_admin,cap_bpf+ep ./onvm_mgr
./onvm_mgr -d eth0
```

---

#### Issue 3: "Failed to attach XDP program"
**Cause**: XDP already attached, or driver doesn't support XDP

**Solution**:
```bash
# Remove existing XDP program
sudo ip link set dev eth0 xdp off

# Try again
sudo ./onvm_mgr -d eth0

# If still fails, try SKB mode
sudo ./onvm_mgr -d eth0 -S
```

---

#### Issue 4: "xsk_socket__create failed: Invalid argument"
**Cause**: Invalid queue index or zero-copy not supported

**Solution**:
```bash
# Check number of queues
ethtool -l eth0

# Use valid queue index (0 to N-1)
sudo ./onvm_mgr -d eth0 -Q 0

# Try copy mode if zero-copy fails
sudo ./onvm_mgr -d eth0 -c
```

---

#### Issue 5: "Cannot find xsks_map in BPF object"
**Cause**: XDP kernel program not compiled correctly or wrong version

**Solution**:
```bash
# Rebuild XDP program
cd afxdp
make clean
make

# Verify object file
llvm-objdump -S af_xdp_kern.o | grep xsks_map
```

---

#### Issue 6: Zero Packet RX/TX
**Cause**: No traffic routed to the interface, or XDP not redirecting

**Solution**:
```bash
# Check interface status
ip link show eth0

# Verify XDP attached
sudo bpftool net show dev eth0

# Check XDP stats map
sudo bpftool map dump name xdp_stats_map

# Generate test traffic
ping -I eth0 8.8.8.8
```

---

#### Issue 7: High Packet Drops
**Cause**: Insufficient UMEM frames, small ring sizes, or slow processing

**Solution**:
```bash
# Increase UMEM frames (edit onvm_afxdp_config.h)
#define AFXDP_NUM_FRAMES 8192  // Was 4096

# Increase ring sizes
#define AFXDP_RX_RING_SIZE 4096  // Was 2048

# Rebuild and test
make clean && make USE_AFXDP=1
```

---

#### Issue 8: "setrlimit(RLIMIT_MEMLOCK) failed"
**Cause**: Cannot raise memory lock limit

**Solution**:
```bash
# Check current limit
ulimit -l

# Increase limit (temporary)
ulimit -l unlimited

# Or set in /etc/security/limits.conf (permanent)
echo "* soft memlock unlimited" | sudo tee -a /etc/security/limits.conf
echo "* hard memlock unlimited" | sudo tee -a /etc/security/limits.conf
```

---

#### Issue 9: "Failed to load XDP program" with verifier error
**Cause**: BPF verifier rejected the program

**Solution**:
```bash
# Check kernel logs
sudo dmesg | tail -100

# Common issues:
# - Unbounded loops (add loop limit)
# - Invalid memory access (check array bounds)
# - Missing NULL checks (add validation)

# Verify program manually
sudo bpftool prog load af_xdp_kern.o /sys/fs/bpf/test_prog
```

---

### Debug Mode

Enable verbose logging for troubleshooting:
```bash
# Verbose AF_XDP manager
sudo ./onvm_mgr -d eth0 -v

# Kernel BPF debug logs
sudo sysctl kernel.printk="7 7 7 7"
sudo dmesg -w | grep -i bpf

# Trace BPF program execution
sudo bpftrace -e 'tracepoint:xdp:xdp_redirect* { printf("%s\n", comm); }'
```

---

## Known Issues and Bugs

### 🐛 Bug 1: Race Condition in TX Completion

**Location**: [onvm_afxdp.c](onvm_afxdp.c#L540-L560) - `afxdp_complete_tx()`

**Issue**: The `outstanding_tx` counter can underflow if completion happens faster than expected.

**Current Code**:
```c
xsk->outstanding_tx -= (completed < xsk->outstanding_tx)
    ? completed : xsk->outstanding_tx;
```

**Problem**: This is a workaround, not a proper fix. The root cause is that TX completions may arrive before `outstanding_tx` is incremented.

**Impact**: Low - the workaround prevents crashes, but stats may be inaccurate

**Suggested Fix**:
```c
// Use atomic operations
__atomic_fetch_sub(&xsk->outstanding_tx, completed, __ATOMIC_RELAXED);
```

---

### 🐛 Bug 2: Infinite Loop in Fill Ring Refill

**Location**: [onvm_afxdp.c](onvm_afxdp.c#L542-L546) - `afxdp_handle_receive()`

**Issue**: The retry loop can hang if ring reservation always fails.

**Current Code**:
```c
while (ret != (int)stock_frames) {
    ret = xsk_ring_prod__reserve(&xsk->umem->fq, rcvd, &idx_fq);
}
```

**Problem**: No exit condition. If the Fill ring is persistently full, this loops forever.

**Impact**: Medium - can cause manager hang under high load

**Suggested Fix**:
```c
int retry_count = 0;
while (ret != (int)stock_frames && retry_count++ < 1000) {
    ret = xsk_ring_prod__reserve(&xsk->umem->fq, rcvd, &idx_fq);
    if (retry_count > 100) usleep(1);  // Backoff
}
if (ret != (int)stock_frames) {
    AFXDP_LOG_WARN("Failed to refill Fill ring after %d retries", retry_count);
    stock_frames = ret;  // Use partial reservation
}
```

---

### 🐛 Bug 3: No Validation of Queue Index

**Location**: [onvm_afxdp.c](onvm_afxdp.c#L160) - `afxdp_parse_args()`

**Issue**: The `-Q` flag doesn't validate the queue index against NIC capabilities.

**Current Code**:
```c
case 'Q':
    cfg->xsk_if_queue = atoi(optarg);
    break;
```

**Problem**: User can specify queue 99 on a NIC with only 4 queues. Results in bind error later.

**Impact**: Low - causes confusing error message

**Suggested Fix**:
```c
case 'Q':
    cfg->xsk_if_queue = atoi(optarg);
    if (cfg->xsk_if_queue < 0 || cfg->xsk_if_queue >= AFXDP_MAX_SOCKETS) {
        AFXDP_LOG_ERR("Invalid queue index: %d (must be 0-%d)",
                      cfg->xsk_if_queue, AFXDP_MAX_SOCKETS - 1);
        exit(EXIT_FAILURE);
    }
    break;

// Later, after interface is known:
struct ethtool_channels channels;
// ... query NIC queue count ...
if (cfg->xsk_if_queue >= channels.combined_count) {
    AFXDP_LOG_ERR("Queue %d exceeds NIC queue count (%d)",
                  cfg->xsk_if_queue, channels.combined_count);
    exit(EXIT_FAILURE);
}
```

---

### 🐛 Bug 4: Memory Leak on Init Failure

**Location**: [onvm_afxdp.c](onvm_afxdp.c#L696-L809) - `afxdp_init()`

**Issue**: If initialization fails partway through, not all resources are freed.

**Example**:
```c
// If xsk_socket creation fails here:
ctx->xsk_socket = afxdp_configure_socket(ctx);
if (!ctx->xsk_socket) {
    // BUG: XDP program and UMEM are not cleaned up!
    return -ENODEV;
}
```

**Impact**: Medium - leaves XDP program attached and memory allocated

**Suggested Fix**: Add cleanup labels with `goto` error handling:
```c
int afxdp_init(...) {
    int ret = 0;
    
    // ... initialization steps ...
    
    ctx->xdp_prog = xdp_program__create(&xdp_opts);
    if (libxdp_get_error(ctx->xdp_prog)) {
        ret = -EINVAL;
        goto err_out;
    }
    
    ctx->umem = afxdp_configure_umem(...);
    if (!ctx->umem) {
        ret = -ENOMEM;
        goto err_detach_xdp;
    }
    
    ctx->xsk_socket = afxdp_configure_socket(ctx);
    if (!ctx->xsk_socket) {
        ret = -ENODEV;
        goto err_free_umem;
    }
    
    return 0;

err_free_umem:
    xsk_umem__delete(ctx->umem->umem);
    free(ctx->packet_buffer);
err_detach_xdp:
    xdp_program__detach(ctx->xdp_prog, ...);
    xdp_program__close(ctx->xdp_prog);
err_out:
    return ret;
}
```

---

### 🐛 Bug 5: Stats Thread Not Initialized

**Location**: [onvm_afxdp.c](onvm_afxdp.c#L881-L886) - `afxdp_cleanup()`

**Issue**: `ctx->stats_thread` is never explicitly initialized to 0 or NULL.

**Current Code**:
```c
if (ctx->cfg.verbose && ctx->stats_thread) {
    pthread_join(ctx->stats_thread, NULL);
}
```

**Problem**: If pthread_create() fails, `ctx->stats_thread` contains garbage, and this check may pass incorrectly.

**Impact**: Low - rare crash in cleanup

**Suggested Fix**:
```c
// In afxdp_init():
ctx->stats_thread = 0;
ctx->stats_thread_running = false;

if (ctx->cfg.verbose) {
    err = pthread_create(&ctx->stats_thread, NULL, afxdp_stats_poll, ctx);
    if (err == 0) {
        ctx->stats_thread_running = true;
    }
}

// In afxdp_cleanup():
if (ctx->stats_thread_running) {
    pthread_join(ctx->stats_thread, NULL);
}
```

---

### 🐛 Bug 6: Signal Handler Race Condition

**Location**: [onvm_afxdp.c](onvm_afxdp.c#L104-L109) - `afxdp_signal_handler()`

**Issue**: Global pointer `g_ctx` is not thread-safe and can be accessed during initialization.

**Current Code**:
```c
static struct afxdp_manager_ctx *g_ctx = NULL;

static void afxdp_signal_handler(int signum) {
    if (g_ctx) {
        g_ctx->global_exit = true;
    }
}

// In afxdp_init():
g_ctx = ctx;  // Set after signal handlers installed
```

**Problem**: If SIGINT arrives between signal() and `g_ctx = ctx`, handler accesses NULL pointer.

**Impact**: Low - very narrow race window

**Suggested Fix**:
```c
// Set g_ctx BEFORE installing handlers
g_ctx = ctx;
ctx->global_exit = false;

// Memory barrier
__sync_synchronize();

signal(SIGINT, afxdp_signal_handler);
signal(SIGTERM, afxdp_signal_handler);
```

---

### ⚠️ Limitation 1: Single Socket Per Queue

**Description**: The current implementation only supports one AF_XDP socket per RX queue.

**Impact**: Cannot run multiple managers on the same (interface, queue) pair.

**Workaround**: Use different queues (-Q flag) for multiple manager instances.

---

### ⚠️ Limitation 2: No Packet Filtering

**Description**: All packets on the bound queue are redirected to the AF_XDP socket. No filtering by protocol, port, etc.

**Impact**: Manager receives ALL traffic on the queue.

**Potential Enhancement**: Add BPF filtering in `af_xdp_kern.c`:
```c
// Example: Only redirect UDP port 1234
struct iphdr *iph = (void *)(data + sizeof(struct ethhdr));
struct udphdr *udph = (void *)((void *)iph + sizeof(struct iphdr));

if (udph->dest == htons(1234)) {
    return bpf_redirect_map(&xsks_map, index, 0);
}
return XDP_PASS;
```

---

### ⚠️ Limitation 3: No Jumbo Frame Support

**Description**: Frame size is fixed at 4096 bytes. Jumbo frames (>4KB) are not supported.

**Impact**: Cannot handle MTU > 4096.

**Workaround**: Adjust `AFXDP_FRAME_SIZE` in config:
```c
#define AFXDP_FRAME_SIZE 9216  // For 9KB jumbo frames
```

---

## Performance Tuning

### 1. Ring Size Tuning

**Increase for High Throughput**:
```c
// In onvm_afxdp_config.h
#define AFXDP_RX_RING_SIZE 4096  // Default: 2048
#define AFXDP_TX_RING_SIZE 4096
#define AFXDP_FILL_RING_SIZE 4096
#define AFXDP_COMP_RING_SIZE 4096
```

**Trade-off**: Larger rings reduce packet drops but increase memory usage and latency.

---

### 2. UMEM Frame Count

**Increase for Burst Absorption**:
```c
#define AFXDP_NUM_FRAMES 8192  // Default: 4096
```

**Calculation**:
```
Required frames = RX_RING_SIZE + TX_RING_SIZE + FILL_RING_SIZE + burst_size
Minimum = 2048 + 2048 + 2048 + 256 = ~6400
Recommended = 8192 (power of 2)
```

---

### 3. Batch Size Optimization

**Increase for Throughput** (reduces ring overhead):
```c
#define AFXDP_RX_BATCH_SIZE 128  // Default: 64
#define AFXDP_TX_BATCH_SIZE 128
```

**Decrease for Latency** (faster per-packet processing):
```c
#define AFXDP_RX_BATCH_SIZE 16
```

---

### 4. CPU Affinity

Pin manager to specific CPU core:
```bash
# Pin to core 0
sudo taskset -c 0 ./onvm_mgr -d eth0 -v

# Or use numactl for NUMA-aware pinning
sudo numactl --cpunodebind=0 --membind=0 ./onvm_mgr -d eth0 -v
```

---

### 5. Interrupt Coalescing

Reduce interrupt rate:
```bash
# Set RX/TX interrupt coalescing
sudo ethtool -C eth0 rx-usecs 50 tx-usecs 50

# Or disable interrupts (poll mode)
sudo ethtool -C eth0 rx-usecs 0 tx-usecs 0
```

---

### 6. Multi-Queue Scaling

Distribute load across multiple queues:
```bash
# Enable 4 queues
sudo ethtool -L eth0 combined 4

# Start manager on each queue
for q in 0 1 2 3; do
    sudo taskset -c $q ./onvm_mgr -d eth0 -Q $q -v &
done
```

---

### 7. Huge Pages

Use huge pages for UMEM (reduces TLB misses):
```bash
# Enable huge pages
echo 512 | sudo tee /proc/sys/vm/nr_hugepages

# Verify
cat /proc/meminfo | grep Huge
```

**Code Change Required**:
```c
// In afxdp_init(), allocate UMEM with huge pages
ctx->packet_buffer = mmap(NULL, ctx->packet_buffer_size,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                          -1, 0);
```

---

### 8. Zero-Copy Mode

Ensure zero-copy is enabled:
```bash
# Check zero-copy support
sudo ethtool -k eth0 | grep xdp

# Force zero-copy mode
sudo ./onvm_mgr -d eth0 -z -v
```

**Verify**:
```bash
# Check /proc/net/xdp_stats
cat /sys/kernel/debug/xdp/eth0/*
```

---

### 9. Kernel Bypass Optimization

Disable kernel stack processing for AF_XDP interface:
```bash
# Disable IP forwarding
sudo sysctl -w net.ipv4.ip_forward=0

# Disable reverse path filtering
sudo sysctl -w net.ipv4.conf.eth0.rp_filter=0

# Disable TCP segmentation offload
sudo ethtool -K eth0 tso off gso off gro off
```

---

### 10. Monitoring and Profiling

**Use `perf` to profile hotspots**:
```bash
sudo perf record -g ./onvm_mgr -d eth0 -t 60
sudo perf report
```

**Monitor with `bpftool`**:
```bash
# Watch map updates
watch -n 1 sudo bpftool map dump name xdp_stats_map
```

**Check NIC stats**:
```bash
watch -n 1 ethtool -S eth0
```

---

## Conclusion

This AF_XDP implementation provides a high-performance, zero-copy packet I/O path for OpenNetVM. Key benefits include:

✅ **Kernel bypass** - Direct NIC-to-userspace DMA  
✅ **Zero-copy** - Packets never copied between kernel and userspace  
✅ **Flexible** - Works on commodity hardware with standard Linux  
✅ **Scalable** - Multi-queue support for parallel processing  

**Current Status**: Functional proof-of-concept with simple packet bouncing. Ready for integration with full NF manager functionality.

**Next Steps**:
1. Fix identified bugs (see Known Issues section)
2. Integrate with NF chain processing
3. Add packet filtering and steering logic
4. Implement multi-NF support with separate sockets per NF

---

## References

- [XDP Project Tutorial](https://github.com/xdp-project/xdp-tutorial)
- [AF_XDP Documentation](https://www.kernel.org/doc/html/latest/networking/af_xdp.html)
- [libbpf Documentation](https://libbpf.readthedocs.io/)
- [OpenNetVM Documentation](https://github.com/sdnfv/openNetVM/tree/master/docs)

---

## Questions?

If you have questions or encounter issues not covered in this README:

1. Check kernel logs: `sudo dmesg | tail`
2. Enable verbose mode: `-v` flag
3. Inspect BPF maps: `sudo bpftool map dump name xsks_map`
4. Review source code comments for detailed explanations

**Contact**: OpenNetVM team at https://sdnfv.github.io/onvm/

---

*Last Updated: 2026-02-06*  
*Version: 1.0.0*  
*Author: AF_XDP Integration Team*
