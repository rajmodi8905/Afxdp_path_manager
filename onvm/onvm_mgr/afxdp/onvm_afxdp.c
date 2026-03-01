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

                              onvm_afxdp.c

    Implementation of the AF_XDP-based NF Manager datapath.

    The manager IS the only NF: it receives packets from the NIC via
    an AF_XDP socket and immediately sends them back out to the NIC.
    This is the simplest useful datapath — a zero-copy bounce:

        NIC RX → XDP redirect → AF_XDP RX ring → TX ring → NIC TX

    It implements:
      - UMEM allocation and frame management (stack-based free-list)
      - XSK socket creation and ring initialization
      - XDP kernel program loading and XSKMAP population
      - RX polling loop: receive → bounce to TX → refill Fill ring
      - TX completion handling (reclaim UMEM frames)
      - Statistics display thread
      - Graceful shutdown and resource cleanup

    Reference: xdp-project/xdp-tutorial advanced03-AF_XDP

******************************************************************************/

#include "onvm_afxdp.h"

#include <sys/mman.h>    /* mmap, munmap, MAP_HUGETLB, MAP_ANONYMOUS */
#include <sys/syscall.h> /* syscall, __NR_mbind                        */

/* MAP_HUGE_2MB: request 2 MiB pages when combined with MAP_HUGETLB.
 * Defined in <linux/mman.h> on modern kernels; provide fallback. */
#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB  (21 << 26)
#endif

/****************************Forward Declarations*****************************/

static void afxdp_parse_args(struct afxdp_config *cfg, int argc, char **argv);
static struct afxdp_umem_info *afxdp_configure_umem(void *buffer, uint64_t size);
static struct afxdp_socket_info *afxdp_configure_socket(struct afxdp_manager_ctx *ctx);
static void afxdp_complete_tx(struct afxdp_socket_info *xsk);
static void afxdp_handle_receive(struct afxdp_manager_ctx *ctx);
static void afxdp_rx_and_process(struct afxdp_manager_ctx *ctx);
static void *afxdp_stats_poll(void *arg);

/* UMEM frame allocator */
static uint64_t afxdp_alloc_umem_frame(struct afxdp_socket_info *xsk);
static void afxdp_free_umem_frame(struct afxdp_socket_info *xsk, uint64_t frame);
static uint64_t afxdp_umem_free_frames(struct afxdp_socket_info *xsk);

/* Packet processing callback (called for each received packet) */
static bool afxdp_process_packet(struct afxdp_socket_info *xsk,
                                 uint64_t addr, uint32_t len);

/* Timing utility */
static uint64_t afxdp_gettime(void);

/* Hugepage UMEM buffer management */
static int  afxdp_get_nic_numa_node(const char *ifname);
static void afxdp_bind_numa(void *addr, uint64_t size, int numa_node);
static int  afxdp_alloc_hugepage_buffer(struct afxdp_manager_ctx *ctx, const char *ifname);

/***********************Signal Handler (module-level)*************************/

/*
 * Pointer to the active manager context, used by the signal handler
 * to set global_exit. Set once during afxdp_init().
 */
static struct afxdp_manager_ctx *g_ctx = NULL;

static void
afxdp_signal_handler(int signum) {
        (void)signum;
        if (g_ctx) {
                g_ctx->global_exit = true;
        }
}

/****************************************************************************
 *
 *                        ARGUMENT PARSING
 *
 ****************************************************************************/

static void
afxdp_print_usage(const char *prog) {
        fprintf(stderr,
                "Usage: %s [options]\n"
                "  -d <ifname>     Network interface to bind (required)\n"
                "  -Q <queue_id>   RX queue index (default: %d)\n"
                "  -S              SKB (generic) XDP mode\n"
                "  -N              Native XDP mode\n"
                "  -c              Force copy mode\n"
                "  -z              Force zero-copy mode\n"
                "  -p              Use poll() instead of busy-wait\n"
                "  -f <file.o>     Custom XDP kernel object file\n"
                "  -P <progname>   XDP program section name\n"
                "  -v              Verbose output (enable stats)\n"
                "  -t <seconds>    Time to live (auto-shutdown)\n"
                "  -l <packets>    Packet limit (auto-shutdown)\n"
                "  -h              Show this help\n",
                prog, AFXDP_DEFAULT_QUEUE_ID);
}

