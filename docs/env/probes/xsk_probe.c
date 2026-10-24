/* Raw-syscall AF_XDP probe: no libbpf/libxdp dependency.
 * Walks the socket setup MTL's mt_af_xdp.c needs (UMEM reg with the
 * unaligned-chunk flag + ring allocation + bind), which is what decides
 * whether this kernel can serve the native AF_XDP backend at all.
 * App-side mmap is intentionally skipped: bind() only needs the kernel
 * side of the rings allocated. */
#include <errno.h>
#include <linux/if_xdp.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char** argv) {
  const char* ifname = argc > 1 ? argv[1] : "eth1";
  unsigned int chunk = 2048, ring = 64;
  void* mem;
  int fd, ret, ifindex;
  struct sockaddr_xdp sxdp;
  struct xdp_umem_reg umem;
  struct xdp_mmap_offsets off;
  socklen_t optlen;

  ifindex = (int)if_nametoindex(ifname);
  if (ifindex == 0) {
    printf("FAIL if_nametoindex(%s): %s\n", ifname, strerror(errno));
    return 1;
  }

  fd = socket(PF_XDP, SOCK_RAW, 0);
  printf("1) socket(PF_XDP)           -> fd=%d  errno=%d(%s)\n", fd, errno,
         strerror(errno));
  if (fd < 0) return 1;

  mem = mmap(NULL, (size_t)chunk * ring * 2, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    printf("FAIL umem mmap: %s\n", strerror(errno));
    return 1;
  }

  memset(&umem, 0, sizeof(umem));
  umem.addr = (uint64_t)(uintptr_t)mem;
  umem.len = (uint64_t)chunk * ring * 2;
  umem.chunk_size = chunk;
  umem.headroom = 0;
  umem.flags = XDP_UMEM_UNALIGNED_CHUNK_FLAG;
  ret = setsockopt(fd, SOL_XDP, XDP_UMEM_REG, &umem, sizeof(umem));
  printf("2) XDP_UMEM_REG unaligned   -> ret=%d  errno=%d(%s)\n", ret, errno,
         strerror(errno));

  ret = setsockopt(fd, SOL_XDP, XDP_RX_RING, &ring, sizeof(ring));
  printf("3) XDP_RX_RING(%u)          -> ret=%d  errno=%d(%s)\n", ring, ret, errno,
         strerror(errno));

  ret = setsockopt(fd, SOL_XDP, XDP_UMEM_FILL_RING, &ring, sizeof(ring));
  printf("4) XDP_UMEM_FILL_RING       -> ret=%d  errno=%d(%s)\n", ret, errno,
         strerror(errno));

  ret = setsockopt(fd, SOL_XDP, XDP_UMEM_COMPLETION_RING, &ring, sizeof(ring));
  printf("5) XDP_UMEM_COMPLETION_RING -> ret=%d  errno=%d(%s)\n", ret, errno,
         strerror(errno));

  memset(&off, 0, sizeof(off));
  optlen = sizeof(off);
  ret = getsockopt(fd, SOL_XDP, XDP_MMAP_OFFSETS, &off, &optlen);
  printf("6) XDP_MMAP_OFFSETS         -> ret=%d optlen=%u\n", ret, optlen);

  memset(&sxdp, 0, sizeof(sxdp));
  sxdp.sxdp_family = PF_XDP;
  sxdp.sxdp_ifindex = ifindex;
  sxdp.sxdp_queue_id = 0;
  sxdp.sxdp_flags = 0;
  ret = bind(fd, (struct sockaddr*)&sxdp, sizeof(sxdp));
  printf("7) bind(%s,q0)              -> ret=%d  errno=%d(%s)\n", ifname, ret, errno,
         strerror(errno));

  if (ret == 0)
    printf("RESULT: AF_XDP socket fully usable on %s\n", ifname);
  else if (errno == ENOENT || errno == EINVAL)
    printf("RESULT: AF_XDP present but bind rejected (%s)\n", strerror(errno));
  return 0;
}
