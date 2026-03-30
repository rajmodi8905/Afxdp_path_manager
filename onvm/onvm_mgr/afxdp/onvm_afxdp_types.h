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

                            onvm_afxdp_types.h

    Data structures for the AF_XDP datapath. Includes UMEM management,
    XSK socket state, per-socket statistics, and runtime configuration.

******************************************************************************/

#ifndef _ONVM_AFXDP_TYPES_H_
#define _ONVM_AFXDP_TYPES_H_

/********************************System Headers*******************************/

#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <locale.h>
#include <getopt.h>

#include <sys/resource.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <linux/if_link.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/tcp.h>

/*****************************AF_XDP / BPF Headers****************************/

#include <bpf/bpf.h>
#include <xdp/xsk.h>
#include <xdp/libxdp.h>

/*****************************Internal Headers********************************/

#include "onvm_afxdp_config.h"

/**************************** UMEM Info ***************************************/

/*
 * Manages the shared UMEM (Unified Memory) region.
 *
 * UMEM is the packet buffer pool shared between kernel and userspace.
 * The kernel writes received packets into UMEM frames; userspace reads
 * them from the RX ring. For TX, userspace writes packets into UMEM
 * and the kernel reads them from the TX ring.
 *
 * The Fill Ring (fq) and Completion Ring (cq) manage buffer ownership:
 *   - Fill Ring:       userspace → kernel  ("here are empty frames to fill")
 *   - Completion Ring: kernel → userspace  ("these TX frames are done, reuse them")
 */
struct afxdp_umem_info {
        struct xsk_ring_prod fq;      /* Fill ring (producer: userspace) */
        struct xsk_ring_cons cq;      /* Completion ring (consumer: userspace) */
        struct xsk_umem *umem;        /* libxdp UMEM handle */
        void *buffer;                 /* Raw pointer to the mmap'd UMEM region */
};

/**************************** Socket Stats ************************************/

/*
 * Per-socket packet statistics, sampled periodically by the stats thread.
 */
struct afxdp_stats_record {
        uint64_t timestamp;           /* Timestamp of this measurement (ns) */
        uint64_t rx_packets;          /* Total RX packets received */
        uint64_t rx_bytes;            /* Total RX bytes received */
        uint64_t tx_packets;          /* Total TX packets transmitted */
        uint64_t tx_bytes;            /* Total TX bytes transmitted */
        uint64_t rx_dropped;          /* Packets dropped (no free UMEM frame) */
};

/**************************** XSK Socket Info *********************************/

/*
 * Complete state for a single AF_XDP socket.
 *
 * Each socket is bound to one (interface, queue_id) pair and manages
 * its own RX/TX descriptor rings plus a pool of UMEM frame addresses.
 *
 * Ring layout:
 *   RX ring (consumer): kernel places received packet descriptors here
 *   TX ring (producer): userspace places outgoing packet descriptors here
 *
 * Frame allocator:
 *   umem_frame_addr[] is a simple stack-based free list of UMEM offsets.
 *   umem_frame_free tracks how many free frames remain.
 */
struct afxdp_socket_info {
        /* Descriptor rings */
        struct xsk_ring_cons rx;              /* RX ring (consumer) */
        struct xsk_ring_prod tx;              /* TX ring (producer) */

        /* Back-reference to the shared UMEM */
        struct afxdp_umem_info *umem;

        /* libxdp socket handle */
        struct xsk_socket *xsk;

        /* UMEM frame free-list (stack-based allocator) */
        uint64_t umem_frame_addr[AFXDP_NUM_FRAMES];
        uint32_t umem_frame_free;

        /* Outstanding TX descriptors not yet completed by the kernel */
        uint32_t outstanding_tx;

        /* Live statistics (updated inline during packet processing) */
        struct afxdp_stats_record stats;

        /* Previous stats snapshot (for rate calculations) */
        struct afxdp_stats_record prev_stats;
};

/**************************** Runtime Config **********************************/

/*
 * Runtime configuration for the AF_XDP manager.
 * Populated from command-line arguments at startup.
 */
struct afxdp_config {
        uint64_t pkt_limit;                   /* Auto-shutdown after N packets (0=off) */

        char ifname[IF_NAMESIZE];             /* Interface name (e.g. "eth0") */
        char xdp_obj_file[512];               /* Path to compiled .o eBPF object */
        char xdp_prog_name[64];               /* Section/function name in the .o */

        enum xdp_attach_mode attach_mode;     /* Native, SKB (generic), or auto */
        __u32 xdp_flags;                      /* XDP flags for socket binding */
        uint32_t time_to_live;                /* Auto-shutdown after N seconds (0=off) */
        int ifindex;                          /* Interface index from if_nametoindex() */
        int xsk_if_queue;                     /* RX queue index to bind to */
        int stats_interval;                   /* Seconds between stats output */

        __u16 xsk_bind_flags;                 /* Copy-mode vs zero-copy flags */

        bool xsk_poll_mode;                   /* Use poll() instead of busy-wait */
        bool custom_xdp_prog;                 /* true if user supplied a custom .o */
        bool verbose;                         /* Enable verbose logging */
};

/**************************** NF Chaining Types *******************************/

/*
 * Forward declarations for ring types.
 * The actual struct is defined in onvm_afxdp_ring.h.
 */
struct afxdp_nf_ring;

/*
 * Ring backend selector.
 * Phase-1 uses CUSTOM only (no DPDK linkage).
 * When DPDK is linked, RTE backend can be selected.
 */
enum afxdp_ring_backend {
        AFXDP_RING_BE_RTE    = AFXDP_RING_BACKEND_RTE,
        AFXDP_RING_BE_CUSTOM = AFXDP_RING_BACKEND_CUSTOM,
};

