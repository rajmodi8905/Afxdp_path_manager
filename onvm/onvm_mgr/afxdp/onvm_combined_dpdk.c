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

                          onvm_combined_dpdk.c

    Thin wrapper: AF_XDP Socket 1 (Port 1, Slice B) <-> rte_mbuf.

    This module bridges the gap between our AF_XDP-managed Socket 1 and
    the DPDK rte_mbuf world used by openNetVM NF processes.  It does NOT
    use DPDK's AF_XDP PMD.  Instead, it directly dequeues from the AF_XDP
    RX ring, copies payloads into rte_mbufs (allocated from a standard
    rte_mempool), and hands them to NFs via rte_ring.  The reverse path
    copies rte_mbuf payloads into Slice B UMEM frames and submits them
    to the AF_XDP TX ring.

    Data flow:
      RX: NIC Port 1 -> XDP -> AF_XDP Socket 1 RX Ring
              -> combined_dpdk_rx() copies to rte_mbuf
              -> rte_ring enqueue to NF rx_q

      TX: NF tx_q -> rte_ring dequeue
              -> combined_dpdk_tx() copies to UMEM Slice B frame
              -> AF_XDP Socket 1 TX Ring -> XDP -> NIC Port 1

******************************************************************************/

#include "onvm_combined_dpdk.h"
#include "onvm_afxdp.h"

#include <rte_eal.h>
#include <rte_lcore.h>
#include <rte_log.h>

/*****************************************************************************
 *  Initialization
 *****************************************************************************/

int
combined_dpdk_init(struct afxdp_manager_ctx *ctx) {
        /*
         * Create a standard rte_mempool for rte_mbuf wrappers.
         * These mbufs hold copies of packets received from AF_XDP Socket 1,
         * allowing openNetVM NFs (which expect rte_mbuf pointers) to process them.
         */
        ctx->dpdk_mempool = rte_pktmbuf_pool_create(
                "combined_dpdk_mbuf_pool",
                COMBINED_DPDK_MBUF_POOL_SIZE,
                256,                             /* cache size */
                0,                               /* priv size */
                RTE_MBUF_DEFAULT_BUF_SIZE,       /* data room size */
                rte_socket_id());

        if (!ctx->dpdk_mempool) {
                AFXDP_LOG_ERR("Failed to create DPDK mbuf pool: %s",
                              rte_strerror(rte_errno));
                return -rte_errno;
        }

        AFXDP_LOG_INFO("Combined DPDK wrapper initialized:");
        AFXDP_LOG_INFO("  mbuf pool: %u mbufs x %u bytes",
                       COMBINED_DPDK_MBUF_POOL_SIZE,
                       (unsigned)RTE_MBUF_DEFAULT_BUF_SIZE);

        return 0;
}

/*****************************************************************************
 *  Packet I/O
 *****************************************************************************/

uint16_t
combined_dpdk_rx(struct afxdp_manager_ctx *ctx,
                 struct rte_mbuf **bufs, uint16_t max_pkts) {
        struct afxdp_socket_info *xsk = ctx->xsk_socket_dpdk;
        uint32_t idx_rx = 0;
        uint32_t idx_fq = 0;
        uint16_t rcvd_out = 0;

        /* Peek at available RX descriptors */
        unsigned int rcvd = xsk_ring_cons__peek(&xsk->rx, max_pkts, &idx_rx);
        if (rcvd == 0)
                return 0;

        /* Process each received packet */
        for (unsigned int i = 0; i < rcvd; i++) {
                const struct xdp_desc *desc =
                        xsk_ring_cons__rx_desc(&xsk->rx, idx_rx + i);
                uint64_t addr = desc->addr;
                uint32_t len  = desc->len;

                /* Allocate rte_mbuf from the DPDK mempool */
                struct rte_mbuf *m = rte_pktmbuf_alloc(ctx->dpdk_mempool);
                if (unlikely(!m)) {
                        /* No mbuf available — drop packet, return frame */
                        afxdp_free_umem_frame(xsk, addr);
                        xsk->stats.rx_dropped++;
                        continue;
                }

                /* Copy packet data from UMEM frame into rte_mbuf */
                void *pkt_data = xsk_umem__get_data(xsk->umem->buffer, addr);
                rte_memcpy(rte_pktmbuf_mtod(m, void *), pkt_data, len);
                m->data_len = len;
                m->pkt_len  = len;

                bufs[rcvd_out++] = m;

                /* Return UMEM frame to Slice B free pool immediately */
                afxdp_free_umem_frame(xsk, addr);

                /* Update stats */
                xsk->stats.rx_packets++;
                xsk->stats.rx_bytes += len;
        }

        /* Release RX ring descriptors */
        xsk_ring_cons__release(&xsk->rx, rcvd);

        /*
         * Refill the Fill Ring with free Slice B frames so the kernel
         * has buffers available for new incoming packets.
         */
        {
                unsigned int stock = xsk_prod_nb_free(&xsk->umem->fq,
                                                      rcvd);
                if (stock > 0) {
                        int reserved = xsk_ring_prod__reserve(
                                &xsk->umem->fq, stock, &idx_fq);
                        for (int j = 0; j < reserved; j++) {
                                uint64_t frame = afxdp_alloc_umem_frame(xsk);
                                if (frame == AFXDP_INVALID_UMEM_FRAME)
                                        break;
                                *xsk_ring_prod__fill_addr(&xsk->umem->fq,
                                                          idx_fq++) = frame;
                        }
                        xsk_ring_prod__submit(&xsk->umem->fq, reserved);
                }
        }

        return rcvd_out;
}

