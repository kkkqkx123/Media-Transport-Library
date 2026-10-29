/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 *
 * V4L2 HDMI capture using Multiplanar API (required by RK3588 rk_hdmirx).
 * Captures NV12/BGR3 frames and exports dma-buf fds for zero-copy to MPP.
 *
 * Robustness notes (mirrors zlmediakit HdmiInV4l2Capture):
 *  - S_FMT is never trusted: G_FMT is read back and the fourcc must match,
 *    otherwise the next candidate format is tried.
 *  - Placeholder resolutions (<=128x128) are treated as "no signal" and fail
 *    with -ENODATA so the caller can retry later.
 *  - Kernel frame timestamps (V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) are
 *    preferred; dequeue time is only a fallback. PTS never goes backwards.
 *  - Buffers are re-queued only via v4l2_capturer_release(): generation +
 *    claim-bit CAS guarantees each buffer is queued at most once per frame,
 *    protecting against double-release and use-after-requeue races with the
 *    async MPP/RGA consumers.
 */

#include "v4l2_capturer.h"
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MAX_PLANES 2

/* release_state per buffer: low bit = claimed, high 63 bits = generation */
#define V4L2_CLAIM_BIT 1ULL

struct v4l2_buf_info {
  int   dma_fd[MAX_PLANES];   /* exported dma-buf fd per plane */
  void* v4l2_buf[MAX_PLANES]; /* mmap'd pointers */
  int   num_planes;
  uint32_t bytesused[MAX_PLANES];
  uint32_t length[MAX_PLANES];
  atomic_uint_fast64_t release_state;
};

struct v4l2_capturer {
  int    fd;
  int    width, height;
  float  fps;
  int    buf_count;
  uint32_t actual_fourcc;   /* actual negotiated pixel format */
  int    actual_num_planes; /* actual number of planes */
  uint32_t plane_stride;    /* bytesperline for plane 0 (byte stride, not pixels!) */

  struct v4l2_buf_info* bufs;

  v4l2_frame_cb cb;
  void*         user_data;

  v4l2_error_cb         error_cb;
  void*                 error_user;
  v4l2_signal_change_cb signal_cb;
  void*                 signal_user;

  atomic_bool running;
  pthread_t   thread;

  uint64_t last_ts_ns;      /* monotonic clamp */
  bool     ts_fallback_warned;
  atomic_int outstanding;   /* dequeued but not yet released */
};

/* ---- helpers ---- */

static uint64_t gettime_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int xioctl(int fd, unsigned long req, void* arg) {
  int r;
  do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
  return r;
}

/* Try one candidate format: S_FMT then G_FMT read-back verify. Returns 0 if
 * the driver accepted the requested fourcc unchanged, -1 otherwise. */
static int try_format(struct v4l2_capturer* c, uint32_t fourcc, int num_planes,
                      struct v4l2_format* out) {
  struct v4l2_format fmt = {0};
  fmt.type                   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  fmt.fmt.pix_mp.width       = (uint32_t)c->width;
  fmt.fmt.pix_mp.height      = (uint32_t)c->height;
  fmt.fmt.pix_mp.pixelformat = fourcc;
  fmt.fmt.pix_mp.field       = V4L2_FIELD_NONE;
  fmt.fmt.pix_mp.num_planes  = (uint32_t)num_planes;
  if (xioctl(c->fd, VIDIOC_S_FMT, &fmt) < 0) return -1;
  /* G_FMT read-back: rkcif-style drivers may silently rewrite the format */
  struct v4l2_format got = {0};
  got.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (xioctl(c->fd, VIDIOC_G_FMT, &got) < 0) return -1;
  if (got.fmt.pix_mp.pixelformat != fourcc) {
    fprintf(stderr,
            "v4l2_capturer: driver rewrote fourcc '%.4s' -> '%.4s'\n",
            (char*)&fourcc, (char*)&got.fmt.pix_mp.pixelformat);
    return -1;
  }
  *out = got;
  return 0;
}

/* ---- create / free ---- */

