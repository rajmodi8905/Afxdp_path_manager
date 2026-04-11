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
 * afxdp_bridge.c — Bridge NF for AF_XDP chaining.
 *
 * Swaps source and destination MAC addresses, then forwards.
 ********************************************************************/

#include "afxdp_bridge.h"
#include "../onvm_afxdp_config.h"
#include "../onvm_afxdp_pkt_helper.h"
#include "../onvm_afxdp_nf_registry.h"

int
afxdp_bridge_handler(struct afxdp_pkt_holder *pkt, struct afxdp_nf *nf) {
        struct ethhdr *eth = afxdp_pkt_eth_hdr(pkt, nf);

        if (eth) {
                /* Swap src and dst MAC addresses */
                uint8_t tmp[ETH_ALEN];
                __builtin_memcpy(tmp, eth->h_dest, ETH_ALEN);
                __builtin_memcpy(eth->h_dest, eth->h_source, ETH_ALEN);
                __builtin_memcpy(eth->h_source, tmp, ETH_ALEN);
        }

        pkt->meta.action = AFXDP_NF_ACTION_NEXT;
        return 0;
}

__attribute__((constructor))
static void afxdp_bridge_register(void) {
        afxdp_nf_register_type("bridge",
                                afxdp_bridge_handler,
                                NULL, NULL);
}