uint16_t
combined_dpdk_tx(struct afxdp_manager_ctx *ctx,
                 struct rte_mbuf **bufs, uint16_t nb_pkts) {
        struct afxdp_socket_info *xsk = ctx->xsk_socket_dpdk;
        uint32_t idx_tx = 0;
        uint16_t sent = 0;

        if (nb_pkts == 0)
                return 0;

        /* Reserve TX ring slots */
        unsigned int reserved = xsk_ring_prod__reserve(
                &xsk->tx, nb_pkts, &idx_tx);

        for (unsigned int i = 0; i < reserved; i++) {
                /* Allocate a free UMEM frame from Slice B */
                uint64_t frame = afxdp_alloc_umem_frame(xsk);
                if (frame == AFXDP_INVALID_UMEM_FRAME) {
                        /* No free frames — stop submitting */
                        break;
                }

                /* Copy packet payload from rte_mbuf into UMEM frame */
                void *dst = xsk_umem__get_data(xsk->umem->buffer, frame);
                uint32_t pkt_len = bufs[i]->pkt_len;
                rte_memcpy(dst, rte_pktmbuf_mtod(bufs[i], void *), pkt_len);

                /* Fill TX descriptor */
                struct xdp_desc *tx_desc =
                        xsk_ring_prod__tx_desc(&xsk->tx, idx_tx + sent);
                tx_desc->addr = frame;
                tx_desc->len  = pkt_len;
                sent++;

                /* Free the rte_mbuf (NF is done with it) */
                rte_pktmbuf_free(bufs[i]);

                /* Update stats */
                xsk->stats.tx_packets++;
                xsk->stats.tx_bytes += pkt_len;
        }

        /* Submit TX descriptors to the ring */
        if (sent > 0) {
                xsk_ring_prod__submit(&xsk->tx, sent);
                xsk->outstanding_tx += sent;
        }

        /* Kick kernel + drain Completion Ring to reclaim frames */
        afxdp_complete_tx(xsk);

        /* Count unsent packets as dropped */
        for (unsigned int i = sent; i < nb_pkts; i++) {
                rte_pktmbuf_free(bufs[i]);
                xsk->stats.tx_dropped++;
        }

        return sent;
}

/*****************************************************************************
 *  Worker Threads
 *****************************************************************************/

/*
 * DPDK RX thread — polls AF_XDP Socket 1 and distributes packets
 * to openNetVM NFs via rte_ring.
 *
 * For the initial version, this is a simple forwarding loop that
 * receives packets and immediately transmits them back out Port 1.
 * The openNetVM NF routing (rte_ring to NF rx_q) will be wired in
 * once the NF manager infrastructure for Port 1 is set up.
 */
void *
combined_dpdk_rx_thread(void *arg) {
        struct afxdp_manager_ctx *ctx = (struct afxdp_manager_ctx *)arg;
        struct rte_mbuf *bufs[COMBINED_DPDK_BATCH_SIZE];

        afxdp_set_thread_affinity(AFXDP_BASE_CORE + 4);
        AFXDP_LOG_INFO("DPDK RX thread started on core %d",
                       AFXDP_BASE_CORE + 4);

        while (!ctx->global_exit) {
                uint16_t nb_rx = combined_dpdk_rx(ctx, bufs,
                                                  COMBINED_DPDK_BATCH_SIZE);
                if (nb_rx == 0)
                        continue;

                /*
                 * Phase 1: Simple loopback — forward all received packets
                 * back out Port 1.  This validates the RX+TX pipeline.
                 *
                 * TODO(Phase 2): Route to openNetVM NF rx_q via rte_ring:
                 *   for (i = 0; i < nb_rx; i++) {
                 *       uint16_t dest = onvm_get_pkt_destination(bufs[i]);
                 *       rte_ring_enqueue(nfs[dest].rx_q, bufs[i]);
                 *   }
                 */
                uint16_t nb_tx = combined_dpdk_tx(ctx, bufs, nb_rx);

                /* Free unsent packets */
                for (uint16_t i = nb_tx; i < nb_rx; i++)
                        rte_pktmbuf_free(bufs[i]);
        }

        AFXDP_LOG_INFO("DPDK RX thread exiting");
        return NULL;
}

/*
 * DPDK TX thread — drains openNetVM NF tx_q rings and transmits
 * via AF_XDP Socket 1 TX ring.
 *
 * This thread is NOT active in Phase 1 (simple loopback mode).
 * It will be activated when openNetVM NF routing is wired in.
 */
void *
combined_dpdk_tx_thread(void *arg) {
        struct afxdp_manager_ctx *ctx = (struct afxdp_manager_ctx *)arg;
        (void)ctx;  /* Unused in Phase 1 */

        afxdp_set_thread_affinity(AFXDP_BASE_CORE + 5);
        AFXDP_LOG_INFO("DPDK TX thread started on core %d",
                       AFXDP_BASE_CORE + 5);

        while (!ctx->global_exit) {
                /*
                 * TODO(Phase 2): Drain NF tx_q rings and call
                 * combined_dpdk_tx() to transmit out Port 1.
                 *
                 * for (i = 0; i < num_dpdk_nfs; i++) {
                 *     nb = rte_ring_dequeue_burst(nfs[i].tx_q, bufs, ...);
                 *     combined_dpdk_tx(ctx, bufs, nb);
                 * }
                 */
                usleep(100);  /* Idle wait in Phase 1 */
        }

        AFXDP_LOG_INFO("DPDK TX thread exiting");
        return NULL;
}
