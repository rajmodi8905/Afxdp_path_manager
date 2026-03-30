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

                           onvm_afxdp_chain.c

    NF chain management implementation for AF_XDP.

    Handles:
      - Packet holder pool (pre-allocated, stack-based free-list)
      - Per-NF ring creation and registration
      - Chain forwarding loop (RX → handler → action routing → TX)
      - Resource teardown

******************************************************************************/

#include "onvm_afxdp_chain.h"
#include "nfs/afxdp_simple_forward.h"

#include <string.h>
#include <inttypes.h>

/******************************* Holder Pool **********************************/

struct afxdp_pkt_holder *
afxdp_holder_alloc(struct afxdp_chain_ctx *chain) {
        if (chain->holder_free_count == 0)
                return NULL;

        uint32_t idx = chain->holder_free_stack[--chain->holder_free_count];
        return &chain->holder_pool[idx];
}

void
afxdp_holder_free(struct afxdp_chain_ctx *chain,
                  struct afxdp_pkt_holder *holder) {
        uint32_t idx = (uint32_t)(holder - chain->holder_pool);
        chain->holder_free_stack[chain->holder_free_count++] = idx;
}

/******************************* Chain Init ************************************/

int
afxdp_chain_init(struct afxdp_manager_ctx *ctx, uint16_t num_nfs) {
        struct afxdp_chain_ctx *chain;
        uint16_t i;

        if (num_nfs == 0 || num_nfs > AFXDP_MAX_CHAIN_LENGTH) {
                AFXDP_LOG_ERR("Chain init: invalid num_nfs %u (max %d)",
                              num_nfs, AFXDP_MAX_CHAIN_LENGTH);
                return -EINVAL;
        }

        /* Allocate the chain context */
        chain = (struct afxdp_chain_ctx *)calloc(1, sizeof(*chain));
        if (!chain) {
                AFXDP_LOG_ERR("Chain init: failed to allocate chain context");
                return -ENOMEM;
        }

        chain->chain_length = num_nfs;
        chain->ring_backend = AFXDP_DEFAULT_RING_BACKEND;

        /* ---- Allocate packet holder pool ---- */
        chain->holder_pool_size = AFXDP_PKT_HOLDER_POOL_SIZE;
        chain->holder_pool = (struct afxdp_pkt_holder *)calloc(
                chain->holder_pool_size, sizeof(struct afxdp_pkt_holder));
        if (!chain->holder_pool) {
                AFXDP_LOG_ERR("Chain init: failed to allocate holder pool (%u entries)",
                              chain->holder_pool_size);
                free(chain);
                return -ENOMEM;
        }

        chain->holder_free_stack = (uint32_t *)calloc(
                chain->holder_pool_size, sizeof(uint32_t));
        if (!chain->holder_free_stack) {
                AFXDP_LOG_ERR("Chain init: failed to allocate holder free stack");
                free(chain->holder_pool);
                free(chain);
                return -ENOMEM;
        }

        /* Populate free stack — all holders start as free */
        for (uint32_t j = 0; j < chain->holder_pool_size; j++) {
                chain->holder_free_stack[j] = j;
        }
        chain->holder_free_count = chain->holder_pool_size;

        AFXDP_LOG_INFO("Chain: holder pool allocated (%u holders, %lu bytes each)",
                       chain->holder_pool_size,
                       (unsigned long)sizeof(struct afxdp_pkt_holder));

        /* ---- Create per-NF rings and register callbacks ---- */
        for (i = 0; i < num_nfs; i++) {
                struct afxdp_nf *nf = &chain->nfs[i];
                char ring_name[64];

                nf->nf_id = i;
                nf->chain_position = i;
                nf->active = true;

                /* rte_ring pointers are NULL in Phase-1 */
                nf->rx_ring = NULL;
                nf->tx_ring = NULL;

                /* Create custom SPSC RX ring */
                snprintf(ring_name, sizeof(ring_name), "nf%u_rx", i);
                nf->rx_ring_custom = afxdp_ring_create(ring_name,
                                                        AFXDP_NF_RING_SIZE);
                if (!nf->rx_ring_custom) {
                        AFXDP_LOG_ERR("Chain init: failed to create RX ring for NF %u", i);
                        goto fail_rings;
                }

                /* Create custom SPSC TX ring */
                snprintf(ring_name, sizeof(ring_name), "nf%u_tx", i);
                nf->tx_ring_custom = afxdp_ring_create(ring_name,
                                                        AFXDP_NF_RING_SIZE);
                if (!nf->tx_ring_custom) {
                        AFXDP_LOG_ERR("Chain init: failed to create TX ring for NF %u", i);
                        afxdp_ring_free(nf->rx_ring_custom);
                        nf->rx_ring_custom = NULL;
                        goto fail_rings;
                }

                /* Register simple_forward handler for all NFs in Phase-1 */
                nf->handler = afxdp_simple_forward_handler;

                /* Set up chain order */
                chain->chain_order[i] = i;

                /* Zero stats */
                memset(&nf->stats, 0, sizeof(nf->stats));

                AFXDP_LOG_INFO("Chain: NF %u initialized (simple_forward)", i);
        }

        ctx->chain = chain;

        AFXDP_LOG_INFO("========================================");
        AFXDP_LOG_INFO("  NF Chain Initialized: %u NFs", num_nfs);
        AFXDP_LOG_INFO("  Ring backend: %s",
                       chain->ring_backend == AFXDP_RING_BE_CUSTOM
                           ? "CUSTOM SPSC" : "DPDK rte_ring");
        AFXDP_LOG_INFO("  Ring size: %d  Burst: %d",
                       AFXDP_NF_RING_SIZE, AFXDP_NF_RING_BURST);
        AFXDP_LOG_INFO("========================================");

        return 0;

fail_rings:
        /* Clean up any rings we already created */
        for (uint16_t j = 0; j < i; j++) {
                afxdp_ring_free(chain->nfs[j].rx_ring_custom);
                afxdp_ring_free(chain->nfs[j].tx_ring_custom);
        }
        free(chain->holder_free_stack);
        free(chain->holder_pool);
        free(chain);
        return -ENOMEM;
}

