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

    The manager acts as the only NF: it receives packets from the NIC
    via AF_XDP and immediately bounces them back out to the NIC
    (zero-copy through the same UMEM).

    This header exposes three functions that replace the entire DPDK
    manager pipeline when compiled with -DUSE_AFXDP:

      1. afxdp_init()    — Parse args, set up UMEM, create XSK socket,
                           load & attach XDP program, populate XSKMAP.
      2. afxdp_run()     — Enter the main polling loop: receive packets,
                           place them on the TX ring to send back to NIC,
                           refill the Fill ring, and complete TX.
      3. afxdp_cleanup() — Detach the XDP program, destroy XSK socket
                           and UMEM, free all resources.

******************************************************************************/

#ifndef _ONVM_AFXDP_H_
#define _ONVM_AFXDP_H_

#include "onvm_afxdp_types.h"

/******************************** Public API **********************************/

/**
 * Initialize the AF_XDP manager.
 *
 * Performs the following steps:
 *   1. Parse command-line arguments (interface, queue, XDP mode, etc.)
 *   2. Raise RLIMIT_MEMLOCK so UMEM can be registered
 *   3. Allocate and configure the UMEM shared buffer
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
 * Clean up and release all AF_XDP resources.
 *
 * Performs the following:
 *   1. Detach and unload the XDP program from the NIC
 *   2. Delete the AF_XDP socket
 *   3. Destroy the UMEM region
 *   4. Free the raw packet buffer
 *   5. Join the stats thread (if running)
 *
 * @param ctx
 *   Pointer to the manager context to clean up.
 */
void afxdp_cleanup(struct afxdp_manager_ctx *ctx);

#endif /* _ONVM_AFXDP_H_ */