struct v4l2_capturer* v4l2_capturer_create(const char* device,
                                            int width, int height, float fps,
                                            int buf_count,
                                            v4l2_frame_cb cb, void* user_data) {
  struct v4l2_capturer* c = calloc(1, sizeof(*c));
  if (!c) return NULL;
  c->width  = width;
  c->height = height;
  c->fps    = fps;
  c->buf_count = buf_count;
  c->cb     = cb;
  c->user_data = user_data;
  c->fd     = -1;
  atomic_init(&c->running, false);
  atomic_init(&c->outstanding, 0);

  /* open device */
  c->fd = open(device, O_RDWR);
  if (c->fd < 0) {
    perror("v4l2_capturer: open device");
    free(c);
    return NULL;
  }

  /* format negotiation: candidate list with G_FMT read-back verification */
  static const struct { uint32_t fourcc; int planes; } candidates[] = {
      {V4L2_PIX_FMT_NV12, 2},  /* zero-copy direct to MPP */
      {V4L2_PIX_FMT_BGR24, 1}, /* default for rk_hdmirx */
  };
  struct v4l2_format fmt = {0};
  bool fmt_ok = false;
  for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
    if (try_format(c, candidates[i].fourcc, candidates[i].planes, &fmt) == 0) {
      fmt_ok = true;
      break;
    }
  }
  if (!fmt_ok) {
    fprintf(stderr, "v4l2_capturer: no supported format negotiated\n");
    close(c->fd); free(c); return NULL;
  }
  c->actual_fourcc     = fmt.fmt.pix_mp.pixelformat;
  c->actual_num_planes = (int)fmt.fmt.pix_mp.num_planes;
  c->plane_stride      = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;

  /* placeholder resolution = no signal: let the caller retry later */
  if (fmt.fmt.pix_mp.width <= 128 || fmt.fmt.pix_mp.height <= 128) {
    fprintf(stderr,
            "v4l2_capturer: placeholder resolution %ux%u (no signal?)\n",
            fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height);
    close(c->fd); free(c); return NULL;
  }
  printf("v4l2_capturer: fmt=%ux%u fourcc=0x%x ('%.4s') planes=%d stride=%u\n",
         fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
         fmt.fmt.pix_mp.pixelformat, (char*)&fmt.fmt.pix_mp.pixelformat,
         fmt.fmt.pix_mp.num_planes,
         fmt.fmt.pix_mp.plane_fmt[0].bytesperline);

  /* set framerate (best effort — some drivers reject S_PARM) */
  struct v4l2_streamparm parm = {0};
  parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  parm.parm.capture.timeperframe.numerator   = 1;
  parm.parm.capture.timeperframe.denominator = (int)fps;
  if (xioctl(c->fd, VIDIOC_S_PARM, &parm) < 0)
    fprintf(stderr, "v4l2_capturer: VIDIOC_S_PARM failed (continuing)\n");

  /* request MMAP buffers */
  struct v4l2_requestbuffers req = {0};
  req.count  = (uint32_t)buf_count;
  req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(c->fd, VIDIOC_REQBUFS, &req) < 0 || req.count == 0) {
    perror("v4l2_capturer: VIDIOC_REQBUFS");
    close(c->fd); free(c); return NULL;
  }
  c->buf_count = (int)req.count; /* driver may have lowered it */
  printf("v4l2_capturer: allocated %d buffers\n", c->buf_count);

  /* mmap and export */
  c->bufs = calloc((size_t)c->buf_count, sizeof(struct v4l2_buf_info));
  for (int i = 0; i < c->buf_count; i++) {
    struct v4l2_buffer buf = {0};
    struct v4l2_plane planes[MAX_PLANES] = {0};
    buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory   = V4L2_MEMORY_MMAP;
    buf.index    = (uint32_t)i;
    buf.m.planes = planes;
    buf.length   = MAX_PLANES;
    if (xioctl(c->fd, VIDIOC_QUERYBUF, &buf) < 0) {
      perror("v4l2_capturer: VIDIOC_QUERYBUF");
      goto fail;
    }
    c->bufs[i].num_planes = (buf.length < MAX_PLANES) ? (int)buf.length : MAX_PLANES;

    for (int p = 0; p < c->bufs[i].num_planes; p++) {
      c->bufs[i].bytesused[p] = planes[p].length; /* updated at DQBUF */
      c->bufs[i].length[p]   = planes[p].length;
      c->bufs[i].v4l2_buf[p] = mmap(NULL, planes[p].length,
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED, c->fd, planes[p].m.mem_offset);
      if (c->bufs[i].v4l2_buf[p] == MAP_FAILED) {
        perror("v4l2_capturer: mmap");
        goto fail;
      }
      /* export as dma-buf */
      struct v4l2_exportbuffer exp = {0};
      exp.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
      exp.index = (uint32_t)i;
      exp.plane = (uint32_t)p;
      exp.flags = O_CLOEXEC;
      if (xioctl(c->fd, VIDIOC_EXPBUF, &exp) < 0) {
        perror("v4l2_capturer: VIDIOC_EXPBUF");
        goto fail;
      }
      c->bufs[i].dma_fd[p] = exp.fd;
    }
    atomic_init(&c->bufs[i].release_state, 0);
    printf("v4l2_capturer: buf[%d] planes=%d fd[0]=%d len=%u\n",
           i, c->bufs[i].num_planes, c->bufs[i].dma_fd[0],
           c->bufs[i].length[0]);
  }

  /* queue all buffers */
  for (int i = 0; i < c->buf_count; i++) {
    struct v4l2_buffer buf = {0};
    struct v4l2_plane planes[MAX_PLANES] = {0};
    buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory   = V4L2_MEMORY_MMAP;
    buf.index    = (uint32_t)i;
    buf.m.planes = planes;
    buf.length   = (uint32_t)c->bufs[i].num_planes;
    for (int p = 0; p < c->bufs[i].num_planes; p++)
      planes[p].length = c->bufs[i].length[p];
    if (xioctl(c->fd, VIDIOC_QBUF, &buf) < 0) {
      perror("v4l2_capturer: VIDIOC_QBUF");
      goto fail;
    }
  }
  return c;

