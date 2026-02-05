/*********************************************************************
 *                     openNetVM
 *              https://sdnfv.github.io
 *
 *   GPL-2.0 LICENSE (required for eBPF programs)
 *
 *   This eBPF program is loaded onto the NIC's XDP hook by the
 *   userspace AF_XDP manager. It acts as the "Gatekeeper":
 *
 *     1. For each incoming packet, check if an AF_XDP socket is
 *        bound to this RX queue in the XSKMAP.
 *     2. If yes  → bpf_redirect_map() into the AF_XDP socket
 *                   (packet goes directly to userspace, bypassing
 *                   the entire Linux kernel network stack).
 *     3. If no   → XDP_PASS (let the kernel handle it normally).
 *
 *   This is the XDP "brain" from the architecture document:
 *     - XDP decides WHICH packets go to the NF manager
 *     - AF_XDP provides the plumbing (UMEM + rings) to deliver them
 *
 *   Compiled with:
 *     clang -O2 -g -target bpf -c af_xdp_kern.c -o af_xdp_kern.o
 *
 *   Reference: xdp-project/xdp-tutorial advanced03-AF_XDP/af_xdp_kern.c
 *
 ********************************************************************/

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

/****************************************************************************
 *
 *  BPF MAPS
 *
 *  These maps live in kernel memory and are shared between:
 *    - This eBPF program (running in kernel context at the NIC driver)
 *    - The userspace AF_XDP manager (reads/writes via file descriptors)
 *
 ****************************************************************************/

/*
 * XSKMAP: Maps RX queue indices → AF_XDP socket file descriptors.
 *
 * When the userspace manager creates an AF_XDP socket and binds it
 * to RX queue N, it inserts the socket fd into xsks_map[N].
 * This eBPF program then uses bpf_redirect_map() to steer packets
 * arriving on queue N directly into that socket.
 *
 * Type:        BPF_MAP_TYPE_XSKMAP
 * Key:         __u32 (RX queue index)
 * Value:       __u32 (XSK socket fd, managed by the kernel)
 * Max entries: 64 (one per possible RX queue)
 */
struct {
        __uint(type, BPF_MAP_TYPE_XSKMAP);
        __type(key, __u32);
        __type(value, __u32);
        __uint(max_entries, 64);
} xsks_map SEC(".maps");

/*
 * Per-CPU statistics map: Counts packets seen per RX queue.
 *
 * This is a per-CPU array so each CPU core updates its own counter
 * without any locking overhead. The userspace monitor can aggregate
 * these counters to compute total packet rates.
 *
 * Type:        BPF_MAP_TYPE_PERCPU_ARRAY
 * Key:         __u32 (RX queue index)
 * Value:       __u32 (packet count)
 * Max entries: 64
 */
struct {
        __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
        __type(key, __u32);
        __type(value, __u32);
        __uint(max_entries, 64);
} xdp_stats_map SEC(".maps");

/****************************************************************************
 *
 *  XDP PROGRAM: Ingress Steering
 *
 *  This is the entry point executed for EVERY packet arriving at the NIC.
 *  It runs in kernel context with near-zero overhead.
 *
 *  Decision logic:
 *    1. Get the RX queue index from the packet context
 *    2. Increment the per-queue packet counter (for monitoring)
 *    3. Check if an AF_XDP socket exists for this queue
 *    4. If yes → redirect into the AF_XDP socket (zero-copy to userspace)
 *    5. If no  → pass to the normal kernel stack
 *
 ****************************************************************************/

SEC("xdp")
int xdp_sock_prog(struct xdp_md *ctx)
{
        /* Get the RX queue index this packet arrived on */
        int index = ctx->rx_queue_index;

        /* Update per-queue packet counter (per-CPU, no lock needed) */
        __u32 *pkt_count;
        pkt_count = bpf_map_lookup_elem(&xdp_stats_map, &index);
        if (pkt_count) {
                (*pkt_count)++;
        }

        /*
         * Check if there is an AF_XDP socket bound to this RX queue.
         * A non-NULL lookup means the userspace manager has registered
         * a socket for this queue in the XSKMAP.
         *
         * bpf_redirect_map() returns XDP_REDIRECT on success, which
         * causes the kernel to place the packet descriptor directly
         * into the AF_XDP socket's RX ring — bypassing the entire
         * Linux network stack (sk_buff allocation, protocol parsing,
         * netfilter, etc.).
         */
        if (bpf_map_lookup_elem(&xsks_map, &index))
                return bpf_redirect_map(&xsks_map, index, 0);

        /*
         * No AF_XDP socket for this queue.
         * Let the packet continue through the normal kernel stack.
         * This allows non-managed traffic (e.g. SSH, ARP) to still
         * work even when the AF_XDP manager is running.
         */
        return XDP_PASS;
}

/* Required license declaration for BPF programs */
char _license[] SEC("license") = "GPL";