static void
afxdp_parse_args(struct afxdp_config *cfg, int argc, char **argv) {
        int opt;

        /* Set defaults */
        memset(cfg, 0, sizeof(*cfg));
        strncpy(cfg->ifname, AFXDP_DEFAULT_IFNAME, IF_NAMESIZE - 1);
        cfg->ifindex = -1;
        cfg->attach_mode = XDP_MODE_UNSPEC;
        cfg->xdp_flags = 0;
        cfg->xsk_bind_flags = 0;
        cfg->xsk_if_queue = AFXDP_DEFAULT_QUEUE_ID;
        cfg->xsk_poll_mode = false;
        cfg->custom_xdp_prog = false;
        cfg->xdp_obj_file[0] = '\0';
        cfg->xdp_prog_name[0] = '\0';
        cfg->stats_interval = AFXDP_STATS_INTERVAL;
        cfg->verbose = false;
        cfg->time_to_live = 0;
        cfg->pkt_limit = 0;

        /* Reset getopt for re-entrant parsing (manager already parsed EAL args) */
        optind = 1;

        while ((opt = getopt(argc, argv, "d:Q:SNczpf:P:vt:l:h")) != -1) {
                switch (opt) {
                case 'd':
                        strncpy(cfg->ifname, optarg, IF_NAMESIZE - 1);
                        cfg->ifname[IF_NAMESIZE - 1] = '\0';
                        break;
                case 'Q':
                        cfg->xsk_if_queue = atoi(optarg);
                        break;
                case 'S':
                        cfg->attach_mode = XDP_MODE_SKB;
                        cfg->xsk_bind_flags |= XDP_COPY;
                        break;
                case 'N':
                        cfg->attach_mode = XDP_MODE_NATIVE;
                        break;
                case 'c':
                        cfg->xsk_bind_flags |= XDP_COPY;
                        break;
                case 'z':
                        cfg->xsk_bind_flags |= XDP_ZEROCOPY;
                        break;
                case 'p':
                        cfg->xsk_poll_mode = true;
                        break;
                case 'f':
                        strncpy(cfg->xdp_obj_file, optarg, sizeof(cfg->xdp_obj_file) - 1);
                        cfg->custom_xdp_prog = true;
                        break;
                case 'P':
                        strncpy(cfg->xdp_prog_name, optarg, sizeof(cfg->xdp_prog_name) - 1);
                        break;
                case 'v':
                        cfg->verbose = true;
                        break;
                case 't':
                        cfg->time_to_live = (uint32_t)atoi(optarg);
                        break;
                case 'l':
                        cfg->pkt_limit = (uint64_t)atoll(optarg);
                        break;
                case 'h':
                default:
                        afxdp_print_usage(argv[0]);
                        exit(EXIT_FAILURE);
                }
        }

        /* Resolve interface index */
        cfg->ifindex = (int)if_nametoindex(cfg->ifname);
        if (cfg->ifindex <= 0) {
                AFXDP_LOG_ERR("Cannot find interface '%s': %s",
                              cfg->ifname, strerror(errno));
                exit(EXIT_FAILURE);
        }

        /* Default XDP program path if custom not specified */
        if (!cfg->custom_xdp_prog) {
                strncpy(cfg->xdp_obj_file, AFXDP_DEFAULT_XDP_OBJ,
                        sizeof(cfg->xdp_obj_file) - 1);
                strncpy(cfg->xdp_prog_name, AFXDP_DEFAULT_XDP_PROG,
                        sizeof(cfg->xdp_prog_name) - 1);
        }

	cfg->custom_xdp_prog = true;

        AFXDP_LOG_INFO("Configuration:");
        AFXDP_LOG_INFO("  Interface:   %s (index %d)", cfg->ifname, cfg->ifindex);
        AFXDP_LOG_INFO("  RX Queue:    %d", cfg->xsk_if_queue);
        AFXDP_LOG_INFO("  XDP Object:  %s", cfg->xdp_obj_file);
        AFXDP_LOG_INFO("  XDP Prog:    %s", cfg->xdp_prog_name);
        AFXDP_LOG_INFO("  Poll Mode:   %s", cfg->xsk_poll_mode ? "yes" : "no");
        AFXDP_LOG_INFO("  Verbose:     %s", cfg->verbose ? "yes" : "no");
        if (cfg->time_to_live)
                AFXDP_LOG_INFO("  TTL:         %u seconds", cfg->time_to_live);
        if (cfg->pkt_limit)
                AFXDP_LOG_INFO("  Pkt Limit:   %lu", cfg->pkt_limit);
}

/****************************************************************************
 *
 *           HUGEPAGE UMEM BUFFER MANAGEMENT
 *
 ****************************************************************************/

/*
 * Read the NUMA node of a network interface from sysfs.
 * Returns AFXDP_NUMA_NODE_UNKNOWN if the file is absent or unreadable
 * (single-socket machine, virtual interface, etc.).
 */
static int
afxdp_get_nic_numa_node(const char *ifname) {
        char path[256];
        FILE *f;
        int node = AFXDP_NUMA_NODE_UNKNOWN;

        snprintf(path, sizeof(path),
                 "/sys/class/net/%s/device/numa_node", ifname);
        f = fopen(path, "r");
        if (f) {
                if (fscanf(f, "%d", &node) != 1)
                        node = AFXDP_NUMA_NODE_UNKNOWN;
                fclose(f);
        }
        return node;
}

/*
 * Bind a virtual memory range to a specific NUMA node using mbind(2).
 * Uses MPOL_BIND | MPOL_MF_MOVE so that any pages already faulted in
 * are migrated to the target node.
 * Uses raw syscall to avoid introducing a libnuma dependency.
 */