/******************************* Chain Forward ********************************/

/*
 * Process one NF: dequeue from its RX ring, call handler, route output.
 *
 * Packets with ACTION_NEXT go to the next NF's RX ring.
 * Packets with ACTION_OUT are collected for egress.
 * Packets with ACTION_DROP are freed back to the holder pool.
 * Packets with ACTION_TONF go to the specified destination NF's RX ring.
 */
static uint32_t
afxdp_chain_process_nf(struct afxdp_chain_ctx *chain,
                       uint16_t nf_idx,
                       struct afxdp_pkt_holder **egress_holders,
                       uint32_t max_egress,
                       uint32_t egress_count) {
        struct afxdp_nf *nf = &chain->nfs[nf_idx];
        struct afxdp_pkt_holder *batch[AFXDP_NF_RING_BURST];
        uint32_t dequeued, i;

        if (!nf->active || !nf->rx_ring_custom)
                return egress_count;

        /* Dequeue burst from NF's RX ring */
        dequeued = afxdp_ring_dequeue_burst(
                nf->rx_ring_custom, (void **)batch, AFXDP_NF_RING_BURST);

        for (i = 0; i < dequeued; i++) {
                struct afxdp_pkt_holder *pkt = batch[i];

                /* Update NF RX stats */
                nf->stats.rx_packets++;
                nf->stats.rx_bytes += pkt->desc.len;

                /* Call the NF handler */
                if (nf->handler) {
                        nf->handler(pkt, nf);
                }

                /* Route based on action */
                switch (pkt->meta.action) {
                case AFXDP_NF_ACTION_NEXT: {
                        /* Forward to next NF in chain */
                        uint16_t next_pos = nf->chain_position + 1;
                        if (next_pos < chain->chain_length) {
                                uint16_t next_id = chain->chain_order[next_pos];
                                struct afxdp_nf *next_nf = &chain->nfs[next_id];
                                pkt->meta.chain_index = next_pos;
                                if (afxdp_ring_enqueue(next_nf->rx_ring_custom,
                                                       pkt) != 0) {
                                        /* Next NF's ring is full — drop */
                                        nf->stats.dropped++;
                                        afxdp_holder_free(chain, pkt);
                                }
                        } else {
                                /* Last NF in chain — treat as OUT */
                                if (egress_count < max_egress) {
                                        egress_holders[egress_count++] = pkt;
                                } else {
                                        nf->stats.dropped++;
                                        afxdp_holder_free(chain, pkt);
                                }
                        }
                        nf->stats.tx_packets++;
                        nf->stats.tx_bytes += pkt->desc.len;
                        break;
                }

                case AFXDP_NF_ACTION_OUT:
                        /* Send to NIC egress */
                        if (egress_count < max_egress) {
                                egress_holders[egress_count++] = pkt;
                        } else {
                                nf->stats.dropped++;
                                afxdp_holder_free(chain, pkt);
                        }
                        nf->stats.tx_packets++;
                        nf->stats.tx_bytes += pkt->desc.len;
                        break;

                case AFXDP_NF_ACTION_TONF: {
                        /* Send to a specific NF */
                        uint16_t dest = pkt->meta.destination;
                        if (dest < chain->chain_length &&
                            chain->nfs[dest].active) {
                                if (afxdp_ring_enqueue(
                                        chain->nfs[dest].rx_ring_custom,
                                        pkt) != 0) {
                                        nf->stats.dropped++;
                                        afxdp_holder_free(chain, pkt);
                                }
                        } else {
                                nf->stats.dropped++;
                                afxdp_holder_free(chain, pkt);
                        }
                        nf->stats.tx_packets++;
                        nf->stats.tx_bytes += pkt->desc.len;
                        break;
                }

                case AFXDP_NF_ACTION_DROP:
                default:
                        /* Drop packet */
                        nf->stats.dropped++;
                        afxdp_holder_free(chain, pkt);
                        break;
                }
        }

        return egress_count;
}

