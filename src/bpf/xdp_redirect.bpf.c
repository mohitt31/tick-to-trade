// Redirects the feed's UDP flow to an AF_XDP socket; everything else goes on
// to the kernel.
//
// A packet is the feed's if it is IPv4 UDP to the configured destination
// address and port. Those go to the socket bound on the queue they arrived on.
// ARP, IGMP joins and anything else pass through untouched: the sender still
// has to resolve this host's MAC address, and the switch still has to learn the
// multicast group.
//
// A feed packet with no socket on its queue is passed to the kernel rather than
// dropped, and counted. That case is the classic way AF_XDP "loses" traffic
// with no error anywhere (the NIC spread flows across queues and the socket is
// on another one), so it gets its own counter instead of a silent drop.
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>

#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

struct config {
    __u32 dst_ip;    // network byte order; 0 matches any
    __u16 dst_port;  // network byte order
    __u16 pad;
};

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct config);
} ttt_config SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 64);
    __type(key, __u32);
    __type(value, __u32);
} ttt_xsks SEC(".maps");

enum {
    COUNT_REDIRECTED,  // feed packets handed to a socket
    COUNT_NO_SOCKET,   // feed packets with no socket on their queue
    COUNT_PASSED,      // everything else
    COUNT_MAX,
};

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, COUNT_MAX);
    __type(key, __u32);
    __type(value, __u64);
} ttt_counters SEC(".maps");

static void count(__u32 which) {
    __u64* c = bpf_map_lookup_elem(&ttt_counters, &which);
    if (c) {
        *c += 1;
    }
}

enum verdict { PASS, REDIRECT, NO_SOCKET };

static __always_inline enum verdict classify(struct xdp_md* ctx) {
    void* data = (void*)(long)ctx->data;
    void* end = (void*)(long)ctx->data_end;

    struct ethhdr* eth = data;
    if ((void*)(eth + 1) > end || eth->h_proto != bpf_htons(ETH_P_IP)) {
        return PASS;
    }
    struct iphdr* ip = (void*)(eth + 1);
    if ((void*)(ip + 1) > end || ip->protocol != IPPROTO_UDP || ip->ihl < 5) {
        return PASS;
    }
    struct udphdr* udp = (void*)ip + ip->ihl * 4;
    if ((void*)(udp + 1) > end) {
        return PASS;
    }

    __u32          zero = 0;
    struct config* cfg = bpf_map_lookup_elem(&ttt_config, &zero);
    if (!cfg || udp->dest != cfg->dst_port || (cfg->dst_ip != 0 && ip->daddr != cfg->dst_ip)) {
        return PASS;
    }
    __u32 queue = ctx->rx_queue_index;
    return bpf_map_lookup_elem(&ttt_xsks, &queue) ? REDIRECT : NO_SOCKET;
}

SEC("xdp")
int ttt_xdp_redirect(struct xdp_md* ctx) {
    switch (classify(ctx)) {
        case REDIRECT: count(COUNT_REDIRECTED); return bpf_redirect_map(&ttt_xsks, ctx->rx_queue_index, XDP_PASS);
        case NO_SOCKET: count(COUNT_NO_SOCKET); return XDP_PASS;
        default: count(COUNT_PASSED); return XDP_PASS;
    }
}

// The same, and it also puts the NIC's receive timestamp in front of every
// redirected packet, as XDP metadata the AF_XDP socket delivers with the frame.
// The timestamp comes from a kfunc the driver implements, so this program has to
// be loaded bound to its device, and a device-bound program cannot run in
// generic mode.
extern int bpf_xdp_metadata_rx_timestamp(const struct xdp_md* ctx, __u64* timestamp) __ksym;

struct rx_meta {
    __u64 timestamp;  // the NIC's clock, nanoseconds; 0 if none
    __s32 rc;         // what the kfunc returned: 0, or -EOPNOTSUPP / -ENODATA
    __u32 magic;      // 0x54544d31, "TTM1"
};

SEC("xdp")
int ttt_xdp_redirect_ts(struct xdp_md* ctx) {
    switch (classify(ctx)) {
        case REDIRECT: break;
        case NO_SOCKET: count(COUNT_NO_SOCKET); return XDP_PASS;
        default: count(COUNT_PASSED); return XDP_PASS;
    }
    if (bpf_xdp_adjust_meta(ctx, -(int)sizeof(struct rx_meta)) == 0) {
        struct rx_meta* meta = (void*)(long)ctx->data_meta;
        if ((void*)(meta + 1) <= (void*)(long)ctx->data) {
            __u64 ts = 0;
            meta->rc = bpf_xdp_metadata_rx_timestamp(ctx, &ts);
            meta->timestamp = meta->rc == 0 ? ts : 0;
            meta->magic = 0x54544d31;
        }
    }
    count(COUNT_REDIRECTED);
    return bpf_redirect_map(&ttt_xsks, ctx->rx_queue_index, XDP_PASS);
}

char LICENSE[] SEC("license") = "Dual MIT/GPL";
