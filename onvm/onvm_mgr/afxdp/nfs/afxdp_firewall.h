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
 * afxdp_firewall.h — Firewall NF for AF_XDP chaining.
 *
 * Drops non-IPv4 packets, forwards IPv4 packets to the next NF.
 * AF_XDP equivalent of examples/firewall.
 ********************************************************************/

#ifndef _AFXDP_FIREWALL_H_
#define _AFXDP_FIREWALL_H_

#include "../onvm_afxdp_types.h"

int
afxdp_firewall_handler(struct afxdp_pkt_holder *pkt,
                       struct afxdp_nf *nf);

#endif /* _AFXDP_FIREWALL_H_ */