uint32_t
afxdp_chain_forward(struct afxdp_chain_ctx *chain,
                    struct afxdp_pkt_holder **egress_holders,
                    uint32_t max_egress) {
        uint32_t egress_count = 0;
        uint16_t i;

        for (i = 0; i < chain->chain_length; i++) {
                uint16_t nf_id = chain->chain_order[i];
                egress_count = afxdp_chain_process_nf(
                        chain, nf_id, egress_holders,
                        max_egress, egress_count);
        }

        return egress_count;
}

/******************************* Chain Stats ***********************************/

void
afxdp_chain_print_stats(const struct afxdp_chain_ctx *chain) {
        uint16_t i;

        printf("\n--- NF Chain Statistics ---\n");
        for (i = 0; i < chain->chain_length; i++) {
                const struct afxdp_nf *nf = &chain->nfs[i];
                if (!nf->active)
                        continue;
                printf("  NF %u: RX %" PRIu64 " pkts (%" PRIu64 " B)  "
                       "TX %" PRIu64 " pkts (%" PRIu64 " B)  "
                       "Dropped %" PRIu64 "\n",
                       nf->nf_id,
                       nf->stats.rx_packets, nf->stats.rx_bytes,
                       nf->stats.tx_packets, nf->stats.tx_bytes,
                       nf->stats.dropped);
        }
        printf("---\n\n");
}

/******************************* Chain Teardown ********************************/

void
afxdp_chain_teardown(struct afxdp_manager_ctx *ctx) {
        struct afxdp_chain_ctx *chain = ctx->chain;
        uint16_t i;

        if (!chain)
                return;

        AFXDP_LOG_INFO("Tearing down NF chain...");

        /* Print final chain stats */
        afxdp_chain_print_stats(chain);

        /* Free per-NF rings */
        for (i = 0; i < chain->chain_length; i++) {
                struct afxdp_nf *nf = &chain->nfs[i];
                if (nf->rx_ring_custom) {
                        afxdp_ring_free(nf->rx_ring_custom);
                        nf->rx_ring_custom = NULL;
                }
                if (nf->tx_ring_custom) {
                        afxdp_ring_free(nf->tx_ring_custom);
                        nf->tx_ring_custom = NULL;
                }
                nf->active = false;
        }

        /* Free holder pool */
        if (chain->holder_pool) {
                free(chain->holder_pool);
                chain->holder_pool = NULL;
        }
        if (chain->holder_free_stack) {
                free(chain->holder_free_stack);
                chain->holder_free_stack = NULL;
        }

        free(chain);
        ctx->chain = NULL;

        AFXDP_LOG_INFO("NF chain teardown complete");
}
