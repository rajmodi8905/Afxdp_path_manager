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
#include "onvm_afxdp_chain.h"

#include <rte_eal.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_errno.h>

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
                struct xsk_ring_prod *fq = AFXDP_XSK_FQ(xsk);
                unsigned int stock = xsk_prod_nb_free(fq,
                                                      rcvd);
                if (stock > 0) {
                        int reserved = xsk_ring_prod__reserve(
                                fq, stock, &idx_fq);
                        for (int j = 0; j < reserved; j++) {
                                uint64_t frame = afxdp_alloc_umem_frame(xsk);
                                if (frame == AFXDP_INVALID_UMEM_FRAME)
                                        break;
                                *xsk_ring_prod__fill_addr(fq,
                                                          idx_fq++) = frame;
                        }
                        xsk_ring_prod__submit(fq, reserved);
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
 * DPDK RX thread — receives from AF_XDP Socket 1, copies to rte_mbuf,
 * and enqueues into Port 1's NF chain for processing.
 *
 * If no chain is configured, falls back to loopback mode.
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

                struct afxdp_chain_ctx *chain = ctx->chain_dpdk;
                if (chain && chain->chain_length > 0) {
                        /*
                         * Enqueue rte_mbuf pointers into the first NF's
                         * rx_ring.  The DPDK NF thread will dequeue,
                         * process, and enqueue to tx_ring.
                         */
                        uint16_t first_nf_id = chain->chain_order[0];
                        struct afxdp_nf *nf = &chain->nfs[first_nf_id];

                        unsigned enqueued = rte_ring_enqueue_burst(
                                (struct rte_ring *)nf->rx_ring,
                                (void **)bufs, nb_rx, NULL);

                        /* Free mbufs that couldn't be enqueued (ring full) */
                        for (uint16_t i = enqueued; i < nb_rx; i++)
                                rte_pktmbuf_free(bufs[i]);
                } else {
                        /* Fallback: loopback when no chain configured */
                        uint16_t nb_tx = combined_dpdk_tx(ctx, bufs, nb_rx);
                        for (uint16_t i = nb_tx; i < nb_rx; i++)
                                rte_pktmbuf_free(bufs[i]);
                }
        }

        AFXDP_LOG_INFO("DPDK RX thread exiting");
        return NULL;
}
/*
 * DPDK TX thread — drains Port 1 NF chain tx_rings and transmits
 * via AF_XDP Socket 1 TX ring.
 */
void *
combined_dpdk_tx_thread(void *arg) {
        struct afxdp_manager_ctx *ctx = (struct afxdp_manager_ctx *)arg;

        afxdp_set_thread_affinity(AFXDP_BASE_CORE + 5);
        AFXDP_LOG_INFO("DPDK TX thread started on core %d",
                       AFXDP_BASE_CORE + 5);

        while (!ctx->global_exit) {
                struct afxdp_chain_ctx *chain = ctx->chain_dpdk;
                if (!chain) {
                        usleep(100);
                        continue;
                }

                /* Drain each NF's tx_ring and transmit */
                for (uint16_t n = 0; n < chain->chain_length; n++) {
                        struct afxdp_nf *nf = &chain->nfs[chain->chain_order[n]];
                        struct rte_mbuf *bufs[COMBINED_DPDK_BATCH_SIZE];
                        unsigned dequeued;

                        if (!nf->active || !nf->tx_ring)
                                continue;

                        dequeued = rte_ring_dequeue_burst(
                                (struct rte_ring *)nf->tx_ring,
                                (void **)bufs, COMBINED_DPDK_BATCH_SIZE, NULL);

                        if (dequeued == 0)
                                continue;

                        /* Update NF TX stats */
                        for (unsigned j = 0; j < dequeued; j++) {
                                nf->stats.tx_packets++;
                                nf->stats.tx_bytes += rte_pktmbuf_pkt_len(bufs[j]);
                        }

                        uint16_t nb_tx = combined_dpdk_tx(ctx, bufs, dequeued);

                        /* Free unsent packets */
                        for (uint16_t i = nb_tx; i < dequeued; i++) {
                                nf->stats.dropped++;
                                rte_pktmbuf_free(bufs[i]);
                        }
                }
        }

        AFXDP_LOG_INFO("DPDK TX thread exiting");
        return NULL;
}

/*
 * DPDK NF thread — runs a dummy NF processing loop for Port 1.
 *
 * Dequeues rte_mbuf pointers from the NF's rx_ring, "processes" them,
 * and enqueues to the NF's tx_ring for egress.
 *
 * Uses the same arg struct (afxdp_dummy_nf_arg) as Port 0 NF threads.
 * The struct is defined in onvm_afxdp.c but the layout is trivial
 * (ctx pointer + nf_idx), so we redefine it here to avoid header coupling.
 */
struct combined_dpdk_nf_arg {
        struct afxdp_manager_ctx *ctx;
        uint16_t nf_idx;
};

void *
combined_dpdk_nf_thread(void *arg) {
        struct combined_dpdk_nf_arg *nf_arg = (struct combined_dpdk_nf_arg *)arg;
        struct afxdp_manager_ctx *ctx = nf_arg->ctx;
        struct afxdp_chain_ctx *chain = ctx->chain_dpdk;
        struct afxdp_nf *nf = &chain->nfs[nf_arg->nf_idx];

        afxdp_set_thread_affinity(AFXDP_BASE_CORE + 6 + nf_arg->nf_idx);
        AFXDP_LOG_INFO("DPDK NF %u thread started on core %d",
                       nf->nf_id, AFXDP_BASE_CORE + 6 + nf_arg->nf_idx);

        while (!ctx->global_exit) {
                struct rte_mbuf *batch[COMBINED_DPDK_BATCH_SIZE];
                unsigned dequeued, j;

                dequeued = rte_ring_dequeue_burst(
                        (struct rte_ring *)nf->rx_ring,
                        (void **)batch, COMBINED_DPDK_BATCH_SIZE, NULL);

                if (dequeued == 0)
                        continue;

                for (j = 0; j < dequeued; j++) {
                        nf->stats.rx_packets++;
                        nf->stats.rx_bytes += rte_pktmbuf_pkt_len(batch[j]);

                        /*
                         * Dummy NF: pass-through.
                         * A real NF would inspect/modify the mbuf here.
                         */
                }

                /* Enqueue processed mbufs to NF's tx_ring */
                unsigned enqueued = rte_ring_enqueue_burst(
                        (struct rte_ring *)nf->tx_ring,
                        (void **)batch, dequeued, NULL);

                /* Free mbufs that couldn't be enqueued */
                for (j = enqueued; j < dequeued; j++) {
                        nf->stats.dropped++;
                        rte_pktmbuf_free(batch[j]);
                }
        }

        AFXDP_LOG_INFO("DPDK NF %u thread exited", nf->nf_id);
        free(nf_arg);
        return NULL;
}
