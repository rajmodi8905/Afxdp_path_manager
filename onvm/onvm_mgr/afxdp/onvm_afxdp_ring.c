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

                           onvm_afxdp_ring.c

    Lockfree SPSC ring implementation using C11 __atomic builtins.

    Memory ordering:
      - Producer (enqueue): store data, then release-store head.
      - Consumer (dequeue): acquire-load head, read data, then release-store tail.
    This ensures the consumer always sees fully-written data.

******************************************************************************/

#include "onvm_afxdp_ring.h"

#include <string.h>
#include <stdio.h>

#include "onvm_afxdp_config.h"

/******************************* Helpers *************************************/

/*
 * Round up to the next power of 2.  Returns v if already a power of 2.
 */
static inline uint32_t
ring_align_pow2(uint32_t v) {
        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v++;
        return v;
}

/******************************* Public API ***********************************/

struct afxdp_nf_ring *
afxdp_ring_create(const char *name, uint32_t count) {
        struct afxdp_nf_ring *r;
        uint32_t size;

        if (count < 2)
                count = 2;

        /* Usable slots = size - 1 (one slot is sentinel to distinguish
         * full from empty), so allocate one extra. */
        size = ring_align_pow2(count + 1);

        r = (struct afxdp_nf_ring *)calloc(
                1, sizeof(*r) + size * sizeof(void *));
        if (!r) {
                AFXDP_LOG_ERR("Failed to allocate SPSC ring '%s' (%u slots)",
                              name ? name : "(anon)", size);
                return NULL;
        }

        r->size = size;
        r->mask = size - 1;
        r->head = 0;
        r->tail = 0;

        AFXDP_LOG_INFO("SPSC ring '%s' created: %u slots (%u usable)",
                       name ? name : "(anon)", size, size - 1);
        return r;
}

void
afxdp_ring_free(struct afxdp_nf_ring *r) {
        free(r);
}

int
afxdp_ring_enqueue(struct afxdp_nf_ring *r, void *obj) {
        uint32_t h = r->head;
        uint32_t next = (h + 1) & r->mask;

        /* Acquire-load tail to see latest consumer progress. */
        uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);

        if (next == t)
                return -1;  /* Full */

        r->ring[h] = obj;

        /* Release-store head so consumer sees the data we just wrote. */
        __atomic_store_n(&r->head, next, __ATOMIC_RELEASE);

        return 0;
}

int
afxdp_ring_dequeue(struct afxdp_nf_ring *r, void **obj) {
        uint32_t t = r->tail;

        /* Acquire-load head to see latest producer progress. */
        uint32_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);

        if (t == h)
                return -1;  /* Empty */

        *obj = r->ring[t];

        /* Release-store tail so producer sees we consumed this slot. */
        __atomic_store_n(&r->tail, (t + 1) & r->mask, __ATOMIC_RELEASE);

        return 0;
}

uint32_t
afxdp_ring_dequeue_burst(struct afxdp_nf_ring *r, void **objs, uint32_t max) {
        uint32_t t = r->tail;
        uint32_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
        uint32_t available, i;

        /* Number of entries available to consume */
        available = (h - t) & r->mask;
        if (available > max)
                available = max;

        for (i = 0; i < available; i++) {
                objs[i] = r->ring[(t + i) & r->mask];
        }

        if (available > 0) {
                __atomic_store_n(&r->tail, (t + available) & r->mask,
                                 __ATOMIC_RELEASE);
        }

        return available;
}

uint32_t
afxdp_ring_count(const struct afxdp_nf_ring *r) {
        uint32_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
        uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
        return (h - t) & r->mask;
}

uint32_t
afxdp_ring_free_count(const struct afxdp_nf_ring *r) {
        /* Usable capacity is size - 1 (one sentinel slot). */
        return (r->size - 1) - afxdp_ring_count(r);
}
