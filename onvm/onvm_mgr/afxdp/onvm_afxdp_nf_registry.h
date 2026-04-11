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
 * onvm_afxdp_nf_registry.h — NF type registry for runtime NF selection.
 *
 * Each NF type (simple_forward, firewall, bridge, ...) registers
 * itself by name at program startup.  The manager resolves NF names
 * from the CLI -C flag to handler function pointers via this registry.
 ********************************************************************/

#ifndef _ONVM_AFXDP_NF_REGISTRY_H_
#define _ONVM_AFXDP_NF_REGISTRY_H_

#include "onvm_afxdp_types.h"

/* A registered NF type entry. */
struct afxdp_nf_type {
        char name[64];
        struct afxdp_nf_function_table ftable;
};

/* Register a new NF type. Returns 0 on success, -1 if registry is full. */
int
afxdp_nf_register_type(const char *name,
                       afxdp_nf_handler_fn handler,
                       afxdp_nf_setup_fn setup,
                       afxdp_nf_teardown_fn teardown);

/* Look up a registered NF type by name. Returns NULL if not found. */
struct afxdp_nf_type *
afxdp_nf_lookup_type(const char *name);

/* Print all registered NF types to stdout. */
void
afxdp_nf_print_registry(void);

#endif /* _ONVM_AFXDP_NF_REGISTRY_H_ */
