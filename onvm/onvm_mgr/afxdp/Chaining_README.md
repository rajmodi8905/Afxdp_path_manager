# AF_XDP NF Chaining — Technical Reference

## Overview

The AF_XDP manager implements NF (Network Function) chaining over a shared UMEM packet buffer. Packets arrive from the NIC via an AF_XDP socket, traverse a chain of in-process NF handlers through per-NF ring pairs (RX + TX), and exit via the same socket for NIC egress.

Two ring backends are supported. The backend is selected at compile time via `AFXDP_DEFAULT_RING_BACKEND` in `onvm_afxdp_config.h`:

| Constant | Value | Backend |
|---|---|---|
| `AFXDP_RING_BACKEND_CUSTOM` | 1 | Custom lockfree SPSC ring (`onvm_afxdp_ring.c`) |
| `AFXDP_RING_BACKEND_RTE` | 0 | DPDK `rte_ring` (requires DPDK linkage) |

---

## File Map

| File | Role |
|---|---|
| `onvm_afxdp_config.h` | All compile-time constants: ring sizes, batch sizes, thread counts, backend selector |
| `onvm_afxdp_types.h` | Data structures: `afxdp_pkt_holder`, `afxdp_nf`, `afxdp_chain_ctx`, `afxdp_manager_ctx` |
| `onvm_afxdp_ring.h / .c` | Custom SPSC ring implementation |
| `onvm_afxdp_chain.h / .c` | Chain init, per-NF RX/TX processing, action routing, teardown |
| `onvm_afxdp.h / .c` | Top-level manager: UMEM, XSK socket, XDP program, thread launch, egress submission |
| `onvm_afxdp_nf_registry.h / .c` | NF type registry for runtime NF selection via `-C` flag |
| `onvm_afxdp_pkt_helper.h` | Inline helpers for raw packet data access (`afxdp_pkt_ipv4_hdr`, etc.) |
| `nfs/afxdp_*.c` | Native NFs (`simple_forward`, `firewall`, `bridge`) auto-registering at startup |
| `af_xdp_kern.c` | eBPF XDP kernel program (redirects packets to the XSKMAP) |

---

## Packet Path (Both Modes)

```
NIC RX
  │
  ▼
XDP program (kernel) ──redirect──► AF_XDP RX ring
  │
  ▼
afxdp_handle_receive()
  ├─ allocate afxdp_pkt_holder from holder pool
  ├─ populate holder with (umem_addr, len, ACTION_NEXT)
  └─ enqueue holder to NF[0].rx_ring
        │
        ▼
   ┌─────────────────────────────────────┐
   │  NF Chain (per-NF: rx_ring → handler → tx_ring)  │
   └─────────────────────────────────────┘
        │
        ▼
  Action routing on tx_ring drain:
    ACTION_NEXT  → enqueue to next NF's rx_ring
    ACTION_TONF  → enqueue to target NF's rx_ring
    ACTION_OUT   → collect in egress array
    ACTION_DROP  → free UMEM frame + return holder
        │
        ▼
afxdp_submit_egress() → XSK TX ring → NIC TX
```

---

## `onvm_afxdp_chain.c` — Function Reference

### Holder Pool

| Function | Description |
|---|---|
| `afxdp_holder_alloc(chain)` | Pop a free `afxdp_pkt_holder` from the stack-based free-list. Returns `NULL` if pool exhausted. |
| `afxdp_holder_free(chain, holder)` | Push a holder back onto the free-list. |

The pool is pre-allocated at chain init with `AFXDP_PKT_HOLDER_POOL_SIZE` (4096) entries. A `holder_free_stack[]` array stores indices of available holders; `holder_free_count` tracks the stack top.

### `afxdp_chain_init(ctx, num_nfs)`

