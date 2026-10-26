⚠️ 一个关键限制（文档已标注）
YT6801 驱动 fuxi-gmac-net.c 里只有被注释掉的 XDP ring，没有实现 ndo_xdp_xmit/XDP_ZEROCOPY。即：在该 6.1 树上，AF_XDP 在 eth0 上只能跑 skb（拷贝）模式，不能零拷贝。MTL native_af_xdp 后端仍可用，但吞吐/延迟受限于拷贝；若要 zc 性能，需另给 YT6801 驱动补 XDP 钩子（不在当前补丁范围）。排障时若见 “zero-copy not supported” 属预期，不是配置漏开。
