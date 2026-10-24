/* Minimal XDP probe program: no CO-RE, no libbpf headers.
 * Returns XDP_PASS unconditionally so it is transparent to traffic.
 * Used only to test whether the kernel/driver path for XDP attach works. */
#include <linux/bpf.h>
#include <linux/types.h>
#include <linux/pkt_cls.h>

#define SEC(s) __attribute__((section(s), used))

struct xdp_md;

SEC("xdp")
int xdp_pass_probe(struct xdp_md *ctx)
{
	return XDP_PASS;
}

SEC("xdp")
int xdp_drop_probe(struct xdp_md *ctx)
{
	return XDP_DROP;
}

char _license[] SEC("license") = "GPL";