fail:
  for (int j = 0; j < c->buf_count; j++) {
    for (int p = 0; p < c->bufs[j].num_planes; p++) {
      if (c->bufs[j].v4l2_buf[p] && c->bufs[j].v4l2_buf[p] != MAP_FAILED)
        munmap(c->bufs[j].v4l2_buf[p], c->bufs[j].length[p]);
      if (c->bufs[j].dma_fd[p] >= 0) close(c->bufs[j].dma_fd[p]);
    }
  }
  free(c->bufs);
  close(c->fd);
  free(c);
  return NULL;
}

void v4l2_capturer_free(struct v4l2_capturer* c) {
  if (!c) return;
  v4l2_capturer_stop(c);
  for (int i = 0; i < c->buf_count; i++) {
    for (int p = 0; p < c->bufs[i].num_planes; p++) {
      if (c->bufs[i].v4l2_buf[p]) munmap(c->bufs[i].v4l2_buf[p], c->bufs[i].length[p]);
      if (c->bufs[i].dma_fd[p] >= 0) close(c->bufs[i].dma_fd[p]);
    }
  }
  free(c->bufs);
  if (c->fd >= 0) close(c->fd);
  free(c);
}

/* ---- release (exactly-once requeue) ---- */

void v4l2_capturer_release(struct v4l2_capturer* c, uint32_t buf_index,
                           uint64_t generation) {
  if (!c || buf_index >= (uint32_t)c->buf_count) return;
  struct v4l2_buf_info* bi = &c->bufs[buf_index];
  uint64_t expected = generation << 1; /* unclaimed, this generation */
  uint64_t want     = expected | V4L2_CLAIM_BIT;
  uint64_t cur = atomic_load(&bi->release_state);
  if (cur != expected) {
    /* already claimed, stale generation, or recycled — ignore */
    return;
  }
  if (!atomic_compare_exchange_strong(&bi->release_state, &cur, want))
    return; /* lost the race — another release won */
  atomic_fetch_sub(&c->outstanding, 1);
  struct v4l2_buffer buf = {0};
  struct v4l2_plane planes[MAX_PLANES] = {0};
  buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  buf.memory   = V4L2_MEMORY_MMAP;
  buf.index    = buf_index;
  buf.m.planes = planes;
  buf.length   = (uint32_t)bi->num_planes;
  for (int p = 0; p < bi->num_planes; p++)
    planes[p].length = bi->length[p];
  if (xioctl(c->fd, VIDIOC_QBUF, &buf) < 0)
    perror("v4l2_capturer: release QBUF");
}

void v4l2_capturer_set_error_cb(struct v4l2_capturer* c,
                                v4l2_error_cb cb, void* user_data) {
  if (!c) return;
  c->error_cb   = cb;
  c->error_user = user_data;
}

void v4l2_capturer_set_signal_change_cb(struct v4l2_capturer* c,
                                        v4l2_signal_change_cb cb,
                                        void* user_data) {
  if (!c) return;
  c->signal_cb   = cb;
  c->signal_user = user_data;
}

/* ---- capture thread ---- */

/* Dequeue pending V4L2 events: SOURCE_CHANGE → signal callback (payload not
 * parsed; the owner rebuilds). Returns 0 on success, -1 on poll error. */
static int dequeue_events(struct v4l2_capturer* c) {
  struct v4l2_event ev;
  while (xioctl(c->fd, VIDIOC_DQEVENT, &ev) == 0) {
    if (ev.type == V4L2_EVENT_SOURCE_CHANGE && c->signal_cb)
      c->signal_cb(c->signal_user);
  }
  return 0;
}

