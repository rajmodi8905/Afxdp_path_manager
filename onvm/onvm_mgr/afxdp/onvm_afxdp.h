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

                              onvm_afxdp.h

    Public API for the AF_XDP-based NF Manager datapath.

    The manager receives packets from the NIC via AF_XDP and routes them
    through a chain of in-process NF handlers using SPSC rings and
    packet holders (zero-copy through shared UMEM).

    This header exposes four functions that replace the entire DPDK
    manager pipeline when compiled with -DUSE_AFXDP:

      1. afxdp_preallocate_hugepages() — Claim UMEM hugepages before
                                          rte_eal_init() (hugepage partitioning).
      2. afxdp_init()    — Parse args, set up UMEM, create XSK socket,
                           load & attach XDP program, initialize NF chain.
      3. afxdp_run()     — Enter the main polling loop: receive packets,
                           route through NF chain, transmit egress.
      4. afxdp_cleanup() — Tear down NF chain, detach XDP, free resources.

******************************************************************************/

#ifndef _ONVM_AFXDP_H_
#define _ONVM_AFXDP_H_

#include "onvm_afxdp_types.h"
#include "onvm_afxdp_chain.h"

/******************************** Public API **********************************/

/**
 * Pre-allocate the UMEM hugepage buffer BEFORE rte_eal_init() is called.
 *
 * This is the key mechanism for shared-pool hugepage partitioning between
 * the DPDK and AF_XDP managers.  By claiming the UMEM pages first, the
 * DPDK EAL cannot consume them even when it initialises over the same
 * /mnt/huge mount point.  The --socket-mem EAL argument then caps how
 * much DPDK may take from the remainder (see AFXDP_DPDK_SOCKET_MEM_MB).
 *
 * The function is a soft-fail: if hugepages are unavailable it falls back
 * to posix_memalign() and logs a warning rather than aborting.
 *
 * Call this once from main() before any DPDK initialisation.  If the
 * context already has a non-NULL packet_buffer (i.e., called twice),
 * it returns immediately.
 *
 * @param ctx
 *   Pointer to a zero-initialised manager context.
 * @return
 *   0 on success (or soft fallback), negative errno on hard failure.
 */
int afxdp_preallocate_hugepages(struct afxdp_manager_ctx *ctx);

/**
 * Initialize the AF_XDP manager.
 *
 * Performs the following steps:
 *   1. Parse command-line arguments (interface, queue, XDP mode, etc.)
 *   2. Raise RLIMIT_MEMLOCK so UMEM can be registered
 *   3. Adopt pre-allocated UMEM buffer (if ctx->packet_buffer set) or
 *      allocate a new hugepage-backed buffer with NUMA binding
 *   4. Load and attach the XDP kernel program to the NIC
 *   5. Create an AF_XDP socket bound to the configured (ifname, queue)
 *   6. Populate the XSKMAP with the socket fd
 *   7. Optionally start a stats polling thread
 *
 * @param ctx
 *   Pointer to the manager context to initialize.
 *   Must be zeroed by the caller before first use.
 * @param argc
 *   Argument count from main().
 * @param argv
 *   Argument vector from main().
 * @return
 *   0 on success, negative errno on failure.
 */
int afxdp_init(struct afxdp_manager_ctx *ctx, int argc, char **argv);

/**
 * Run the AF_XDP manager main loop.
 *
 * This function blocks until ctx->global_exit is set (via signal handler).
 * It continuously:
 *   1. Polls / busy-waits on the AF_XDP RX ring
 *   2. Dequeues received packet descriptors
 *   3. Places each descriptor on the TX ring (bounce back to NIC)
 *   4. Refills the Fill ring with free UMEM frames
 *   5. Completes any outstanding TX operations (reclaims UMEM frames)
 *
 * @param ctx
 *   Pointer to an initialized manager context.
 * @return
 *   0 on clean shutdown, negative errno on error.
 */
int afxdp_run(struct afxdp_manager_ctx *ctx);

/**
 * Clean up and release AF_XDP resources.
 *
 * When final_cleanup is false (mode switch: DPDK ↔ AF_XDP), only the
 * XSK socket, UMEM registration, and XDP program are torn down.  The
 * hugepage UMEM buffer is kept mapped so it can be re-used when the
 * AF_XDP manager is started again without going through pre-allocation.
 *
 * When final_cleanup is true (process exit), the UMEM buffer is also
 * released: munmap() if it was hugepage-backed, free() otherwise.
 *
 * Performs:
 *   1. Join the stats thread (if running)
 *   2. Detach and unload the XDP program from the NIC
 *   3. Delete the AF_XDP socket
 *   4. Destroy the UMEM registration (xsk_umem__delete)
 *   5. [final_cleanup only] Release the packet buffer
 *
 * @param ctx
 *   Pointer to the manager context to clean up.
 * @param final_cleanup
 *   true  → full teardown including packet buffer (process exit).
 *   false → teardown XSK/UMEM only, keep hugepage buffer for reuse.
 */
void afxdp_cleanup(struct afxdp_manager_ctx *ctx, bool final_cleanup);

#endif /* _ONVM_AFXDP_H_ */