1. Allocates `afxdp_chain_ctx`.
2. Sets `ring_backend` from `AFXDP_DEFAULT_RING_BACKEND`.
3. Allocates the holder pool and populates the free stack.
4. For each NF (0 … num_nfs − 1):
   - If **RTE backend**: creates two `rte_ring` instances (`afxdp_nfN_rx`, `afxdp_nfN_tx`) with `RING_F_SP_ENQ | RING_F_SC_DEQ` flags (single-producer/single-consumer). Sets `rx_ring_custom = NULL`, `tx_ring_custom = NULL`.
   - If **Custom backend**: creates two `afxdp_nf_ring` instances (`nfN_rx`, `nfN_tx`) via `afxdp_ring_create()`. Sets `rx_ring = NULL`, `tx_ring = NULL`.
5. Registers `afxdp_simple_forward_handler` as the handler callback for every NF.
6. Sets `chain_order[i] = i` (linear chain).
7. Stores the chain on `ctx->chain`.

Failure at any NF causes rollback cleanup of all previously created rings.

### `afxdp_chain_rx_nf(chain, nf_idx)` — static, Custom mode only

Per-NF RX processing (called by `afxdp_chain_forward`):

1. Dequeue burst from `nf->rx_ring_custom`.
2. Update `nf->stats.rx_packets` and `rx_bytes`.
3. Call `nf->handler(pkt, nf)` — the handler sets `pkt->meta.action`.
4. Enqueue the processed holder to `nf->tx_ring_custom`.
5. On enqueue failure (TX ring full): free UMEM frame + return holder to pool, increment `dropped`.

### `afxdp_chain_tx_nf(chain, nf_idx, egress_holders, max_egress, egress_count)` — static, Custom mode only

Per-NF TX drain and action routing (called by `afxdp_chain_forward`):

1. Dequeue burst from `nf->tx_ring_custom`.
2. For each packet, route by `pkt->meta.action`:
   - **`ACTION_NEXT`**: Enqueue to `nfs[chain_order[next_pos]].rx_ring_custom`. If this NF is the last in the chain, treat as `ACTION_OUT`.
   - **`ACTION_OUT`**: Append to `egress_holders[]` if space remains.
   - **`ACTION_TONF`**: Enqueue to `nfs[destination].rx_ring_custom` if destination is valid and active.
   - **`ACTION_DROP`** (default): Free UMEM frame + return holder.
3. On any failed enqueue: free UMEM frame + return holder, increment `dropped`.
4. Returns the updated `egress_count`.

### `afxdp_chain_forward(chain, egress_holders, max_egress)`

Runs up to `chain_length` passes. Each pass:

1. RX side: call `afxdp_chain_rx_nf()` for every NF in chain order.
2. TX side: call `afxdp_chain_tx_nf()` for every NF in chain order.
3. Check if any NF still has pending packets in its `rx_ring_custom`. If none, break early.

Multiple passes are necessary because `ACTION_NEXT` / `ACTION_TONF` can inject packets into downstream NFs that were already drained in the current pass. The worst case is `chain_length` passes (a packet at position 0 that hops through every NF one position per pass).

Returns the total number of egress holders collected.

### `afxdp_chain_print_stats(chain)`

Prints per-NF RX/TX/Dropped counters to stdout.

### `afxdp_chain_teardown(ctx)`

1. Prints final chain stats.
2. For each NF: drains both rings (freeing UMEM frames and returning holders), then frees the ring resources.
3. Frees the holder pool and free stack.
4. Frees the chain context, sets `ctx->chain = NULL`.

---

## `onvm_afxdp_ring.c` — Custom SPSC Ring

### Structure (`afxdp_nf_ring`)

```c
struct afxdp_nf_ring {
    uint32_t          size;              /* slots (power of 2)          */
    uint32_t          mask;              /* size − 1                    */
    volatile uint32_t head  __aligned(64);  /* producer index           */
    volatile uint32_t tail  __aligned(64);  /* consumer index           */
    void             *ring[] __aligned(64); /* flexible array of void*  */
};
```

- `size` is always a power of 2 (rounded up from the requested `count + 1`). One slot is reserved as a sentinel to distinguish full from empty, so usable capacity = `size − 1`.
- `head` and `tail` are cache-line aligned (64 bytes) to avoid false sharing between producer and consumer.
- Data is stored as `void*` pointers. In practice these are always `afxdp_pkt_holder *`.

### Memory Ordering

