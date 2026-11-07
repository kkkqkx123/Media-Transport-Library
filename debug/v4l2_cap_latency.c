/* V4L2 capture interval probe.
 *
 * Streams a few hundred frames from the HDMI receiver and reports the
 * dequeue interval distribution. Setup path mirrors the production
 * negotiation (S_FMT with num_planes set, G_FMT read-back verify,
 * O_NONBLOCK, per-plane mmap): the receiver rejects S_FMT calls that
 * leave num_planes at zero, even for listed formats.
 *
 * Build on board: gcc debug/v4l2_cap_latency.c -o ~/code/mtl/bin/v4l2_cap_latency
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MAX_FRAMES 3600
#define NBUFS 6
#define MAX_PLANES 2

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void* a, const void* b) {
  uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
  return (x > y) - (x < y);
}

static uint64_t pct(uint64_t* v, int n, int p) {
  if (n <= 0) return 0;
  int i = (p * n + 50) / 100 - 1;
  if (i < 0) i = 0;
  if (i >= n) i = n - 1;
  return v[i];
}

static uint32_t fourcc_of(const char* s) {
  return ((uint32_t)s[0]) | ((uint32_t)s[1] << 8) |
         ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24);
}

/* One S_FMT attempt with the plane count set, verified by G_FMT read-back.
 * Returns 1 on negotiated match, 0 on refused/coerced. */
static int try_fmt(int fd, uint32_t fourcc, uint32_t w, uint32_t h,
                   struct v4l2_format* negotiated) {
  struct v4l2_format sfmt;
  memset(&sfmt, 0, sizeof(sfmt));
  sfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  sfmt.fmt.pix_mp.width = w;
  sfmt.fmt.pix_mp.height = h;
  sfmt.fmt.pix_mp.pixelformat = fourcc;
  sfmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
  sfmt.fmt.pix_mp.num_planes = 1;
  if (ioctl(fd, VIDIOC_S_FMT, &sfmt) < 0) return 0;
  memset(negotiated, 0, sizeof(*negotiated));
  negotiated->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (ioctl(fd, VIDIOC_G_FMT, negotiated) < 0) return 0;
  return negotiated->fmt.pix_mp.pixelformat == fourcc;
}

