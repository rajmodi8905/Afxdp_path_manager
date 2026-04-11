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
 * afxdp_firewall.c — Firewall NF for AF_XDP chaining.
 *
 * Drops non-IPv4 packets, forwards IPv4 packets to the next NF.
 ********************************************************************/

#include "afxdp_firewall.h"
#include "../onvm_afxdp_config.h"
#include "../onvm_afxdp_pkt_helper.h"
#include "../onvm_afxdp_nf_registry.h"

int
afxdp_firewall_handler(struct afxdp_pkt_holder *pkt, struct afxdp_nf *nf) {
        struct iphdr *ip = afxdp_pkt_ipv4_hdr(pkt, nf);

        if (ip) {
                /* IPv4 packet — forward to next NF */
                pkt->meta.action = AFXDP_NF_ACTION_NEXT;
        } else {
                /* Non-IPv4 — drop */
                pkt->meta.action = AFXDP_NF_ACTION_DROP;
        }
        return 0;
}

__attribute__((constructor))
static void afxdp_firewall_register(void) {
        afxdp_nf_register_type("firewall",
                                afxdp_firewall_handler,
                                NULL, NULL);
}
