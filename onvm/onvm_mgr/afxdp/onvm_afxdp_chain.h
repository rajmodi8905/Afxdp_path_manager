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

                           onvm_afxdp_chain.h

    Chain management API for AF_XDP NF chaining.

    Provides initialization, per-batch forwarding through the NF chain,
    and teardown of all chaining resources (rings, holder pool, NF state).

******************************************************************************/

#ifndef _ONVM_AFXDP_CHAIN_H_
#define _ONVM_AFXDP_CHAIN_H_

#include "onvm_afxdp_types.h"
#include "onvm_afxdp_ring.h"

/******************************** Holder Pool *********************************/

/**
 * Allocate a packet holder from the pre-allocated pool.
 *
 * @return
 *   Pointer to holder, or NULL if pool is exhausted.
 */
struct afxdp_pkt_holder *
afxdp_holder_alloc(struct afxdp_chain_ctx *chain);

/**
 * Return a packet holder to the pool.
 */
void
afxdp_holder_free(struct afxdp_chain_ctx *chain,
                  struct afxdp_pkt_holder *holder);

/**
 * Return a UMEM frame to the socket free-list.
 * Defined in onvm_afxdp.c; declared here so the chain module
 * can reclaim UMEM frames when dropping packets.
 */
void
afxdp_free_umem_frame(struct afxdp_socket_info *xsk, uint64_t frame);

/******************************** Chain API ************************************/

/**
 * Initialize the NF chain.
 *
 * Allocates the chain context, creates per-NF SPSC rings, pre-allocates
 * the packet holder pool, and registers NF handler callbacks.
 *
 * @param ctx
 *   Manager context. On success, ctx->chain is set.
 * @param num_nfs
 *   Number of NFs in the static chain (e.g. 2 for simple_forward × 2).
 * @return
 *   0 on success, negative errno on failure.
 */
int
afxdp_chain_init(struct afxdp_manager_ctx *ctx, uint16_t num_nfs);

/**
 * Forward packets through the NF chain.
 *
 * For each NF in chain order:
 *   1. Dequeue burst from NF's RX ring
 *   2. Call the NF handler callback for each packet
 *   3. Based on meta.action, route to next NF's RX ring or to egress
 *
 * Packets marked ACTION_OUT are returned via the egress arrays.
 *
 * @param chain
 *   Chain context.
 * @param egress_holders
 *   Output array to receive holders destined for NIC TX.
 * @param max_egress
 *   Maximum number of egress holders to return.
 * @return
 *   Number of holders placed in egress_holders.
 */
uint32_t
afxdp_chain_forward(struct afxdp_chain_ctx *chain,
                    struct afxdp_pkt_holder **egress_holders,
                    uint32_t max_egress);

/**
 * Print per-NF statistics.
 */
void
afxdp_chain_print_stats(const struct afxdp_chain_ctx *chain);

/**
 * Tear down all chaining resources.
 *
 * Frees all rings, the holder pool, and the chain context itself.
 * Sets ctx->chain to NULL.
 */
void
afxdp_chain_teardown(struct afxdp_manager_ctx *ctx);

/**
 * Initialize the NF chain from a comma-separated NF type spec string.
 *
 * Tokenizes the spec (e.g. "simple_forward,firewall"), looks up each
 * NF type in the registry, and assigns the handler function table.
 *
 * @param ctx
 *   Manager context.
 * @param spec
 *   Comma-separated NF type names.
 * @return
 *   0 on success, negative errno on failure.
 */
int
afxdp_chain_init_from_spec(struct afxdp_manager_ctx *ctx, const char *spec);

#endif /* _ONVM_AFXDP_CHAIN_H_ */