static void* capture_thread(void* arg) {
  struct v4l2_capturer* c = (struct v4l2_capturer*)arg;

  /* subscribe source-change events (hot-plug / resolution change) */
  struct v4l2_event_subscription sub = {0};
  sub.type = V4L2_EVENT_SOURCE_CHANGE;
  if (xioctl(c->fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0)
    fprintf(stderr, "v4l2_capturer: SOURCE_CHANGE subscribe unsupported\n");

  /* start streaming */
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (xioctl(c->fd, VIDIOC_STREAMON, &type) < 0) {
    perror("v4l2_capturer: VIDIOC_STREAMON");
    if (c->error_cb) c->error_cb(c->error_user);
    return NULL;
  }
  printf("v4l2_capturer: streaming @ %dx%d %.1ffps\n",
         c->width, c->height, (double)c->fps);

  uint64_t frame_seq = 0;
  while (atomic_load(&c->running)) {
    /* poll for data and out-of-band events */
    struct pollfd pfd = {.fd = c->fd, .events = POLLIN | POLLPRI};
    int pr = poll(&pfd, 1, 100);
    if (pr < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      perror("v4l2_capturer: poll");
      if (c->error_cb) c->error_cb(c->error_user);
      goto out;
    }
    if (pr == 0) continue; /* idle tick: re-check running */
    if (pfd.revents & POLLPRI) dequeue_events(c);
    if (!(pfd.revents & POLLIN)) continue;

    struct v4l2_buffer buf = {0};
    struct v4l2_plane planes[MAX_PLANES] = {0};
    buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory   = V4L2_MEMORY_MMAP;
    buf.m.planes = planes;
    buf.length   = MAX_PLANES;

    if (xioctl(c->fd, VIDIOC_DQBUF, &buf) < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      perror("v4l2_capturer: VIDIOC_DQBUF");
      if (c->error_cb) c->error_cb(c->error_user);
      goto out;
    }

    /* bump generation, clear claim bit */
    struct v4l2_buf_info* bi = &c->bufs[buf.index];
    uint64_t gen = (atomic_load(&bi->release_state) >> 1) + 1;
    atomic_store(&bi->release_state, gen << 1);
    atomic_fetch_add(&c->outstanding, 1);

    /* timestamp: prefer the kernel frame stamp (ISP time), fall back to
     * dequeue time; never let PTS go backwards */
    uint64_t ts;
    if ((buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) ==
            V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC &&
        (buf.timestamp.tv_sec || buf.timestamp.tv_usec)) {
      ts = (uint64_t)buf.timestamp.tv_sec * 1000000000ULL +
           (uint64_t)buf.timestamp.tv_usec * 1000ULL;
    } else {
      ts = gettime_ns();
      if (!c->ts_fallback_warned) {
        fprintf(stderr,
                "v4l2_capturer: driver timestamp unavailable, using dequeue "
                "time (once)\n");
        c->ts_fallback_warned = true;
      }
    }
    if (ts < c->last_ts_ns) ts = c->last_ts_ns;
    c->last_ts_ns = ts;

    uint32_t total_size = 0;
    for (int p = 0; p < bi->num_planes; p++)
      total_size += planes[p].bytesused;
    if (total_size == 0) { /* empty frame: return buffer, skip callback */
      v4l2_capturer_release(c, buf.index, gen);
      continue;
    }

    struct v4l2_frame f = {
        .dma_fd       = bi->dma_fd[0],
        .dma_fd_uv    = (bi->num_planes >= 2) ? bi->dma_fd[1] : -1,
        .stride       = c->plane_stride,
        .size         = total_size,
        .timestamp_ns = ts,
        .frame_seq    = frame_seq,
        .buf_index    = buf.index,
        .generation   = gen,
    };
    c->cb(c->user_data, &f);
    frame_seq++;
    /* buffer stays held until the consumer calls v4l2_capturer_release() */
  }

out:
  xioctl(c->fd, VIDIOC_STREAMOFF, &type);
  printf("v4l2_capturer: stopped, %llu frames\n", (unsigned long long)frame_seq);
  return NULL;
}

int v4l2_capturer_start(struct v4l2_capturer* c) {
  atomic_store(&c->running, true);
  return pthread_create(&c->thread, NULL, capture_thread, c);
}

void v4l2_capturer_stop(struct v4l2_capturer* c) {
  if (!c) return;
  if (!atomic_exchange(&c->running, false)) return;
  if (c->thread) {
    pthread_join(c->thread, NULL);
    c->thread = 0;
  }
}

uint32_t v4l2_capturer_fourcc(struct v4l2_capturer* c) {
  return c ? c->actual_fourcc : V4L2_PIX_FMT_NV12;
}

int v4l2_capturer_num_planes(struct v4l2_capturer* c) {
  return c ? c->actual_num_planes : 1;
}

int v4l2_capturer_outstanding(struct v4l2_capturer* c) {
  return c ? atomic_load(&c->outstanding) : 0;
}
