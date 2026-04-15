/*********************************************************************
 *                     openNetVM
 *              https://sdnfv.github.io
 *
 *   BSD LICENSE
 *
 *   Copyright(c)
 *            2015-2019 George Washington University
 *            2015-2019 University of California Riverside
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * The name of the author may not be used to endorse or promote
 *       products derived from this software without specific prior
 *       written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ********************************************************************/

/******************************************************************************

                            onvm_afxdp_config.h

    Configuration defines, constants, and macros for the AF_XDP datapath.
    This file contains all tunable parameters for UMEM, ring sizes,
    batch processing, and XDP attachment modes.

******************************************************************************/

#ifndef _ONVM_AFXDP_CONFIG_H_
#define _ONVM_AFXDP_CONFIG_H_

/***********************UMEM Configuration************************************/

/* Number of UMEM frames available for packet storage.
 * Each frame holds exactly one packet. Must be a power of 2. */
#define AFXDP_NUM_FRAMES         16384

/* Size of each UMEM frame in bytes.
 * XSK_UMEM__DEFAULT_FRAME_SIZE is typically 4096 (one page). */
#define AFXDP_FRAME_SIZE         XSK_UMEM__DEFAULT_FRAME_SIZE

/* Sentinel value indicating an invalid/unallocated UMEM frame address */
#define AFXDP_INVALID_UMEM_FRAME UINT64_MAX

/*********************Ring Size Configuration**********************************/

/* Number of descriptors in the RX ring (consumer ring, kernel → user).
 * Larger values reduce the chance of packet drops under burst. */
#define AFXDP_RX_RING_SIZE       4096

/* Number of descriptors in the TX ring (producer ring, user → kernel).
 * Should match or exceed the expected TX burst. */
#define AFXDP_TX_RING_SIZE       4096

/* Number of descriptors in the Fill ring.
 * Used by userspace to provide empty buffers for the kernel to fill. */
#define AFXDP_FILL_RING_SIZE     4096

/* Number of descriptors in the Completion ring.
 * Used by the kernel to notify userspace that TX buffers are done. */
#define AFXDP_COMP_RING_SIZE     4096

/********************Batch Processing Configuration***************************/

/* Maximum number of packets to process in a single RX batch.
 * Larger batch sizes amortize syscall and ring overhead but increase latency. */
#define AFXDP_RX_BATCH_SIZE      256

/* Maximum number of packets to process in a single TX batch. */
#define AFXDP_TX_BATCH_SIZE      256

/**********************Stats Configuration************************************/

/* Interval (in seconds) between statistics printouts */
#define AFXDP_STATS_INTERVAL     2

/**********************Threading Configuration********************************/

/* Number of RX threads (one per AF_XDP socket / NIC queue). */
#define AFXDP_NUM_RX_THREADS         1

/* Number of TX threads. Drains NF tx_rings and submits
 * packets to the AF_XDP socket for NIC egress. */
#define AFXDP_NUM_TX_THREADS         1

/* Number of auxiliary management threads (stats, TTL/pkt-limit
 * checks, graceful shutdown coordination). */
#define AFXDP_NUM_MGR_AUX_THREADS    1

/* Number of wakeup threads. Ensures the process can respond
 * to SIGINT/SIGTERM even when the RX thread is in a tight
 * busy-wait loop with no syscall to interrupt. Also serves
 * as a hook for future shared-core NF wakeup support. */
#define AFXDP_NUM_WAKEUP_THREADS     1

/**********************XSKMAP Configuration***********************************/

/* Maximum number of AF_XDP sockets in the XSKMAP (one per RX queue).
 * This must match the max_entries in the kernel-side BPF map definition. */
#define AFXDP_MAX_SOCKETS        64

/**********************XDP Attachment Defaults*********************************/

/* Default network interface name if none is specified */
#define AFXDP_DEFAULT_IFNAME     "eth0"

/* Default RX queue index to bind the AF_XDP socket to */
#define AFXDP_DEFAULT_QUEUE_ID   0

/* Default path where the compiled eBPF kernel object resides.
 * This is loaded and attached to the NIC by the userspace manager. */
#define AFXDP_DEFAULT_XDP_OBJ    "afxdp/af_xdp_kern.o"

/* Default XDP program section name inside the .o ELF file */
#define AFXDP_DEFAULT_XDP_PROG   "xdp_sock_prog"

/**********************Backpressure Thresholds********************************/

/* High watermark: fraction of ring fullness (0.0 - 1.0) above which
 * the manager considers the downstream NF congested. */
#define AFXDP_HIGH_WATERMARK     0.8

/* Low watermark: fraction of ring fullness below which the NF is
 * considered recovered from congestion. */
#define AFXDP_LOW_WATERMARK      0.2

/**********************Resource Limits****************************************/

/* Allow unlimited locking of memory (required for UMEM registration).
 * If your system uses cgroups memory accounting (kernel >= 5.11),
 * you may not need this. */
#define AFXDP_RLIMIT_MEMLOCK     RLIM_INFINITY

/**********************NF Management Constants********************************/

/* Maximum number of NFs supported in AF_XDP mode.
 * Each NF gets its own XSK socket in the XSKMAP. */
#define AFXDP_MAX_NFS            64

/**********************NF Chaining Constants (Phase-1 Active)*****************/

/* Maximum number of NFs in a single static chain. */
#define AFXDP_MAX_CHAIN_LENGTH       8

