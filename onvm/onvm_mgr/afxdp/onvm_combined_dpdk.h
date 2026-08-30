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

                          onvm_combined_dpdk.h

    Thin wrapper layer for the combined DPDK+AF_XDP architecture.
    Bridges AF_XDP Socket 1 (Port 1, Slice B) with DPDK's rte_mbuf world
    WITHOUT using the DPDK AF_XDP PMD.  Our manager creates the AF_XDP
    socket directly; this wrapper copies packets between UMEM frames and
    rte_mbufs so that openNetVM NFs (separate processes using rte_ring)
    can process them.

******************************************************************************/

#ifndef _ONVM_COMBINED_DPDK_H_
#define _ONVM_COMBINED_DPDK_H_

#include "onvm_afxdp_types.h"

#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_memcpy.h>

/*****************************************************************************
 *  Configuration
 *****************************************************************************/

/* Number of rte_mbufs in the DPDK mempool (must be > ring sizes). */
#define COMBINED_DPDK_MBUF_POOL_SIZE     16384

/* Size of the rte_mbuf data room (must hold the largest packet). */
#define COMBINED_DPDK_MBUF_DATA_SIZE     2048

/* Maximum burst size for RX/TX operations. */
#define COMBINED_DPDK_BATCH_SIZE         256

/*****************************************************************************
 *  Initialization
 *****************************************************************************/

/**
 * Initialize the combined DPDK wrapper.
 *
 * Creates the rte_mempool used to allocate rte_mbuf wrappers for
 * packets received from AF_XDP Socket 1.
 *
 * Must be called AFTER rte_eal_init() and afxdp_init().
 *
 * @param ctx  Manager context with xsk_socket_dpdk already initialized.
 * @return 0 on success, negative errno on failure.
 */
int combined_dpdk_init(struct afxdp_manager_ctx *ctx);

/*****************************************************************************
 *  Packet I/O (AF_XDP Socket 1 ↔ rte_mbuf)
 *****************************************************************************/

/**
 * Receive packets from AF_XDP Socket 1 (Port 1).
 *
 * Dequeues descriptors from Socket 1's RX ring, copies each packet
 * payload into a freshly allocated rte_mbuf, returns the UMEM frame
 * to Slice B's free pool, and refills the Fill Ring.
 *
 * @param ctx       Manager context.
 * @param bufs      Output array of rte_mbuf pointers.
 * @param max_pkts  Maximum number of packets to receive.
 * @return Number of packets received (0 if none available).
 */
uint16_t combined_dpdk_rx(struct afxdp_manager_ctx *ctx,
                          struct rte_mbuf **bufs, uint16_t max_pkts);

/**
 * Transmit packets through AF_XDP Socket 1 (Port 1).
 *
 * For each rte_mbuf, allocates a Slice B UMEM frame, copies the
 * packet payload into it, submits a TX descriptor, and frees the mbuf.
 *
 * @param ctx      Manager context.
 * @param bufs     Array of rte_mbuf pointers to transmit.
 * @param nb_pkts  Number of packets in the array.
 * @return Number of packets successfully submitted to the TX ring.
 */
uint16_t combined_dpdk_tx(struct afxdp_manager_ctx *ctx,
                          struct rte_mbuf **bufs, uint16_t nb_pkts);

/*****************************************************************************
 *  Worker Threads
 *****************************************************************************/

/**
 * DPDK RX thread — polls Socket 1, routes packets to openNetVM NF rx_q.
 *
 * Thread entry point. Pins to AFXDP_BASE_CORE + 4.
 */
void *combined_dpdk_rx_thread(void *arg);

/**
 * DPDK TX thread — drains openNetVM NF tx_q, sends via Socket 1 TX ring.
 *
 * Thread entry point. Pins to AFXDP_BASE_CORE + 5.
 */
void *combined_dpdk_tx_thread(void *arg);

#endif /* _ONVM_COMBINED_DPDK_H_ */
