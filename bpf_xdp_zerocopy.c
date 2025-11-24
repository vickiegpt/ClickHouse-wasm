/*
 * eBPF XDP program for zero-copy packet processing
 * Attaches to network interfaces for direct packet handling
 *
 * Compile with:
 *   clang -O2 -target bpf -c bpf_xdp_zerocopy.c -o bpf_xdp_zerocopy.o
 */

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/in.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

/* Map for statistics */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __type(key, __u32);
    __type(value, __u64);
    __uint(max_entries, 256);
} stats_map SEC(".maps");

/* Map for configuration */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __type(key, __u32);
    __type(value, __u32);
    __uint(max_entries, 16);
} config_map SEC(".maps");

/* Statistics indices */
#define STAT_RX_PACKETS      0
#define STAT_RX_BYTES        1
#define STAT_TX_PACKETS      2
#define STAT_TX_BYTES        3
#define STAT_ZEROCOPY_HITS   4
#define STAT_DROPPED         5

/* Config indices */
#define CFG_ZEROCOPY_ENABLE  0
#define CFG_ZEROCOPY_PORT    1
#define CFG_BYPASS_MODE      2

static __always_inline void update_stat(__u32 index, __u64 value)
{
    __u64 *stat = bpf_map_lookup_elem(&stats_map, &index);
    if (stat)
        __sync_fetch_and_add(stat, value);
}

static __always_inline __u32 get_config(__u32 index)
{
    __u32 *val = bpf_map_lookup_elem(&config_map, &index);
    return val ? *val : 0;
}

/* Main XDP program */
SEC("xdp")
int xdp_zerocopy_prog(struct xdp_md *ctx)
{
    void *data_end = (void *)(long)ctx->data_end;
    void *data = (void *)(long)ctx->data;

    /* Parse Ethernet header */
    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end)
        return XDP_DROP;

    update_stat(STAT_RX_PACKETS, 1);
    update_stat(STAT_RX_BYTES, data_end - data);

    /* Only process IP packets */
    if (eth->h_proto != bpf_htons(ETH_P_IP))
        return XDP_PASS;

    /* Parse IP header */
    struct iphdr *iph = (struct iphdr *)(eth + 1);
    if ((void *)(iph + 1) > data_end)
        return XDP_DROP;

    /* Check if zero-copy is enabled */
    __u32 zerocopy_enabled = get_config(CFG_ZEROCOPY_ENABLE);
    if (!zerocopy_enabled)
        return XDP_PASS;

    /* Handle TCP packets */
    if (iph->protocol == IPPROTO_TCP) {
        struct tcphdr *tcph = (struct tcphdr *)(iph + 1);
        if ((void *)(tcph + 1) > data_end)
            return XDP_DROP;

        __u32 dport = bpf_ntohs(tcph->dest);
        __u32 zerocopy_port = get_config(CFG_ZEROCOPY_PORT);

        /* If this is our target port, mark for zero-copy */
        if (zerocopy_port && dport == zerocopy_port) {
            update_stat(STAT_ZEROCOPY_HITS, 1);
            /* In real implementation, would set metadata for bypass path */
        }
    }

    return XDP_PASS;
}

/* Program for TX offload */
SEC("xdp")
int xdp_zerocopy_tx(struct xdp_md *ctx)
{
    void *data_end = (void *)(long)ctx->data_end;
    void *data = (void *)(long)ctx->data;

    update_stat(STAT_TX_PACKETS, 1);
    update_stat(STAT_TX_BYTES, data_end - data);

    return XDP_TX;
}

char _license[] SEC("license") = "GPL";
