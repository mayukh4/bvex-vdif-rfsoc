/*
 * xsk_redirect.bpf.c -- minimal frags-aware AF_XDP redirect program for the VDIF
 * recorder (--rx xdp). Loaded by vdif_recorder via libbpf and attached to the NIC;
 * it redirects every packet on the bound RX queue into the AF_XDP socket map.
 *
 * WHY OUR OWN PROGRAM (not libxdp's default): at MTU 9000 the mlx5 driver requires a
 * frags-aware XDP program to attach (each 8074-B jumbo frame spans >1 page). The
 * SEC("xdp.frags") annotation makes the (modern, bundled) libbpf set
 * BPF_F_XDP_HAS_FRAGS at load time, so the attach is accepted on the legacy-RQ path.
 * libxdp's bundled default program / dispatcher failed to attach in COPY mode on this
 * ConnectX-5, so the recorder loads THIS program directly and binds the XSK with
 * XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD.
 *
 * Build (Makefile): clang -O2 -g -target bpf -c xsk_redirect.bpf.c -o xsk_redirect.bpf.o
 */
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

/* one slot per RX queue; the recorder inserts its xsk fd at key == queue id */
struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 64);
    __type(key, __u32);
    __type(value, __u32);
} xsks_map SEC(".maps");

SEC("xdp.frags")
int xsk_redirect_prog(struct xdp_md *ctx)
{
    __u32 idx = ctx->rx_queue_index;
    /* redirect to the AF_XDP socket bound to this queue; if none, let it pass up the
     * stack (harmless -- the recorder validates VDIF, ARP/junk is ignored). */
    if (bpf_map_lookup_elem(&xsks_map, &idx))
        return bpf_redirect_map(&xsks_map, idx, XDP_PASS);
    return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
