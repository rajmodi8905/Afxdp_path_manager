/*********************************************************************\r
 *                     openNetVM\r
 *              https://sdnfv.github.io\r
 *\r
 *   BSD LICENSE\r
 *\r
 *   Copyright(c)\r
 *            2015-2019 George Washington University\r
 *            2015-2019 University of California Riverside\r
 *   All rights reserved.\r
 *\r
 *   Redistribution and use in source and binary forms, with or without\r
 *   modification, are permitted provided that the following conditions\r
 *   are met:\r
 *\r
 *     * Redistributions of source code must retain the above copyright\r
 *       notice, this list of conditions and the following disclaimer.\r
 *     * Redistributions in binary form must reproduce the above copyright\r
 *       notice, this list of conditions and the following disclaimer in\r
 *       the documentation and/or other materials provided with the\r
 *       distribution.\r
 *     * The name of the author may not be used to endorse or promote\r
 *       products derived from this software without specific prior\r
 *       written permission.\r
 *\r
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS\r
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT\r
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR\r
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT\r
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,\r
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT\r
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,\r
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY\r
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT\r
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE\r
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\r
 *\r
 ********************************************************************/\r
\r
/******************************************************************************\r
\r
                          onvm_combined_dpdk.h\r
\r
    Thin wrapper layer for the combined DPDK+AF_XDP architecture.\r
    Bridges AF_XDP Socket 1 (Port 1, Slice B) with DPDK's rte_mbuf world\r
    WITHOUT using the DPDK AF_XDP PMD.  Our manager creates the AF_XDP\r
    socket directly; this wrapper copies packets between UMEM frames and\r
    rte_mbufs so that openNetVM NFs (separate processes using rte_ring)\r
    can process them.\r
\r
******************************************************************************/\r
\r
#ifndef _ONVM_COMBINED_DPDK_H_\r
#define _ONVM_COMBINED_DPDK_H_\r
\r
#include "onvm_afxdp_types.h"\r
\r
#include <rte_mbuf.h>\r
#include <rte_mempool.h>\r
#include <rte_ring.h>\r
#include <rte_memcpy.h>\r
\r
/*****************************************************************************\r
 *  Configuration\r
 *****************************************************************************/\r
\r
/* Number of rte_mbufs in the DPDK mempool (must be > ring sizes). */\r
#define COMBINED_DPDK_MBUF_POOL_SIZE     16384\r
\r
/* Size of the rte_mbuf data room (must hold the largest packet). */\r
#define COMBINED_DPDK_MBUF_DATA_SIZE     2048\r
\r
/* Maximum burst size for RX/TX operations. */\r
#define COMBINED_DPDK_BATCH_SIZE         256\r
\r
/*****************************************************************************\r
 *  Initialization\r
 *****************************************************************************/\r
\r
/**\r
 * Initialize the combined DPDK wrapper.\r
 *\r
 * Creates the rte_mempool used to allocate rte_mbuf wrappers for\r
 * packets received from AF_XDP Socket 1.\r
 *\r
 * Must be called AFTER rte_eal_init() and afxdp_init().\r
 *\r
 * @param ctx  Manager context with xsk_socket_dpdk already initialized.\r
 * @return 0 on success, negative errno on failure.\r
 */\r
int combined_dpdk_init(struct afxdp_manager_ctx *ctx);\r
\r
/*****************************************************************************\r
 *  Packet I/O (AF_XDP Socket 1 ↔ rte_mbuf)\r
 *****************************************************************************/\r
\r
/**\r
 * Receive packets from AF_XDP Socket 1 (Port 1).\r
 *\r
 * Dequeues descriptors from Socket 1's RX ring, copies each packet\r
 * payload into a freshly allocated rte_mbuf, returns the UMEM frame\r
 * to Slice B's free pool, and refills the Fill Ring.\r
 *\r
 * @param ctx       Manager context.\r
 * @param bufs      Output array of rte_mbuf pointers.\r
 * @param max_pkts  Maximum number of packets to receive.\r
 * @return Number of packets received (0 if none available).\r
 */\r
uint16_t combined_dpdk_rx(struct afxdp_manager_ctx *ctx,\r
                          struct rte_mbuf **bufs, uint16_t max_pkts);\r
\r
/**\r
 * Transmit packets through AF_XDP Socket 1 (Port 1).\r
 *\r
 * For each rte_mbuf, allocates a Slice B UMEM frame, copies the\r
 * packet payload into it, submits a TX descriptor, and frees the mbuf.\r
 *\r
 * @param ctx      Manager context.\r
 * @param bufs     Array of rte_mbuf pointers to transmit.\r
 * @param nb_pkts  Number of packets in the array.\r
 * @return Number of packets successfully submitted to the TX ring.\r
 */\r
uint16_t combined_dpdk_tx(struct afxdp_manager_ctx *ctx,\r
                          struct rte_mbuf **bufs, uint16_t nb_pkts);\r
\r
/*****************************************************************************\r
 *  Worker Threads\r
 *****************************************************************************/\r
\r
/**\r
 * DPDK RX thread — polls Socket 1, routes packets to openNetVM NF rx_q.\r
 *\r
 * Thread entry point. Pins to AFXDP_BASE_CORE + 4.\r
 */\r
void *combined_dpdk_rx_thread(void *arg);\r
\r
/**\r
 * DPDK TX thread — drains openNetVM NF tx_q, sends via Socket 1 TX ring.\r
 *\r
 * Thread entry point. Pins to AFXDP_BASE_CORE + 5.\r
 */\r
void *combined_dpdk_tx_thread(void *arg);\r
\r
#endif /* _ONVM_COMBINED_DPDK_H_ */\r