| Operation | Side | Ordering |
|---|---|---|
| Enqueue: read `tail` | Producer | `__ATOMIC_ACQUIRE` — see consumer's latest progress |
| Enqueue: write `head` | Producer | `__ATOMIC_RELEASE` — make data visible before advancing head |
| Dequeue: read `head` | Consumer | `__ATOMIC_ACQUIRE` — see producer's latest progress |
| Dequeue: write `tail` | Consumer | `__ATOMIC_RELEASE` — make data consumption visible to producer |

No locks, no CAS. Correctness relies on the SPSC invariant: exactly one thread writes `head`, exactly one thread writes `tail`.

### Function Reference

| Function | Description |
|---|---|
| `afxdp_ring_create(name, count)` | Allocate a ring with `count` usable slots (rounded up to power of 2 + 1 sentinel). |
| `afxdp_ring_free(r)` | Free the ring allocation. |
| `afxdp_ring_enqueue(r, obj)` | Enqueue one object. Returns 0 on success, −1 if full. |
| `afxdp_ring_dequeue(r, &obj)` | Dequeue one object. Returns 0 on success, −1 if empty. |
| `afxdp_ring_dequeue_burst(r, objs, max)` | Dequeue up to `max` objects. Returns actual count dequeued. |
| `afxdp_ring_count(r)` | Current number of objects in the ring. |
| `afxdp_ring_free_count(r)` | Number of free slots. |

---

## Threading Model

### Custom SPSC Mode — 3 threads total

| # | Thread | Function | Role |
|---|---|---|---|
| 1 | RX thread | `afxdp_rx_thread_main` → `afxdp_rx_and_process` → `afxdp_handle_receive` | Polls XSK RX ring, wraps packets into holders, enqueues to NF[0].rx_ring_custom, runs `afxdp_chain_forward()` inline, calls `afxdp_submit_egress()` to place egress on XSK TX ring, refills Fill ring, completes TX. |
| 2 | Mgr thread | `afxdp_mgr_thread_main` | Periodic stats printing, TTL / packet-limit enforcement, sets `global_exit`. |
| 3 | Wakeup thread | `afxdp_wakeup_thread_main` | Sleeps 200 ms in a loop; ensures SIGINT/SIGTERM is processed even during busy-wait. |

In Custom mode, the TX thread is still spawned but its body is entirely wrapped in `#if (AFXDP_DEFAULT_RING_BACKEND == AFXDP_RING_BACKEND_RTE)`, so it exits immediately with a no-op. The chain forwarding and TX submission are done inline on the RX thread, making the datapath single-threaded.

**Total active threads: 3** (RX, Mgr, Wakeup). The TX thread is launched but does no work.

### RTE (`rte_ring`) Mode — 3 + 1 + N threads total

| # | Thread | Function | Role |
|---|---|---|---|
| 1 | RX thread | `afxdp_rx_thread_main` → `afxdp_handle_receive` | Polls XSK RX ring, wraps packets into holders, enqueues to NF[0].rx_ring (rte_ring). Does **not** call `afxdp_chain_forward()` — forwarding is decoupled. |
| 2 | TX thread | `afxdp_tx_thread_main` | Drains each NF's `tx_ring` (rte_ring), routes by action (NEXT/OUT/TONF/DROP), submits egress to XSK TX ring, calls `afxdp_complete_tx()`. |
| 3 | Mgr thread | `afxdp_mgr_thread_main` | Same as Custom mode. |
| 4 | Wakeup thread | `afxdp_wakeup_thread_main` | Same as Custom mode. |
| 5..4+N | NF threads (×N) | `afxdp_dummy_nf_thread` / `afxdp_real_nf_thread` | One per NF. For Real NFs (`-C` flag), dequeues from `rx_ring`, runs native `pkt_handler` (which sets action), enqueues to `tx_ring`. Without `-C`, Dummy NFs just set `ACTION_NEXT`. |

With the default chain length of 2 NFs:

**Total active threads: 6** (1 RX + 1 TX + 2 NF + 1 Mgr + 1 Wakeup).

### Thread Count Summary

