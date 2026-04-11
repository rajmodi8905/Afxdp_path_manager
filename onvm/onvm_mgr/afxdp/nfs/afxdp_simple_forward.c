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
 * afxdp_simple_forward.c — Simple forward NF for AF_XDP chaining.
 *
 * Sets every packet's action to AFXDP_NF_ACTION_NEXT to forward
 * to the next NF in the chain. AF_XDP equivalent of examples/simple_forward.
 ********************************************************************/

#include "afxdp_simple_forward.h"
#include "../onvm_afxdp_config.h"
#include "../onvm_afxdp_nf_registry.h"

int
afxdp_simple_forward_handler(struct afxdp_pkt_holder *pkt,
                             struct afxdp_nf *nf) {
        (void)nf;
        pkt->meta.action = AFXDP_NF_ACTION_NEXT;
        return 0;
}

/* Auto-register this NF type at program startup */
__attribute__((constructor))
static void afxdp_simple_forward_register(void) {
        afxdp_nf_register_type("simple_forward",
                                afxdp_simple_forward_handler,
                                NULL, NULL);
}