int main(int argc, char** argv) {
  const char* dev = "/dev/video0";
  int count = 300;
  const char* csv = NULL;
  const char* only = NULL; /* force a single fourcc, e.g. NV12 */
  int no_stream = 0; /* negotiation only: exit after S_FMT/G_FMT verify */

  static struct option opts[] = {
    {"device", required_argument, 0, 'd'},
    {"count", required_argument, 0, 'n'},
    {"csv", required_argument, 0, 'c'},
    {"pixfmt", required_argument, 0, 'p'},
    {"no-stream", no_argument, 0, 's'},
    {0, 0, 0, 0},
  };
  int o;
  while ((o = getopt_long(argc, argv, "d:n:c:p:s", opts, NULL)) != -1) {
    switch (o) {
      case 'd': dev = optarg; break;
      case 'n': count = atoi(optarg); break;
      case 'c': csv = optarg; break;
      case 'p': only = optarg; break;
      case 's': no_stream = 1; break;
      default:
        fprintf(stderr, "usage: %s [--device DEV] [--count N] [--csv FILE] [--pixfmt FCC] [--no-stream]\n", argv[0]);
        return 1;
    }
  }
  if (count < 2) count = 2;
  if (count > MAX_FRAMES) count = MAX_FRAMES;

  int fd = open(dev, O_RDWR | O_NONBLOCK);
  if (fd < 0) { perror("open"); return 1; }

  struct v4l2_format fmt;
  memset(&fmt, 0, sizeof(fmt));
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (ioctl(fd, VIDIOC_G_FMT, &fmt) < 0) { perror("G_FMT"); close(fd); return 1; }
  uint32_t rw = fmt.fmt.pix_mp.width, rh = fmt.fmt.pix_mp.height;

  static const char* cands[] = {"NV12", "NV16", "NV24", "BGR3", "UYVY", "YUYV"};
  const char* single[1] = {only};
  const char** list = only ? single : cands;
  int nlist = only ? 1 : 6;

  int negotiated = 0;
  for (int i = 0; i < nlist; i++) {
    struct v4l2_format next;
    uint32_t f = fourcc_of(list[i]);
    if (try_fmt(fd, f, rw, rh, &next)) {
      fmt = next;
      negotiated = 1;
      printf("negotiated %.4s %ux%u planes=%u\n", list[i],
             next.fmt.pix_mp.width, next.fmt.pix_mp.height,
             next.fmt.pix_mp.num_planes);
      break;
    }
    fprintf(stderr, "note: S_FMT %.4s refused/coerced\n", list[i]);
  }
  if (!negotiated) {
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd, VIDIOC_G_FMT, &fmt) < 0) { perror("G_FMT"); close(fd); return 1; }
    printf("keeping driver format %.4s %ux%u\n",
           (char*)&fmt.fmt.pix_mp.pixelformat,
           fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height);
  }

  if (no_stream) { close(fd); return 0; }

  uint32_t nplanes = fmt.fmt.pix_mp.num_planes;
  if (nplanes < 1) nplanes = 1;
  if (nplanes > MAX_PLANES) nplanes = MAX_PLANES;

  struct v4l2_requestbuffers req;
  memset(&req, 0, sizeof(req));
  req.count = NBUFS;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  req.memory = V4L2_MEMORY_MMAP;
  if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); close(fd); return 1; }
  printf("reqbufs granted: %u\n", req.count);
  if (req.count == 0) {
    fprintf(stderr, "FAIL: REQBUFS granted 0 buffers\n");
    close(fd);
    return 1;
  }
  uint32_t nb = req.count;
  if (nb > NBUFS) nb = NBUFS;

  void* addr[NBUFS][MAX_PLANES];
  size_t lens[NBUFS][MAX_PLANES];
  memset(addr, 0, sizeof(addr));
  for (uint32_t i = 0; i < nb; i++) {
    struct v4l2_buffer b;
    struct v4l2_plane planes[MAX_PLANES];
    memset(&b, 0, sizeof(b));
    memset(planes, 0, sizeof(planes));
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = i;
    b.length = nplanes;
    b.m.planes = planes;
    if (ioctl(fd, VIDIOC_QUERYBUF, &b) < 0) { perror("QUERYBUF"); close(fd); return 1; }
    for (uint32_t p = 0; p < nplanes; p++) {
      lens[i][p] = planes[p].length;
      addr[i][p] = mmap(NULL, planes[p].length, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, planes[p].m.mem_offset);
      if (addr[i][p] == MAP_FAILED) { perror("mmap"); close(fd); return 1; }
    }
    if (ioctl(fd, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); close(fd); return 1; }
  }

  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); close(fd); return 1; }

  uint64_t* ts = calloc((size_t)count, sizeof(uint64_t));
  if (!ts) { close(fd); return 1; }
  int got = 0, timeouts = 0, empty = 0;
  struct pollfd pfd = {.fd = fd, .events = POLLIN | POLLPRI, .revents = 0};
  while (got < count) {
    pfd.revents = 0;
    int r = poll(&pfd, 1, 1000);
    if (r < 0) {
      if (errno == EINTR) continue;
      perror("poll");
      break;
    }
    if (r == 0) {
      if (++timeouts > 12) {
        fprintf(stderr, "FAIL: poll timeout x12, streaming stalls\n");
        break;
      }
      continue;
    }
    if (!(pfd.revents & POLLIN)) continue;
    struct v4l2_buffer b;
    struct v4l2_plane planes[MAX_PLANES];
    memset(&b, 0, sizeof(b));
    memset(planes, 0, sizeof(planes));
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.length = nplanes;
    b.m.planes = planes;
    if (ioctl(fd, VIDIOC_DQBUF, &b) < 0) {
      if (errno == EAGAIN) continue;
      perror("DQBUF");
      break;
    }
    size_t used = planes[0].bytesused;
    if (nplanes > 1) used += planes[1].bytesused;
    if (used == 0) {
      empty++;
      if (ioctl(fd, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); break; }
      continue;
    }
    ts[got++] = now_ns();
    if (ioctl(fd, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); break; }
  }
  ioctl(fd, VIDIOC_STREAMOFF, &type);

  printf("frames=%d timeouts=%d empty=%d\n", got, timeouts, empty);
  if (got < 2) {
    fprintf(stderr, "FAIL: only %d frames\n", got);
    free(ts);
    close(fd);
    return 1;
  }

  int n = got - 1;
  uint64_t* d = malloc((size_t)n * sizeof(uint64_t));
  for (int i = 0; i < n; i++) d[i] = ts[i + 1] - ts[i];
  uint64_t* s = malloc((size_t)n * sizeof(uint64_t));
  memcpy(s, d, (size_t)n * sizeof(uint64_t));
  qsort(s, (size_t)n, sizeof(uint64_t), cmp_u64);
  uint64_t sum = 0, mx = 0;
  for (int i = 0; i < n; i++) { sum += d[i]; if (d[i] > mx) mx = d[i]; }
  printf("interval_us mean=%.1f p50=%llu p90=%llu p99=%llu max=%llu\n",
         (double)(sum / 1000) / n,
         (unsigned long long)(pct(s, n, 50) / 1000),
         (unsigned long long)(pct(s, n, 90) / 1000),
         (unsigned long long)(pct(s, n, 99) / 1000),
         (unsigned long long)(mx / 1000));

  if (csv) {
    FILE* f = fopen(csv, "w");
    if (f) {
      fprintf(f, "frame,interval_us\n");
      for (int i = 0; i < n; i++)
        fprintf(f, "%d,%llu\n", i, (unsigned long long)(d[i] / 1000));
      fclose(f);
      printf("written: %s\n", csv);
    }
  }

  free(d);
  free(s);
  free(ts);
  close(fd);
  return 0;
}
