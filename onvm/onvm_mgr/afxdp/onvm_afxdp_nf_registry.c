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
 * onvm_afxdp_nf_registry.c — NF type registry implementation.
 ********************************************************************/

#include "onvm_afxdp_nf_registry.h"
#include <string.h>

/******************************* Registry Table *******************************/

static struct afxdp_nf_type nf_registry[AFXDP_MAX_NF_TYPES];
static uint16_t nf_registry_count = 0;

/******************************* API ******************************************/

int
afxdp_nf_register_type(const char *name,
                       afxdp_nf_handler_fn handler,
                       afxdp_nf_setup_fn setup,
                       afxdp_nf_teardown_fn teardown) {
        if (nf_registry_count >= AFXDP_MAX_NF_TYPES) {
                AFXDP_LOG_ERR("NF registry full, cannot register '%s'", name);
                return -1;
        }
        if (!name || !handler) {
                AFXDP_LOG_ERR("NF registry: name and handler are required");
                return -1;
        }

        struct afxdp_nf_type *entry = &nf_registry[nf_registry_count++];
        strncpy(entry->name, name, sizeof(entry->name) - 1);
        entry->name[sizeof(entry->name) - 1] = '\0';
        entry->ftable.pkt_handler = handler;
        entry->ftable.setup = setup;
        entry->ftable.teardown = teardown;

        return 0;
}

struct afxdp_nf_type *
afxdp_nf_lookup_type(const char *name) {
        for (uint16_t i = 0; i < nf_registry_count; i++) {
                if (strcmp(nf_registry[i].name, name) == 0)
                        return &nf_registry[i];
        }
        return NULL;
}

void
afxdp_nf_print_registry(void) {
        AFXDP_LOG_INFO("NF Registry: %u types registered", nf_registry_count);
        for (uint16_t i = 0; i < nf_registry_count; i++) {
                AFXDP_LOG_INFO("  [%u] %s", i, nf_registry[i].name);
        }
}