static void
afxdp_bind_numa(void *addr, uint64_t size, int numa_node) {
        unsigned long nodemask = 0;

        if (numa_node < 0 || numa_node >= (int)(sizeof(nodemask) * 8))
                return;

        nodemask = 1UL << numa_node;

        /*
         * mbind(addr, len, MPOL_BIND=2, nodemask, maxnode, MPOL_MF_MOVE=2)
         * maxnode must be > the highest set bit index, so sizeof*8+1.
         */
        if (syscall(__NR_mbind, (long)addr, size,
                    2L /* MPOL_BIND */,
                    &nodemask, (unsigned long)(sizeof(nodemask) * 8 + 1),
                    2UL /* MPOL_MF_MOVE */) != 0) {
                AFXDP_LOG_WARN("mbind to NUMA node %d failed: %s",
                               numa_node, strerror(errno));
        } else {
                AFXDP_LOG_INFO("UMEM bound to NUMA node %d", numa_node);
        }
}

/*
 * Allocate the UMEM packet buffer.
 *
 * Tries mmap(MAP_HUGETLB | MAP_HUGE_2MB) first for guaranteed
 * 2 MiB page backing.  Falls back to posix_memalign on failure.
 *
 * If ifname is non-NULL and NUMA node detection succeeds, the buffer
 * is bound to that node via mbind().
 *
 * Sets ctx->packet_buffer, ctx->packet_buffer_size,
 *      ctx->packet_buffer_size_aligned, ctx->use_hugepages.
 */
static int
afxdp_alloc_hugepage_buffer(struct afxdp_manager_ctx *ctx,
                             const char *ifname) {
        uint64_t size = AFXDP_UMEM_HUGEPAGE_ALIGNED;
        void *buf;

        buf = mmap(NULL, size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB,
                   -1, 0);

        if (buf == MAP_FAILED) {
                AFXDP_LOG_WARN("hugepage mmap failed (%s); "
                               "falling back to posix_memalign",
                               strerror(errno));
                if (posix_memalign(&buf, getpagesize(), size) != 0) {
                        AFXDP_LOG_ERR("posix_memalign fallback failed: %s",
                                      strerror(errno));
                        return -ENOMEM;
                }
                ctx->use_hugepages = false;
        } else {
                ctx->use_hugepages = true;
                if (ifname) {
                        int node = afxdp_get_nic_numa_node(ifname);
                        if (node >= 0)
                                afxdp_bind_numa(buf, size, node);
                }
        }

        ctx->packet_buffer              = buf;
        ctx->packet_buffer_size         = (uint64_t)AFXDP_NUM_FRAMES
                                          * AFXDP_FRAME_SIZE;
        ctx->packet_buffer_size_aligned = size;
        return 0;
}

/*
 * Public pre-allocation entry point — called from main() BEFORE
 * rte_eal_init() to claim the UMEM hugepages from the kernel pool.
 *
 * NUMA binding is deferred: the interface name is not yet known here
 * (it comes from AF_XDP argument parsing).  afxdp_init() will apply
 * mbind() once the interface is resolved.
 */
int
afxdp_preallocate_hugepages(struct afxdp_manager_ctx *ctx) {
        int ret;

        if (ctx->packet_buffer) {
                AFXDP_LOG_WARN("afxdp_preallocate_hugepages called twice; "
                               "ignoring second call");
                return 0;
        }

        AFXDP_LOG_INFO("Pre-allocating UMEM buffer (%lu KB) "
                       "before rte_eal_init()",
                       AFXDP_UMEM_HUGEPAGE_ALIGNED / 1024);

        ret = afxdp_alloc_hugepage_buffer(ctx, NULL /* NUMA deferred */);
        if (ret == 0) {
                ctx->hugepage_preallocated = true;
                AFXDP_LOG_INFO("UMEM pre-allocation %s (%lu KB)",
                               ctx->use_hugepages
                                   ? "succeeded (MAP_HUGETLB)"
                                   : "fell back to posix_memalign",
                               ctx->packet_buffer_size_aligned / 1024);
        }
        return ret;
}

/****************************************************************************
 *
 *                   UMEM MANAGEMENT (Shared Packet Buffer)
 *
 *   UMEM is a contiguous memory region registered with the kernel.
 *   It is divided into fixed-size frames. The kernel DMAs incoming
 *   packets directly into these frames (zero-copy) or copies into
 *   them (copy mode).
 *
 *   Frame ownership is tracked by two rings:
 *     Fill Ring:       user tells kernel "these frames are empty, use them"
 *     Completion Ring: kernel tells user  "these TX frames are done"
 *
 *   We manage a stack-based free-list of frame addresses for fast
 *   alloc/free without any locking (single-threaded access).
 *
 ****************************************************************************/

static struct afxdp_umem_info *
afxdp_configure_umem(void *buffer, uint64_t size) {
        struct afxdp_umem_info *umem;
        int ret;

        umem = calloc(1, sizeof(*umem));
        if (!umem) {
                AFXDP_LOG_ERR("Failed to allocate umem info struct");
                return NULL;
        }

        ret = xsk_umem__create(&umem->umem, buffer, size,
                               &umem->fq, &umem->cq, NULL);
        if (ret) {
                AFXDP_LOG_ERR("xsk_umem__create failed: %s", strerror(-ret));
                free(umem);
                return NULL;
        }

        umem->buffer = buffer;
        return umem;
}

/*
 * Allocate one UMEM frame from the free-list.
 * Returns AFXDP_INVALID_UMEM_FRAME if pool is exhausted.
 */
