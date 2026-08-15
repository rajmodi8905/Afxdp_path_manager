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

                           onvm_afxdp_ring.h

    Lockfree single-producer / single-consumer (SPSC) ring for AF_XDP
    NF chaining.  Provides an rte_ring-compatible API surface so the
    backend can be swapped to DPDK rte_ring when the binary is linked
    against DPDK in a future phase.

    The ring stores void* pointers internally. In the NF chaining path
    the stored objects are always (struct afxdp_pkt_holder *).

******************************************************************************/

#ifndef _ONVM_AFXDP_RING_H_
#define _ONVM_AFXDP_RING_H_

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "onvm_afxdp_config.h"

/******************************** Ring Struct **********************************/

/*
 * Lockfree SPSC ring.
 *
 * - `size` is always a power of 2 so masking works.
 * - `head` / `tail` are only written by their owning side
 *   (producer writes head, consumer writes tail) and read by
 *   the other side, using acquire/release atomics.
 */
struct afxdp_nf_ring {
        uint32_t size;            /* Number of slots (power of 2)            */
        uint32_t mask;            /* size - 1, for index wrapping            */

        /* Producer state (written by enqueue side only) */
        volatile uint32_t head __attribute__((aligned(64)));

        /* Consumer state (written by dequeue side only) */
        volatile uint32_t tail __attribute__((aligned(64)));

        /* Flexible array of pointers */
        void *ring[] __attribute__((aligned(64)));
};

/******************************** Public API **********************************/

/**
 * Create a new SPSC ring.
 *
 * @param name
 *   Human-readable name (for logging). Can be NULL.
 * @param count
 *   Requested number of entries. Will be rounded up to the next power of 2.
 *   Must be >= 2.
 * @return
 *   Pointer to the ring, or NULL on allocation failure.
 */
struct afxdp_nf_ring *
afxdp_ring_create(const char *name, uint32_t count);

/**
 * Free a ring created by afxdp_ring_create().
 */
void
afxdp_ring_free(struct afxdp_nf_ring *r);

/**
 * Enqueue a single object (SPSC producer side).
 *
 * @return
 *   0 on success, -1 if ring is full.
 */
int
afxdp_ring_enqueue(struct afxdp_nf_ring *r, void *obj);

/**
 * Dequeue a single object (SPSC consumer side).
 *
 * @return
 *   0 on success (object stored in *obj), -1 if ring is empty.
 */
int
afxdp_ring_dequeue(struct afxdp_nf_ring *r, void **obj);

/**
 * Dequeue up to `max` objects in a burst.
 *
 * @return
 *   Number of objects actually dequeued (may be 0).
 */
uint32_t
afxdp_ring_dequeue_burst(struct afxdp_nf_ring *r, void **objs, uint32_t max);

/**
 * Return the number of objects currently in the ring.
 */
uint32_t
afxdp_ring_count(const struct afxdp_nf_ring *r);

/**
 * Return the number of free slots in the ring.
 */
uint32_t
afxdp_ring_free_count(const struct afxdp_nf_ring *r);

#endif /* _ONVM_AFXDP_RING_H_ */
