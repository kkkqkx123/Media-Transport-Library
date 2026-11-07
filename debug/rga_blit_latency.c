/* RGA single-blit latency probe.
 *
 * Allocates two NV12 buffers from the dma32 heap (always below 4 GiB,
 * same choice as the production RGA pool) and times repeated blits.
 * Separates the convert step from capture and encode costs.
 *
 * Build on board:
 *   gcc debug/rga_blit_latency.c -o ~/code/mtl/bin/rga_blit_latency \
 *     -lrga -ldrm -I/usr/include/rga
 */
#include <getopt.h>
#include <rga/RgaApi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

/* dma_heap alloc plumbing matching <linux/dma-heap.h> field order
 * (len, fd, fd_flags); keeps the probe free of kernel header deps. */
#include <linux/ioctl.h>
struct dma_heap_alloc {
  uint64_t len;
  uint32_t fd;
  uint32_t fd_flags;
};
#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOC_ALLOC _IOWR(DMA_HEAP_IOC_MAGIC, 0, struct dma_heap_alloc)

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int cmp_u32(const void* a, const void* b) {
  uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
  return (x > y) - (x < y);
}

static uint32_t pct_u32(uint32_t* v, int n, int p) {
  if (n <= 0) return 0;
  int i = (p * n + 50) / 100 - 1;
  if (i < 0) i = 0;
  if (i >= n) i = n - 1;
  return v[i];
}

static int heap_alloc(const char* heap, size_t len) {
  int hfd = open(heap, O_RDWR);
  if (hfd < 0) return -1;
  struct dma_heap_alloc a;
  memset(&a, 0, sizeof(a));
  a.len = len;
  a.fd_flags = O_RDWR | O_CLOEXEC;
  int r = ioctl(hfd, DMA_HEAP_IOC_ALLOC, &a);
  close(hfd);
  if (r < 0) return -1;
  return (int)a.fd;
}

int main(int argc, char** argv) {
  int w = 1920, h = 1080, loop = 200, dw = 0, dh = 0;
  const char* csv = NULL;
  const char* srcfmt = "nv12"; /* nv12 | bgr3 (production capture path) */

  static struct option opts[] = {
    {"w", required_argument, 0, 'w'},
    {"h", required_argument, 0, 'H'},
    {"loop", required_argument, 0, 'n'},
    {"dw", required_argument, 0, 'x'},
    {"dh", required_argument, 0, 'y'},
    {"csv", required_argument, 0, 'c'},
    {"srcfmt", required_argument, 0, 'F'},
    {0, 0, 0, 0},
  };
  int o;
  while ((o = getopt_long(argc, argv, "w:H:n:x:y:c:F:", opts, NULL)) != -1) {
    switch (o) {
      case 'w': w = atoi(optarg); break;
      case 'H': h = atoi(optarg); break;
      case 'n': loop = atoi(optarg); break;
      case 'x': dw = atoi(optarg); break;
      case 'y': dh = atoi(optarg); break;
      case 'c': csv = optarg; break;
      case 'F': srcfmt = optarg; break;
      default:
        fprintf(stderr, "usage: %s [--w W] [--h H] [--loop N] [--dw W --dh H] [--csv FILE] [--srcfmt nv12|bgr3]\n", argv[0]);
        return 1;
    }
  }
  if (dw <= 0) dw = w;
  if (dh <= 0) dh = h;

  int src_fmt = RK_FORMAT_YCbCr_420_SP;
  size_t src_len = (size_t)w * (size_t)h * 3 / 2;
  if (strcmp(srcfmt, "bgr3") == 0) {
    src_fmt = RK_FORMAT_BGR_888;
    src_len = (size_t)w * (size_t)h * 3;
  } else if (strcmp(srcfmt, "nv12") != 0) {
    fprintf(stderr, "unknown --srcfmt %s\n", srcfmt);
    return 1;
  }
  size_t dst_len = (size_t)dw * (size_t)dh * 3 / 2;
  int src_fd = heap_alloc("/dev/dma_heap/system-dma32", src_len);
  int dst_fd = heap_alloc("/dev/dma_heap/system-dma32", dst_len);
  if (src_fd < 0 || dst_fd < 0) {
    fprintf(stderr, "FAIL: dma32 heap alloc\n");
    return 1;
  }

  uint32_t* cost = calloc((size_t)(loop > 0 ? loop : 1), sizeof(uint32_t));
  if (!cost) return 1;
  int fails = 0;
  for (int i = 0; i < loop; i++) {
    rga_info_t src, dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    src.fd = src_fd;
    src.virAddr = NULL;
    src.mmuFlag = 1;
    rga_set_rect(&src.rect, 0, 0, w, h, w, h, src_fmt);
    dst.fd = dst_fd;
    dst.virAddr = NULL;
    dst.mmuFlag = 1;
    rga_set_rect(&dst.rect, 0, 0, dw, dh, dw, dh, RK_FORMAT_YCbCr_420_SP);
    uint64_t t0 = now_ns();
    int r = c_RkRgaBlit(&src, &dst, NULL);
    uint64_t t1 = now_ns();
    if (r != 0) {
      fails++;
      continue;
    }
    cost[i] = (uint32_t)((t1 - t0) / 1000ULL);
  }

  int ok = loop - fails;
  if (ok > 0) {
    uint32_t* s = malloc((size_t)ok * sizeof(uint32_t));
    int k = 0;
    uint64_t sum = 0;
    for (int i = 0; i < loop; i++) {
      if (cost[i] == 0 && fails > 0) continue;
      s[k++] = cost[i];
      sum += cost[i];
    }
    qsort(s, (size_t)k, sizeof(uint32_t), cmp_u32);
    printf("blit %s %dx%d->%dx%d ok=%d fail=%d mean_us=%.1f p50=%u p90=%u p99=%u\n",
           srcfmt, w, h, dw, dh, ok, fails, (double)sum / k,
           pct_u32(s, k, 50), pct_u32(s, k, 90), pct_u32(s, k, 99));
    free(s);
    if (csv) {
      FILE* cf = fopen(csv, "w");
      if (cf) {
        fprintf(cf, "iter,cost_us\n");
        for (int i = 0; i < loop; i++) fprintf(cf, "%d,%u\n", i, cost[i]);
        fclose(cf);
        printf("written: %s\n", csv);
      }
    }
  } else {
    fprintf(stderr, "FAIL: all %d blits failed\n", loop);
  }

  close(src_fd);
  close(dst_fd);
  free(cost);
  return ok > 0 ? 0 : 1;
}