static uint64_t
afxdp_alloc_umem_frame(struct afxdp_socket_info *xsk) {
        uint64_t frame;

        if (xsk->umem_frame_free == 0)
                return AFXDP_INVALID_UMEM_FRAME;

        frame = xsk->umem_frame_addr[--xsk->umem_frame_free];
        xsk->umem_frame_addr[xsk->umem_frame_free] = AFXDP_INVALID_UMEM_FRAME;
        return frame;
}

/*
 * Return a UMEM frame to the free-list.
 */
static void
afxdp_free_umem_frame(struct afxdp_socket_info *xsk, uint64_t frame) {
        assert(xsk->umem_frame_free < AFXDP_NUM_FRAMES);
        xsk->umem_frame_addr[xsk->umem_frame_free++] = frame;
}

/*
 * Return the number of free UMEM frames available.
 */
static uint64_t
afxdp_umem_free_frames(struct afxdp_socket_info *xsk) {
        return xsk->umem_frame_free;
}

/****************************************************************************
 *
 *                  XSK SOCKET CREATION & RING SETUP
 *
 *   An AF_XDP socket (XSK) is bound to a specific (interface, queue)
 *   pair. It has four rings:
 *     RX ring:         kernel → user (received packets)
 *     TX ring:         user → kernel (packets to transmit)
 *     Fill ring:       user → kernel (empty buffers for kernel to fill)
 *     Completion ring: kernel → user (TX buffers kernel is done with)
 *
 *   The socket configuration specifies ring sizes and binding flags
 *   (copy-mode vs zero-copy, XDP flags, etc.).
 *
 ****************************************************************************/

static struct afxdp_socket_info *
afxdp_configure_socket(struct afxdp_manager_ctx *ctx) {
        struct xsk_socket_config xsk_cfg;
        struct afxdp_socket_info *xsk_info;
        struct afxdp_config *cfg = &ctx->cfg;
        uint32_t idx;
        uint32_t i;
        int ret;

        xsk_info = calloc(1, sizeof(*xsk_info));
        if (!xsk_info) {
                AFXDP_LOG_ERR("Failed to allocate xsk_socket_info");
                return NULL;
        }

        xsk_info->umem = ctx->umem;

        /* Configure the socket */
        xsk_cfg.rx_size = AFXDP_RX_RING_SIZE;
        xsk_cfg.tx_size = AFXDP_TX_RING_SIZE;
        xsk_cfg.xdp_flags = cfg->xdp_flags;
        xsk_cfg.bind_flags = cfg->xsk_bind_flags;

        /*
         * If we loaded a custom XDP program, we must inhibit the default
         * XDP program load that xsk_socket__create() would normally do.
         * We will manually insert the socket into our XSKMAP instead.
         */
        xsk_cfg.libbpf_flags = cfg->custom_xdp_prog
                ? XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD
                : 0;

        /* Create the AF_XDP socket */
        ret = xsk_socket__create(&xsk_info->xsk, cfg->ifname,
                                 cfg->xsk_if_queue, ctx->umem->umem,
                                 &xsk_info->rx, &xsk_info->tx, &xsk_cfg);
        if (ret) {
                AFXDP_LOG_ERR("xsk_socket__create failed: %s", strerror(-ret));
                free(xsk_info);
                return NULL;
        }

        /*
         * If using a custom XDP program, manually insert this socket
         * into the XSKMAP so the kernel program can redirect to it.
         */
        if (cfg->custom_xdp_prog) {
                ret = xsk_socket__update_xskmap(xsk_info->xsk, ctx->xsk_map_fd);
                if (ret) {
                        AFXDP_LOG_ERR("xsk_socket__update_xskmap failed: %s",
                                      strerror(-ret));
                        xsk_socket__delete(xsk_info->xsk);
                        free(xsk_info);
                        return NULL;
                }
                AFXDP_LOG_INFO("Socket inserted into XSKMAP (fd=%d)", ctx->xsk_map_fd);
        }

        /* Initialize UMEM frame allocator: all frames start as free */
        for (i = 0; i < AFXDP_NUM_FRAMES; i++)
                xsk_info->umem_frame_addr[i] = i * AFXDP_FRAME_SIZE;
        xsk_info->umem_frame_free = AFXDP_NUM_FRAMES;

        /*
         * Pre-populate the Fill Ring with empty buffers so the kernel
         * has frames to receive packets into immediately.
         */
        ret = xsk_ring_prod__reserve(&xsk_info->umem->fq,
                                     AFXDP_FILL_RING_SIZE, &idx);
        if (ret != (int)AFXDP_FILL_RING_SIZE) {
                AFXDP_LOG_ERR("Failed to reserve fill ring entries: got %d, need %u",
                              ret, AFXDP_FILL_RING_SIZE);
                xsk_socket__delete(xsk_info->xsk);
                free(xsk_info);
                return NULL;
        }

        for (i = 0; i < AFXDP_FILL_RING_SIZE; i++) {
                *xsk_ring_prod__fill_addr(&xsk_info->umem->fq, idx++) =
                        afxdp_alloc_umem_frame(xsk_info);
        }
        xsk_ring_prod__submit(&xsk_info->umem->fq, AFXDP_FILL_RING_SIZE);

        AFXDP_LOG_INFO("AF_XDP socket created on %s queue %d",
                       cfg->ifname, cfg->xsk_if_queue);
        AFXDP_LOG_INFO("  RX ring: %u  TX ring: %u  Fill ring: %u  Comp ring: %u",
                       AFXDP_RX_RING_SIZE, AFXDP_TX_RING_SIZE,
                       AFXDP_FILL_RING_SIZE, AFXDP_COMP_RING_SIZE);
        AFXDP_LOG_INFO("  UMEM frames: %u × %u bytes = %lu KB total",
                       AFXDP_NUM_FRAMES, AFXDP_FRAME_SIZE,
                       ((unsigned long)AFXDP_NUM_FRAMES * AFXDP_FRAME_SIZE) / 1024);

        return xsk_info;
}

