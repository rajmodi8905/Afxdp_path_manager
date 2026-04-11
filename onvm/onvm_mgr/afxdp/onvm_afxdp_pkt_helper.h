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
 * onvm_afxdp_pkt_helper.h — packet data access helpers for native AF_XDP NFs.
 *
 * Provides inline functions to access raw packet bytes and protocol
 * headers from an afxdp_pkt_holder.  Mirrors onvm_pkt_helper.h from
 * the DPDK NF library.
 ********************************************************************/

#ifndef _ONVM_AFXDP_PKT_HELPER_H_
#define _ONVM_AFXDP_PKT_HELPER_H_

#include "onvm_afxdp_types.h"
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <arpa/inet.h>

/* Return a pointer to the start of the raw packet data. */
static inline void *
afxdp_pkt_data(struct afxdp_pkt_holder *h, struct afxdp_nf *nf) {
        return (uint8_t *)nf->packet_buffer + h->desc.umem_addr;
}

/* Return the Ethernet header, or NULL if packet is too short. */
static inline struct ethhdr *
afxdp_pkt_eth_hdr(struct afxdp_pkt_holder *h, struct afxdp_nf *nf) {
        if (h->desc.len < sizeof(struct ethhdr))
                return NULL;
        return (struct ethhdr *)afxdp_pkt_data(h, nf);
}

/* Return the IPv4 header, or NULL if not an IPv4 packet. */
static inline struct iphdr *
afxdp_pkt_ipv4_hdr(struct afxdp_pkt_holder *h, struct afxdp_nf *nf) {
        struct ethhdr *eth = afxdp_pkt_eth_hdr(h, nf);
        if (!eth || ntohs(eth->h_proto) != ETH_P_IP)
                return NULL;
        if (h->desc.len < sizeof(struct ethhdr) + sizeof(struct iphdr))
                return NULL;
        return (struct iphdr *)(eth + 1);
}

/* Return the TCP header, or NULL if not a TCP packet. */
static inline struct tcphdr *
afxdp_pkt_tcp_hdr(struct afxdp_pkt_holder *h, struct afxdp_nf *nf) {
        struct iphdr *ip = afxdp_pkt_ipv4_hdr(h, nf);
        if (!ip || ip->protocol != IPPROTO_TCP)
                return NULL;
        return (struct tcphdr *)((uint8_t *)ip + (ip->ihl * 4));
}

/* Return the UDP header, or NULL if not a UDP packet. */
static inline struct udphdr *
afxdp_pkt_udp_hdr(struct afxdp_pkt_holder *h, struct afxdp_nf *nf) {
        struct iphdr *ip = afxdp_pkt_ipv4_hdr(h, nf);
        if (!ip || ip->protocol != IPPROTO_UDP)
                return NULL;
        return (struct udphdr *)((uint8_t *)ip + (ip->ihl * 4));
}

#endif /* _ONVM_AFXDP_PKT_HELPER_H_ */
