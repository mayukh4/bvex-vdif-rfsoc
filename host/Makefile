# Makefile for the VDIF recorder + test generator.
# BUILD ON THE RECORDING HOST so -march=native matches the CPU.
#   ./build_libxdp.sh   # one-time: build libxdp + libbpf from source under ./xdpbuild
#   make                # builds vdif_recorder (+ AF_XDP backend if libxdp present) and vdif_gen
#   make tune           # optional NIC tuning (sudo)
#   make clean
#
# AF_XDP (--rx xdp) needs libxdp + a modern libbpf, built from source under ./xdpbuild
# by ./build_libxdp.sh (see the README, "AF_XDP recorder"). If that static libxdp.a is
# present, the recorder is compiled WITH the kernel-bypass backend (-DHAVE_XDP);
# otherwise it builds fine without it (udp/raw/ring backends only).

CC      ?= gcc
CFLAGS  ?= -O3 -march=native -pthread -Wall -Wextra
IFACE   ?= enp1s0f0np0

# --- AF_XDP (libxdp built from source) ------------------------------------------------
XDP_DIR    ?= xdpbuild/xdp-tools-1.5.8
XDP_LIBXDP := $(XDP_DIR)/lib/libxdp/libxdp.a
XDP_LIBBPF := $(XDP_DIR)/lib/libbpf/src/libbpf.a
BPF_CLANG ?= clang
ifneq ($(wildcard $(XDP_LIBXDP)),)
  XDP_CFLAGS := -DHAVE_XDP -I$(XDP_DIR)/headers -I$(XDP_DIR)/lib/libbpf/src/root/include
  # static archives, in dependency order: libxdp -> libbpf -> elf/z (zstd if libbpf needs it)
  XDP_LDLIBS := $(XDP_LIBXDP) $(XDP_LIBBPF) -lelf -lz
  XDP_LDLIBS += $(shell nm $(XDP_LIBBPF) 2>/dev/null | grep -q ZSTD && echo -lzstd)
  BPF_OBJ   := xsk_redirect.bpf.o     # our frags-aware redirect program (loaded at runtime)
  $(info Makefile: AF_XDP backend ENABLED (libxdp at $(XDP_LIBXDP)))
else
  $(info Makefile: AF_XDP backend DISABLED (no $(XDP_LIBXDP)); --rx xdp will error at runtime))
endif

all: vdif_recorder vdif_gen $(BPF_OBJ)

vdif_recorder: vdif_recorder.c
	$(CC) $(CFLAGS) $(XDP_CFLAGS) -o $@ $< $(XDP_LDLIBS)

# compile the AF_XDP redirect program to BPF bytecode (SEC xdp.frags). The modern
# bundled libbpf headers provide bpf_helpers.h; loaded by the recorder at runtime.
# (-I.../<multiarch> so the bpf target finds asm/types.h)
BPF_ARCH_INC := -I/usr/include/$(shell gcc -print-multiarch 2>/dev/null)
xsk_redirect.bpf.o: xsk_redirect.bpf.c
	$(BPF_CLANG) -O2 -g -target bpf -D__TARGET_ARCH_x86 $(BPF_ARCH_INC) \
	  -I$(XDP_DIR)/lib/libbpf/src/root/include -c $< -o $@

# synthetic VDIF UDP source for the off-hardware veth self-test (Linux only)
vdif_gen: vdif_gen.c
	$(CC) $(CFLAGS) -o $@ $<

tune: tune_nic.sh
	sudo ./tune_nic.sh $(IFACE)

clean:
	rm -f vdif_recorder vdif_gen xsk_redirect.bpf.o

.PHONY: all tune clean
