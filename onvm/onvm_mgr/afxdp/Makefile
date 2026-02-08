# SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause)

CLANG	?= clang

# Force architecture-specific include path
ARCH_INC	= -I/usr/include/$(shell uname -m)-linux-gnu

# Critical flags:
# -target bpf     : Compile for BPF architecture (NOT x86)
# -march=bpf      : Explicitly set machine architecture
# -c              : Compile only (do not link)
BPF_FLAGS	= -O2 -g -target bpf -D__BPF_TRACING__ $(ARCH_INC)

BPF_OBJ	= af_xdp_kern.o
BPF_SRC	= af_xdp_kern.c

.PHONY: all clean

all: $(BPF_OBJ)

$(BPF_OBJ): $(BPF_SRC)
	$(CLANG) $(BPF_FLAGS) -c $< -o $@
	@echo "  BPF     $@"

clean:
	rm -f $(BPF_OBJ)
