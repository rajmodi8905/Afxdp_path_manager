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
        /* Network interface */
        char ifname[IF_NAMESIZE];             /* Interface name (e.g. "eth0") */
        int ifindex;                          /* Interface index from if_nametoindex() */

        /* XDP attach mode */
        enum xdp_attach_mode attach_mode;     /* Native, SKB (generic), or auto */
        __u32 xdp_flags;                      /* XDP flags for socket binding */

        /* Socket binding */
        __u16 xsk_bind_flags;                 /* Copy-mode vs zero-copy flags */
        int xsk_if_queue;                     /* RX queue index to bind to */
        bool xsk_poll_mode;                   /* Use poll() instead of busy-wait */

        /* Custom XDP program */
        char xdp_obj_file[512];               /* Path to compiled .o eBPF object */
        char xdp_prog_name[64];               /* Section/function name in the .o */
        bool custom_xdp_prog;                 /* true if user supplied a custom .o */

        /* Stats */
        int stats_interval;                   /* Seconds between stats output */
        bool verbose;                         /* Enable verbose logging */

        /* Manager limits */
        uint32_t time_to_live;                /* Auto-shutdown after N seconds (0=off) */
        uint64_t pkt_limit;                   /* Auto-shutdown after N packets (0=off) */
};

/**************************** Manager Context *********************************/

/*
 * Top-level context holding all AF_XDP manager state.
 * Passed to init/run/cleanup functions.
 */
struct afxdp_manager_ctx {
        /* Configuration */
        struct afxdp_config cfg;

        /* UMEM region */
        struct afxdp_umem_info *umem;
        void *packet_buffer;                  /* Raw allocated buffer (for free) */
        uint64_t packet_buffer_size;

        /* Primary AF_XDP socket (ingress) */
        struct afxdp_socket_info *xsk_socket;

        /* XDP program handle */
        struct xdp_program *xdp_prog;
        int xsk_map_fd;                       /* fd of the XSKMAP in the kernel */

        /* Threads */
        pthread_t stats_thread;

        /* Shutdown flag */
        volatile bool global_exit;
};

#endif /* _ONVM_AFXDP_TYPES_H_ */