/****************************************************************************
 *
 *                      TX COMPLETION HANDLING
 *
 *   After userspace submits descriptors on the TX ring, the kernel
 *   asynchronously transmits them. Once done, the kernel places the
 *   consumed descriptors on the Completion Ring. We must drain the
 *   Completion Ring to reclaim those UMEM frames for reuse.
 *
 ****************************************************************************/

static void
afxdp_complete_tx(struct afxdp_socket_info *xsk) {
        unsigned int completed;
        uint32_t idx_cq;

        if (!xsk->outstanding_tx)
                return;

        /*
         * Kick the kernel to process our TX ring.
         * MSG_DONTWAIT ensures we don't block if kernel is busy.
         */
        sendto(xsk_socket__fd(xsk->xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);

        /* Drain the Completion Ring: reclaim UMEM frames */
        completed = xsk_ring_cons__peek(&xsk->umem->cq,
                                        AFXDP_COMP_RING_SIZE, &idx_cq);
        if (completed > 0) {
                for (unsigned int i = 0; i < completed; i++) {
                        afxdp_free_umem_frame(
                                xsk,
                                *xsk_ring_cons__comp_addr(&xsk->umem->cq, idx_cq++));
                }
                xsk_ring_cons__release(&xsk->umem->cq, completed);
                xsk->outstanding_tx -= (completed < xsk->outstanding_tx)
                        ? completed : xsk->outstanding_tx;
        }
}

/****************************************************************************
 *
 *                      PACKET PROCESSING
 *
 *   The manager IS the only NF. For every received packet:
 *     1. Read it from the RX ring (already done by the caller)
 *     2. Place the same UMEM descriptor on the TX ring to send it
 *        back out through the NIC — zero-copy bounce.
 *
 *   This is the simplest useful datapath: NIC → AF_XDP → NIC.
 *   No packet modification, no chaining, no steering.
 *
 *   Return true  → packet was placed on TX ring (will be sent out)
 *   Return false → TX ring was full; caller frees the frame
 *
 ****************************************************************************/

static bool
afxdp_process_packet(struct afxdp_socket_info *xsk,
                     uint64_t addr, uint32_t len) {
        uint32_t tx_idx = 0;
        int ret;

        /*
         * Reserve one slot on the TX ring.
         * If the ring is full we cannot transmit — return false so the
         * caller frees the UMEM frame instead of leaking it.
         */
        ret = xsk_ring_prod__reserve(&xsk->tx, 1, &tx_idx);
        if (ret != 1) {
                /* TX ring full, drop this packet */
                return false;
        }

        /*
         * Fill the TX descriptor with the same UMEM address and length
         * that we received on the RX side. The packet data is already
         * sitting in the UMEM buffer — no copy needed.
         */
        xsk_ring_prod__tx_desc(&xsk->tx, tx_idx)->addr = addr;
        xsk_ring_prod__tx_desc(&xsk->tx, tx_idx)->len  = len;

        /* Submit the descriptor to the kernel for transmission */
        xsk_ring_prod__submit(&xsk->tx, 1);
        xsk->outstanding_tx++;

        /* Update TX stats */
        xsk->stats.tx_bytes += len;
        xsk->stats.tx_packets++;

        return true;
}

/****************************************************************************
 *
 *                    RX RECEIVE AND PROCESS LOOP
 *
 *   Core packet reception logic. Called repeatedly from the main loop.
 *   Steps:
 *     1. Peek at the RX ring to see how many packets arrived
 *     2. Refill the Fill Ring so kernel has buffers for next batch
 *     3. Process each received packet
 *     4. Release the consumed RX ring entries
 *     5. Complete any outstanding TX operations
 *
 ****************************************************************************/

static void
afxdp_handle_receive(struct afxdp_manager_ctx *ctx) {
        struct afxdp_socket_info *xsk = ctx->xsk_socket;
        unsigned int rcvd, stock_frames, i;
        uint32_t idx_rx = 0, idx_fq = 0;
        int ret;

        /* Step 1: Check how many packets arrived on the RX ring */
        rcvd = xsk_ring_cons__peek(&xsk->rx, AFXDP_RX_BATCH_SIZE, &idx_rx);
        if (!rcvd)
                return;

        /*
         * Step 2: Refill the Fill Ring.
         * We need to give the kernel empty UMEM frames to write the
         * next batch of incoming packets into. We push as many free
         * frames as we have available.
         */
        stock_frames = xsk_prod_nb_free(&xsk->umem->fq,
                                        afxdp_umem_free_frames(xsk));
        if (stock_frames > 0) {
                ret = xsk_ring_prod__reserve(&xsk->umem->fq,
                                             stock_frames, &idx_fq);
                /* Retry until we get all the slots we asked for */
                while (ret != (int)stock_frames) {
                        ret = xsk_ring_prod__reserve(&xsk->umem->fq,
                                                     rcvd, &idx_fq);
                }
                for (i = 0; i < stock_frames; i++) {
                        *xsk_ring_prod__fill_addr(&xsk->umem->fq, idx_fq++) =
                                afxdp_alloc_umem_frame(xsk);
                }
                xsk_ring_prod__submit(&xsk->umem->fq, stock_frames);
        }

        /* Step 3: Process each received packet */
        for (i = 0; i < rcvd; i++) {
                uint64_t addr = xsk_ring_cons__rx_desc(&xsk->rx, idx_rx)->addr;
                uint32_t len  = xsk_ring_cons__rx_desc(&xsk->rx, idx_rx++)->len;

                if (!afxdp_process_packet(xsk, addr, len)) {
                        /* Packet was not forwarded; return frame to pool */
                        afxdp_free_umem_frame(xsk, addr);
                }

                xsk->stats.rx_bytes += len;
        }

        /* Step 4: Release consumed RX entries back to the kernel */
        xsk_ring_cons__release(&xsk->rx, rcvd);
        xsk->stats.rx_packets += rcvd;

        /* Step 5: Complete any outstanding TX operations */
        afxdp_complete_tx(xsk);
}

/****************************************************************************
 *
 *                       MAIN POLLING LOOP
 *
 *   Two modes are supported:
 *     - Busy-wait (default): tight loop calling handle_receive()
 *     - Poll mode (-p flag): uses poll() to sleep until packets arrive,
 *       saving CPU at the cost of some latency
 *
 ****************************************************************************/

static void
afxdp_rx_and_process(struct afxdp_manager_ctx *ctx) {
        struct pollfd fds[1];
        int ret;

        memset(fds, 0, sizeof(fds));
        fds[0].fd = xsk_socket__fd(ctx->xsk_socket->xsk);
        fds[0].events = POLLIN;

        AFXDP_LOG_INFO("Entering main polling loop (mode: %s)",
                       ctx->cfg.xsk_poll_mode ? "poll()" : "busy-wait");

        while (!ctx->global_exit) {
                /* In poll mode, block until there's data */
                if (ctx->cfg.xsk_poll_mode) {
                        ret = poll(fds, 1, 1000); /* 1s timeout */
                        if (ret <= 0)
                                continue;
                }
                afxdp_handle_receive(ctx);

                /* Check auto-shutdown conditions */
                if (ctx->cfg.pkt_limit &&
                    ctx->xsk_socket->stats.rx_packets >= ctx->cfg.pkt_limit) {
                        AFXDP_LOG_INFO("Packet limit reached (%lu), shutting down",
                                       ctx->cfg.pkt_limit);
                        ctx->global_exit = true;
                }
        }
}

/****************************************************************************
 *
 *                      STATISTICS THREAD
 *
 *   Runs in a separate pthread, periodically printing RX/TX counters
 *   and computed rates (pps, Mbps).
 *
 ****************************************************************************/

static uint64_t
afxdp_gettime(void) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

static void
afxdp_stats_print(struct afxdp_stats_record *stats,
                  struct afxdp_stats_record *prev) {
        double period, pps, bps;
        uint64_t packets, bytes;
        const char *fmt = "%-12s %'11lu pkts (%'10.0f pps)"
                          " %'11lu Kbytes (%'6.0f Mbits/s) period:%f\n";

        period = (stats->timestamp - prev->timestamp) / 1000000000.0;
        if (period <= 0)
                period = 1.0;

        /* RX stats */
        packets = stats->rx_packets - prev->rx_packets;
        pps = packets / period;
        bytes = stats->rx_bytes - prev->rx_bytes;
        bps = (bytes * 8) / period / 1000000;
        printf(fmt, "AF_XDP RX:", stats->rx_packets, pps,
               stats->rx_bytes / 1000, bps, period);

        /* TX stats */
        packets = stats->tx_packets - prev->tx_packets;
        pps = packets / period;
        bytes = stats->tx_bytes - prev->tx_bytes;
        bps = (bytes * 8) / period / 1000000;
        printf(fmt, "       TX:", stats->tx_packets, pps,
               stats->tx_bytes / 1000, bps, period);

        printf("\n");
}

static void *
afxdp_stats_poll(void *arg) {
        struct afxdp_manager_ctx *ctx = (struct afxdp_manager_ctx *)arg;
        struct afxdp_socket_info *xsk = ctx->xsk_socket;
        struct afxdp_stats_record previous = { 0 };
        unsigned int interval = ctx->cfg.stats_interval;

        setlocale(LC_NUMERIC, "en_US");
        previous.timestamp = afxdp_gettime();

        while (!ctx->global_exit) {
                sleep(interval);
                xsk->stats.timestamp = afxdp_gettime();
                afxdp_stats_print(&xsk->stats, &previous);
                previous = xsk->stats;
        }
        return NULL;
}

/****************************************************************************
 *
 *                     PUBLIC API IMPLEMENTATION
 *
 ****************************************************************************/

/*
 * afxdp_init() — Full initialization sequence.
 */
int
afxdp_init(struct afxdp_manager_ctx *ctx, int argc, char **argv) {
        struct rlimit rlim = { AFXDP_RLIMIT_MEMLOCK, AFXDP_RLIMIT_MEMLOCK };
        char errmsg[1024];
        int err;

        /* Store global pointer for signal handler */
        g_ctx = ctx;
        ctx->global_exit = false;

        AFXDP_LOG_INFO("========================================");
        AFXDP_LOG_INFO("  openNetVM AF_XDP Manager Initializing");
        AFXDP_LOG_INFO("========================================");

        /* ---- Step 1: Parse command-line arguments ---- */
        afxdp_parse_args(&ctx->cfg, argc, argv);

        /* ---- Step 2: Install signal handlers ---- */
        signal(SIGINT, afxdp_signal_handler);
        signal(SIGTERM, afxdp_signal_handler);

        /* ---- Step 3: Load and attach the XDP kernel program ---- */
        AFXDP_LOG_INFO("Loading XDP program: %s (section: %s)",
                       ctx->cfg.xdp_obj_file, ctx->cfg.xdp_prog_name);

        DECLARE_LIBXDP_OPTS(xdp_program_opts, xdp_opts,
                .open_filename = ctx->cfg.xdp_obj_file,
                .prog_name = ctx->cfg.xdp_prog_name,
        );

        ctx->xdp_prog = xdp_program__create(&xdp_opts);
        err = libxdp_get_error(ctx->xdp_prog);
        if (err) {
                libxdp_strerror(err, errmsg, sizeof(errmsg));
                AFXDP_LOG_ERR("Failed to load XDP program: %s", errmsg);
                return -err;
        }

        err = xdp_program__attach(ctx->xdp_prog, ctx->cfg.ifindex,
                                  ctx->cfg.attach_mode, 0);
        if (err) {
                libxdp_strerror(err, errmsg, sizeof(errmsg));
                AFXDP_LOG_ERR("Failed to attach XDP program to %s: %s",
                              ctx->cfg.ifname, errmsg);
                return err;
        }
        AFXDP_LOG_INFO("XDP program attached to %s", ctx->cfg.ifname);

        /* Find the xsks_map in the loaded BPF object */
        {
                struct bpf_map *map;
                map = bpf_object__find_map_by_name(
                        xdp_program__bpf_obj(ctx->xdp_prog), "xsks_map");
                ctx->xsk_map_fd = bpf_map__fd(map);
                if (ctx->xsk_map_fd < 0) {
                        AFXDP_LOG_ERR("Cannot find xsks_map in BPF object: %s",
                                      strerror(errno));
                        return -ENOENT;
                }
                AFXDP_LOG_INFO("Found xsks_map (fd=%d)", ctx->xsk_map_fd);
        }

        /* ---- Step 4: Raise RLIMIT_MEMLOCK ---- */
        if (setrlimit(RLIMIT_MEMLOCK, &rlim)) {
                AFXDP_LOG_ERR("setrlimit(RLIMIT_MEMLOCK) failed: %s",
                              strerror(errno));
                return -errno;
        }

        /* ---- Step 5: Adopt pre-allocated UMEM buffer or allocate now ---- */
        ctx->packet_buffer_size = (uint64_t)AFXDP_NUM_FRAMES * AFXDP_FRAME_SIZE;
        if (ctx->packet_buffer) {
                /*
                 * Buffer was claimed before rte_eal_init() to partition the
                 * hugepage pool.  Reuse it and apply NUMA binding now that
                 * the interface name is known from argument parsing.
                 */
                if (ctx->use_hugepages) {
                        int numa_node = afxdp_get_nic_numa_node(ctx->cfg.ifname);
                        if (numa_node >= 0)
                                afxdp_bind_numa(ctx->packet_buffer,
                                                ctx->packet_buffer_size_aligned,
                                                numa_node);
                }
                AFXDP_LOG_INFO("Reusing pre-allocated UMEM buffer (%s, %lu KB)",
                               ctx->use_hugepages ? "hugepages" : "normal pages",
                               ctx->packet_buffer_size_aligned / 1024);
        } else {
                /*
                 * No pre-allocation (AF_XDP-only binary without DPDK EAL).
                 * Allocate now with hugepages + NUMA binding.
                 */
                if (afxdp_alloc_hugepage_buffer(ctx, ctx->cfg.ifname) != 0) {
                        AFXDP_LOG_ERR("UMEM buffer allocation failed");
                        return -ENOMEM;
                }
                AFXDP_LOG_INFO("UMEM buffer allocated: %s, %lu KB",
                               ctx->use_hugepages
                                   ? "hugepages (MAP_HUGETLB)"
                                   : "normal pages (fallback)",
                               ctx->packet_buffer_size_aligned / 1024);
        }

        /* ---- Step 6: Configure UMEM ---- */
        ctx->umem = afxdp_configure_umem(ctx->packet_buffer,
                                         ctx->packet_buffer_size);
        if (!ctx->umem) {
                AFXDP_LOG_ERR("UMEM configuration failed");
                /* Buffer freed by caller via afxdp_cleanup(ctx, true) */
                return -ENOMEM;
        }

        /* ---- Step 7: Create AF_XDP socket ---- */
        ctx->xsk_socket = afxdp_configure_socket(ctx);
        if (!ctx->xsk_socket) {
                AFXDP_LOG_ERR("AF_XDP socket creation failed");
                xsk_umem__delete(ctx->umem->umem);
                free(ctx->umem);
                ctx->umem = NULL;
                /* Buffer freed by caller via afxdp_cleanup(ctx, true) */
                return -ENODEV;
        }

        /* ---- Step 8: Start stats thread (if verbose) ---- */
        if (ctx->cfg.verbose) {
                err = pthread_create(&ctx->stats_thread, NULL,
                                     afxdp_stats_poll, ctx);
                if (err) {
                        AFXDP_LOG_ERR("Failed to create stats thread: %s",
                                      strerror(err));
                        /* Non-fatal: continue without stats */
                }
        }

        AFXDP_LOG_INFO("========================================");
        AFXDP_LOG_INFO("  AF_XDP Manager Initialization Complete");
        AFXDP_LOG_INFO("========================================");

        return 0;
}

/*
 * afxdp_run() — Enter the main polling loop.
 */
int
afxdp_run(struct afxdp_manager_ctx *ctx) {
        uint64_t start_time = 0;

        if (ctx->cfg.time_to_live) {
                start_time = afxdp_gettime();
        }

        AFXDP_LOG_INFO("Manager entering main loop...");

        /* Check TTL in a wrapper around the polling loop */
        while (!ctx->global_exit) {
                /* Process one batch of packets */
                afxdp_rx_and_process(ctx);

                /* Check time-to-live */
                if (ctx->cfg.time_to_live) {
                        uint64_t elapsed_ns = afxdp_gettime() - start_time;
                        uint64_t elapsed_s = elapsed_ns / 1000000000ULL;
                        if (elapsed_s >= ctx->cfg.time_to_live) {
                                AFXDP_LOG_INFO("Time to live exceeded (%u s), shutting down",
                                               ctx->cfg.time_to_live);
                                ctx->global_exit = true;
                        }
                }
        }

        AFXDP_LOG_INFO("Main loop exited");
        return 0;
}

/*
 * afxdp_cleanup() — Release AF_XDP resources.
 *
 * When final_cleanup is false (mode switch), only XSK, UMEM registration,
 * and XDP program are torn down; the hugepage buffer stays mapped.
 * When final_cleanup is true (process exit), the buffer is also released.
 */
void
afxdp_cleanup(struct afxdp_manager_ctx *ctx, bool final_cleanup) {
        int err;
        char errmsg[1024];

        AFXDP_LOG_INFO("Cleaning up AF_XDP resources...");

        /* Wait for stats thread to finish */
        if (ctx->cfg.verbose && ctx->stats_thread) {
                pthread_join(ctx->stats_thread, NULL);
        }

        /* Print final statistics */
        if (ctx->xsk_socket) {
                printf("\n--- Final Statistics ---\n");
                printf("RX: %lu packets, %lu bytes\n",
                       ctx->xsk_socket->stats.rx_packets,
                       ctx->xsk_socket->stats.rx_bytes);
                printf("TX: %lu packets, %lu bytes\n",
                       ctx->xsk_socket->stats.tx_packets,
                       ctx->xsk_socket->stats.tx_bytes);
        }

        /* Detach and unload XDP program from the interface */
        if (ctx->xdp_prog) {
                err = xdp_program__detach(ctx->xdp_prog, ctx->cfg.ifindex,
                                          ctx->cfg.attach_mode, 0);
                if (err) {
                        libxdp_strerror(err, errmsg, sizeof(errmsg));
                        AFXDP_LOG_WARN("Failed to detach XDP program: %s", errmsg);
                }
                xdp_program__close(ctx->xdp_prog);
                ctx->xdp_prog = NULL;
                AFXDP_LOG_INFO("XDP program detached from %s", ctx->cfg.ifname);
        }

        /* Delete the AF_XDP socket */
        if (ctx->xsk_socket) {
                xsk_socket__delete(ctx->xsk_socket->xsk);
                free(ctx->xsk_socket);
                ctx->xsk_socket = NULL;
        }

        /* Delete UMEM */
        if (ctx->umem) {
                xsk_umem__delete(ctx->umem->umem);
                free(ctx->umem);
                ctx->umem = NULL;
        }

        /* Release packet buffer only on final process exit.
         * In mode-switch scenarios the hugepage mapping is kept alive
         * so it can be re-used when AF_XDP mode restarts. */
        if (final_cleanup && ctx->packet_buffer) {
                if (ctx->use_hugepages)
                        munmap(ctx->packet_buffer,
                               ctx->packet_buffer_size_aligned);
                else
                        free(ctx->packet_buffer);
                ctx->packet_buffer = NULL;
        }

        AFXDP_LOG_INFO("Cleanup complete");
}