/*
 * Per-packet metadata carried through the NF chain.
 * The NF handler sets `action` to decide what happens next.
 */
struct afxdp_pkt_meta {
        uint8_t  action;          /* AFXDP_NF_ACTION_*                       */
        uint8_t  chain_index;     /* Current position in the chain           */
        uint16_t destination;     /* Target NF id (used with ACTION_TONF)    */
        uint32_t flags;           /* Reserved for future per-packet flags    */
};

/*
 * Descriptor linking a packet to its UMEM location.
 * This is what the ring payload ultimately refers to.
 */
struct afxdp_nf_desc {
        uint64_t umem_addr;       /* UMEM offset of the frame                */
        uint32_t len;             /* Packet length in bytes                  */
        uint32_t _pad;            /* Alignment padding                       */
};

/*
 * Packet holder — the object placed inside NF rings.
 * Combines the UMEM descriptor with NF routing metadata.
 */
struct afxdp_pkt_holder {
        struct afxdp_nf_desc  desc;    /* UMEM address + frame length        */
        struct afxdp_pkt_meta meta;    /* NF action / state metadata         */
};

/*
 * Per-NF statistics counters.
 */
struct afxdp_nf_stats {
        uint64_t rx_packets;      /* Packets dequeued from NF RX ring        */
        uint64_t tx_packets;      /* Packets enqueued to NF TX ring          */
        uint64_t rx_bytes;        /* Total bytes received by this NF         */
        uint64_t tx_bytes;        /* Total bytes transmitted by this NF      */
        uint64_t dropped;         /* Packets dropped by this NF              */
};

/*
 * NF handler callback type.
 * Called for each packet dequeued from the NF's RX ring.
 * The handler inspects/modifies the packet and sets meta.action.
 * Returns 0 on success, negative errno on error.
 */
struct afxdp_nf;  /* forward declaration */
typedef int (*afxdp_nf_handler_fn)(struct afxdp_pkt_holder *pkt,
                                   struct afxdp_nf *nf);

/*
 * NF instance — represents one network function in the chain.
 */
struct afxdp_nf {
        uint16_t nf_id;                           /* Unique NF identifier    */
        uint16_t chain_position;                  /* Position in chain (0-based) */

        /* Custom SPSC ring pointers (Phase-1 active) */
        struct afxdp_nf_ring *rx_ring_custom;     /* Packets waiting for NF  */
        struct afxdp_nf_ring *tx_ring_custom;     /* Packets produced by NF  */

        /* rte_ring pointers (Phase-2 — NULL in Phase-1) */
        void *rx_ring;                            /* struct rte_ring * */
        void *tx_ring;                            /* struct rte_ring * */

        /* NF handler callback */
        afxdp_nf_handler_fn handler;

        /* Per-NF stats */
        struct afxdp_nf_stats stats;

        /* Is this NF slot active? */
        bool active;
};

/*
 * Service chain entry — maps a position to an NF id.
 * Future-ready: not consumed in Phase-1 runtime path.
 */
struct afxdp_service_chain_entry {
        uint16_t nf_id;           /* NF to invoke at this position           */
        uint16_t _reserved;
};

/*
 * Service chain definition.
 * Future-ready: Phase-1 uses a simple linear array in chain_ctx instead.
 */
struct afxdp_service_chain {
        char name[AFXDP_MAX_CHAIN_NAME_LEN];
        struct afxdp_service_chain_entry entries[AFXDP_MAX_CHAIN_ENTRIES];
        uint16_t length;          /* Number of active entries                */
        uint16_t chain_id;
        bool active;
};

/*
 * Chain context — holds all NF chaining state.
 * Owned by the manager context.
 */
struct afxdp_chain_ctx {
        /* NF table (indexed by nf_id) */
        struct afxdp_nf nfs[AFXDP_MAX_CHAIN_LENGTH];

        /* Static chain order — nf_ids in execution sequence */
        uint16_t chain_order[AFXDP_MAX_CHAIN_LENGTH];

        /* Number of NFs in the active chain */
        uint16_t chain_length;

        /* Pre-allocated packet holder pool */
        struct afxdp_pkt_holder *holder_pool;
        uint32_t holder_pool_size;

        /* Simple free-list for holders (stack-based, like UMEM allocator) */
        uint32_t *holder_free_stack;
        uint32_t holder_free_count;

        /* Selected ring backend */
        enum afxdp_ring_backend ring_backend;
};

/**************************** Manager Context *********************************/

/*
 * Top-level context holding all AF_XDP manager state.
 * Passed to init/run/cleanup functions.
 */
struct afxdp_manager_ctx {
        struct afxdp_config cfg;

        struct afxdp_umem_info *umem;
        void *packet_buffer;                  /* Raw allocated buffer             */
        struct afxdp_socket_info *xsk_socket;
        struct xdp_program *xdp_prog;

        uint64_t packet_buffer_size;          /* logical UMEM size (frames×frame) */
        uint64_t packet_buffer_size_aligned;  /* actual mmap size (2 MiB aligned) */
        pthread_t stats_thread;

        int xsk_map_fd;                       /* fd of the XSKMAP in the kernel */

        volatile bool global_exit;
        bool use_hugepages;                   /* true → munmap; false → free()    */
        bool hugepage_preallocated;           /* pre-alloc'd before rte_eal_init()*/

        /* NF Chaining (Phase-1) */
        struct afxdp_chain_ctx *chain;        /* NULL when chaining is disabled   */
};

#endif /* _ONVM_AFXDP_TYPES_H_ */
