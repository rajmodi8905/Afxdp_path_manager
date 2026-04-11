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
 * afxdp_bridge.h — Bridge NF for AF_XDP chaining.
 *
 * Swaps source and destination MAC addresses, then forwards
 * to the next NF. AF_XDP equivalent of examples/bridge.
 ********************************************************************/

#ifndef _AFXDP_BRIDGE_H_
#define _AFXDP_BRIDGE_H_

#include "../onvm_afxdp_types.h"

int
afxdp_bridge_handler(struct afxdp_pkt_holder *pkt,
                     struct afxdp_nf *nf);

#endif /* _AFXDP_BRIDGE_H_ */
