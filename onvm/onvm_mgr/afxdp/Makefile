# SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause)
#
#                    openNetVM — AF_XDP Kernel Program
#
# This Makefile compiles the eBPF/XDP kernel program (af_xdp_kern.c)
# into a BPF object file (af_xdp_kern.o) that the userspace AF_XDP
# manager loads and attaches to the NIC at runtime.
#
# Usage:
#   make              — build af_xdp_kern.o
#   make clean        — remove build artifacts
#
# Requirements:
#   - clang (with BPF target support)
#   - libbpf headers (bpf/bpf_helpers.h)
#   - linux kernel headers (linux/bpf.h)
#

CLANG     ?= clang
LLC       ?= llc
BPF_FLAGS  = -O2 -g -target bpf

# Output
BPF_OBJ    = af_xdp_kern.o

# Source
BPF_SRC    = af_xdp_kern.c

.PHONY: all clean

all: $(BPF_OBJ)

$(BPF_OBJ): $(BPF_SRC)
	$(CLANG) $(BPF_FLAGS) -c $< -o $@
	@echo "  BPF     $@"

clean:
	rm -f $(BPF_OBJ)