| Mode | RX | TX | NF | Mgr | Wakeup | Total |
|---|---|---|---|---|---|---|
| Custom SPSC | 1 | 0 (no-op) | 0 (inline) | 1 | 1 | **3** |
| RTE `rte_ring` (N NFs) | 1 | 1 | N | 1 | 1 | **4 + N** |

---

## Mode-Specific Workflow

### Custom SPSC Mode

```
[RX Thread]
  poll/busy-wait XSK RX ring
  for each packet:
    holder = afxdp_holder_alloc()
    holder.desc = {umem_addr, len}
    holder.meta.action = ACTION_NEXT
    afxdp_ring_enqueue(NF[0].rx_ring_custom, holder)

  egress[] = afxdp_chain_forward(chain)   ← runs inline
    └─ for each pass:
         for each NF: afxdp_chain_rx_nf()  (dequeue rx → handler → enqueue tx)
         for each NF: afxdp_chain_tx_nf()  (dequeue tx → route by action)

  afxdp_submit_egress(egress[])            ← writes XSK TX ring
  refill Fill ring
  afxdp_complete_tx()                      ← drain Completion ring
```

The entire datapath (RX → NF processing → action routing → TX) executes on a single thread. SPSC correctness is trivially satisfied since only one thread touches each ring.

### RTE `rte_ring` Mode

```
[RX Thread]                        [NF Thread i]                  [TX Thread]
  poll XSK RX ring                   while (!exit):                 while (!exit):
  for each packet:                     dequeue NF[i].rx_ring          for each NF:
    holder = alloc()                   handler(pkt)                     dequeue NF.tx_ring
    enqueue NF[0].rx_ring              pkt.action = NEXT                route by action:
                                       enqueue NF[i].tx_ring              NEXT → NF[j].rx_ring
                                                                          OUT  → submit_egress()
                                                                          TONF → NF[d].rx_ring
                                                                          DROP → free
                                                                   afxdp_complete_tx()
```

The rings are created with `RING_F_SP_ENQ | RING_F_SC_DEQ` (single-producer/single-consumer). The producer/consumer pairs per ring:

| Ring | Producer | Consumer |
|---|---|---|
| `NF[0].rx_ring` | RX thread | NF[0] thread |
| `NF[i].rx_ring` (i > 0) | TX thread | NF[i] thread |
| `NF[i].tx_ring` | NF[i] thread | TX thread |

---

## Key Data Structures

### `afxdp_pkt_holder`

The object that flows through the NF rings:

```c
struct afxdp_pkt_holder {
    struct afxdp_nf_desc  desc;   // { umem_addr, len }
    struct afxdp_pkt_meta meta;   // { action, chain_index, destination, flags }
};
```

Allocated from a pre-allocated pool (`holder_pool[]` + `holder_free_stack[]`). No `malloc`/`free` in the hot path.

### `afxdp_nf`

```c
struct afxdp_nf {
    uint16_t            nf_id;
    uint16_t            chain_position;
    struct afxdp_nf_ring *rx_ring_custom;   // Custom mode
    struct afxdp_nf_ring *tx_ring_custom;   // Custom mode
    void                *rx_ring;           // rte_ring* (RTE mode)
    void                *tx_ring;           // rte_ring* (RTE mode)
    afxdp_nf_handler_fn  handler;           // Custom mode inline path
    struct afxdp_nf_function_table *function_table; // Real NF threads
    void                *packet_buffer;     // UMEM buffer base
    void                *nf_state;          // NF-private state
    struct afxdp_nf_stats stats;
    bool                 active;
};
```

Only the pointer set matching the active backend is non-NULL at runtime. The other pair is always NULL.

### NF Actions

| Constant | Value | Meaning |
|---|---|---|
| `AFXDP_NF_ACTION_DROP` | 0 | Drop packet, reclaim UMEM frame |
| `AFXDP_NF_ACTION_NEXT` | 1 | Forward to next NF in chain order |
| `AFXDP_NF_ACTION_TONF` | 2 | Forward to NF specified in `meta.destination` |
| `AFXDP_NF_ACTION_OUT` | 3 | Transmit to NIC (egress) |

---

## Native NF Integration and Runtime Registration

NFs natively operate directly on `afxdp_pkt_holder` structures to avoid per-packet allocation overhead. NFs are registered dynamically and chains are configured at runtime.