/* Number of entries in each per-NF SPSC ring. Must be a power of 2. */
#define AFXDP_NF_RING_SIZE           8192

/* Maximum burst size when dequeue-ing from an NF ring. */
#define AFXDP_NF_RING_BURST          256

/* Total number of pre-allocated packet holders.
 * Should be >= AFXDP_NUM_FRAMES so every in-flight UMEM frame
 * can have a holder wrapper. */
#define AFXDP_PKT_HOLDER_POOL_SIZE   16384

/* Size (bytes) of one afxdp_pkt_holder element. */
#define AFXDP_HOLDER_ELEMENT_SIZE    24

/* Total bytes for the embedded holder pool, appended after UMEM frames. */
#define AFXDP_HOLDER_REGION_BYTES    ((uint64_t)AFXDP_PKT_HOLDER_POOL_SIZE * AFXDP_HOLDER_ELEMENT_SIZE)

/* NF action constants — set by the NF handler callback in pkt_meta.action. */
#define AFXDP_NF_ACTION_DROP         0   /* Drop the packet                */
#define AFXDP_NF_ACTION_NEXT         1   /* Forward to next NF in chain    */
#define AFXDP_NF_ACTION_TONF         2   /* Send to a specific NF (by id)  */
#define AFXDP_NF_ACTION_OUT          3   /* Transmit out of NIC (egress)   */

/* Ring backend selection constants. */
#define AFXDP_RING_BACKEND_RTE       0   /* DPDK rte_ring (future)         */
#define AFXDP_RING_BACKEND_CUSTOM    1   /* Custom lockfree SPSC ring      */

/* Default ring backend for this build. */
#define AFXDP_DEFAULT_RING_BACKEND   AFXDP_RING_BACKEND_RTE

/* Maximum number of NF types that can be registered in the NF registry. */
#define AFXDP_MAX_NF_TYPES           32

/**********************Socket Topology (Chain Mode)**************************/

/* Index of the ingress XSK socket (NIC RX, chain entry). */
#define AFXDP_INGRESS_SOCKET_IDX     0

/* Index of the egress XSK socket (NIC TX, chain exit).
 * In Phase-1 ingress and egress share the same socket. */
#define AFXDP_EGRESS_SOCKET_IDX      0

/* Number of XSK sockets used in chain mode.
 * Phase-1: 1 (shared ingress/egress). Future: 2 (separate). */
#define AFXDP_CHAIN_NUM_SOCKETS      1

/**********************Phase-2/3 Future-Ready Parameters*********************/

/* Maximum number of independent service chains. */
#define AFXDP_MAX_SERVICE_CHAINS     16

/* Maximum entries/rules per service chain. */
#define AFXDP_MAX_CHAIN_ENTRIES      32

/* Maximum dynamic NF registration/deregistration events queued. */
#define AFXDP_MAX_DYNAMIC_NF_EVENTS  64

/* Control-ring size for NF registration/deregistration queues. */
#define AFXDP_CTRL_RING_SIZE         256

/* Maximum number of NFs sharing a single core group. */
#define AFXDP_MAX_CORE_GROUP         8

/* Maximum length of a service chain name string. */
#define AFXDP_MAX_CHAIN_NAME_LEN     64

/**********************Logging Macros*****************************************/

#define AFXDP_LOG_INFO(fmt, ...)  fprintf(stdout, "[AFXDP INFO] " fmt "\n", ##__VA_ARGS__)
#define AFXDP_LOG_ERR(fmt, ...)   fprintf(stderr, "[AFXDP ERROR] " fmt "\n", ##__VA_ARGS__)
#define AFXDP_LOG_WARN(fmt, ...)  fprintf(stderr, "[AFXDP WARN] " fmt "\n", ##__VA_ARGS__)

/**********************Hugepage Pool Partitioning*****************************/

/* Size of one x86 large (2 MiB) hugepage in bytes. */
#define AFXDP_HUGEPAGE_SIZE          (2ULL * 1024 * 1024)

/* Raw UMEM size: AFXDP_NUM_FRAMES frames of AFXDP_FRAME_SIZE each. */
#define AFXDP_UMEM_TOTAL_BYTES       ((uint64_t)AFXDP_NUM_FRAMES * AFXDP_FRAME_SIZE)

/* Full buffer size: UMEM frames + embedded holder pool. */
#define AFXDP_BUFFER_TOTAL_BYTES     (AFXDP_UMEM_TOTAL_BYTES + AFXDP_HOLDER_REGION_BYTES)

/* Actual mmap size — rounded up to the next 2 MiB boundary so the allocation
 * covers whole hugepages. Includes both the kernel UMEM region and the
 * userspace-only holder pool appended at the tail. */
#define AFXDP_UMEM_HUGEPAGE_ALIGNED (((AFXDP_BUFFER_TOTAL_BYTES) + AFXDP_HUGEPAGE_SIZE - 1) & ~(AFXDP_HUGEPAGE_SIZE - 1))

/* Maximum memory (MB per NUMA socket) that the DPDK EAL is permitted to claim from the hugepage pool.*/
#define AFXDP_DPDK_SOCKET_MEM_MB     1792

/* Returned by afxdp_get_nic_numa_node() when the NIC's NUMA node cannot be determined (single-socket machine or sysfs unavailable). */
#define AFXDP_NUMA_NODE_UNKNOWN      (-1)

#endif /* _ONVM_AFXDP_CONFIG_H_ */