1. **NF Function Table**: Each NF type defines an `afxdp_nf_function_table` with `pkt_handler`, and optional `setup` / `teardown`.
2. **Auto-Registration**: NFs use `__attribute__((constructor))` to auto-register their function table into the `nf_registry` using `afxdp_nf_register_type()`.
3. **CLI Configuration**: The `-C` flag defines the chain (e.g., `-C "simple_forward,firewall"`). `afxdp_chain_init_from_spec()` resolves these names, sets up the rings, and assigns function tables.
4. **Packet Access**: NF handlers access raw packet bytes using inline helper functions in `onvm_afxdp_pkt_helper.h` (e.g., `afxdp_pkt_ipv4_hdr(pkt, nf)`).

---

## Configuration Constants (Chaining)

| Define | Default | Meaning |
|---|---|---|
| `AFXDP_MAX_CHAIN_LENGTH` | 8 | Maximum NFs in one chain |
| `AFXDP_MAX_NF_TYPES` | 32 | Maximum number of NF types in the registry |
| `AFXDP_MAX_NFS` | 64 | Maximum total NF slots |
| `AFXDP_NF_RING_SIZE` | 1024 | Entries per NF ring (must be power of 2) |
| `AFXDP_NF_RING_BURST` | 64 | Max dequeue burst per ring poll |
| `AFXDP_PKT_HOLDER_POOL_SIZE` | 4096 | Pre-allocated holders (≥ `AFXDP_NUM_FRAMES`) |
| `AFXDP_DEFAULT_RING_BACKEND` | `AFXDP_RING_BACKEND_RTE` (0) | Backend selected at compile time |
| `AFXDP_NUM_RX_THREADS` | 1 | RX thread count |
| `AFXDP_NUM_TX_THREADS` | 1 | TX thread count |
| `AFXDP_NUM_MGR_AUX_THREADS` | 1 | Manager/stats thread count |
| `AFXDP_NUM_WAKEUP_THREADS` | 1 | Wakeup/signal thread count |

---

## UMEM Frame Reclamation

Every drop path (ring full, invalid destination, `ACTION_DROP`) calls both:
1. `afxdp_free_umem_frame(xsk, holder->desc.umem_addr)` — returns the UMEM frame offset to the socket's frame free-list.
2. `afxdp_holder_free(chain, holder)` — returns the holder to the chain's holder pool.

This prevents UMEM exhaustion and holder pool starvation. The same two-step reclamation is performed during `afxdp_chain_teardown()` when draining residual packets from all rings.

---

## Future Work: Dynamic TX Thread Count for the RTE Backend

The current RTE mode spawns exactly `AFXDP_NUM_TX_THREADS` (1) TX threads regardless of chain length or NF count. This creates a bottleneck: a single TX thread must drain `tx_ring` for every NF and perform all egress submission and Completion ring processing. As the chain grows or NF throughput increases, this thread becomes the datapath ceiling.

**Planned refinement**: dynamically determine the TX thread count at `afxdp_run()` time based on:

1. **Chain length** — scale TX threads proportionally (e.g., 1 TX thread per K NFs, where K is tunable).
2. **NF-to-TX-thread affinity** — partition the NF table across TX threads so each TX thread only drains a subset of NF `tx_ring`s. This preserves the SPSC invariant for `rte_ring` (one consumer per ring) and avoids contention.
3. **XSK TX ring serialization** — the single XSK TX ring must still be written by one thread, or a lightweight spinlock / dedicated egress collector must be introduced to merge submissions from multiple TX threads.

Implementation path:
- Replace `AFXDP_NUM_TX_THREADS` with a runtime variable computed as `min(chain_length / K, max_tx_threads)`.
- Each TX thread receives a `(start_nf, end_nf)` range and only drains those NFs' `tx_ring`s.
- Egress packets from each TX thread are collected into per-thread local arrays. A single egress flush (either on a dedicated egress thread or via a shared lock on the XSK TX ring) submits them to the socket.
- The `afxdp_run()` join logic already handles variable-count thread arrays, so minimal structural change is needed.
