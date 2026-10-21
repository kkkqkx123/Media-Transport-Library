/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 *
 * V4L2 HDMI capture using Multiplanar API (required by RK3588 rk_hdmirx).
 * Captures NV12 frames and exports dma-buf fds for zero-copy to MPP.
 */

#include "v4l2_capturer.h"
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
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

struct v4l2_buf_info {
  int   dma_fd[MAX_PLANES];  /* exported dma-buf fd per plane */
  void* v4l2_buf[MAX_PLANES]; /* mmap'd pointers */
  int   num_planes;
  uint32_t bytesused[MAX_PLANES];
  uint32_t length[MAX_PLANES];
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
  void*          user_data;

  atomic_bool running;
  pthread_t   thread;
};

/* ---- helpers ---- */

static uint64_t gettime_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int xioctl(int fd, unsigned long req, void* arg) {
  int r;
  do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
  return r;
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
  atomic_init(&c->running, false);

  /* open device */
  c->fd = open(device, O_RDWR);
  if (c->fd < 0) {
    perror("v4l2_capturer: open device");
    free(c);
    return NULL;
  }

  /* set format: try NV12 first (2-plane multiplanar), fall back to BGR3 */
  struct v4l2_format fmt = {0};
  fmt.type                    = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  fmt.fmt.pix_mp.width        = width;
  fmt.fmt.pix_mp.height       = height;
  fmt.fmt.pix_mp.pixelformat  = V4L2_PIX_FMT_NV12;
  fmt.fmt.pix_mp.field        = V4L2_FIELD_NONE;
  fmt.fmt.pix_mp.num_planes   = 2; /* NV12 multiplanar: plane0=Y, plane1=UV */
  if (xioctl(c->fd, VIDIOC_S_FMT, &fmt) < 0) {
    /* retry with BGR3 (default for rk_hdmirx) */
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_BGR24;
    fmt.fmt.pix_mp.num_planes  = 1;
    if (xioctl(c->fd, VIDIOC_S_FMT, &fmt) < 0) {
      perror("v4l2_capturer: VIDIOC_S_FMT failed for both NV12 and BGR3");
      close(c->fd); free(c); return NULL;
    }
  }
  c->actual_fourcc     = fmt.fmt.pix_mp.pixelformat;
  c->actual_num_planes = fmt.fmt.pix_mp.num_planes;
  c->plane_stride      = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
  printf("v4l2_capturer: fmt=%dx%d fourcc=0x%x ('%.4s') planes=%d stride=%u\n",
         fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
         fmt.fmt.pix_mp.pixelformat, (char*)&fmt.fmt.pix_mp.pixelformat,
         fmt.fmt.pix_mp.num_planes,
         fmt.fmt.pix_mp.plane_fmt[0].bytesperline);

  /* set framerate */
  struct v4l2_streamparm parm = {0};
  parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  parm.parm.capture.timeperframe.numerator   = 1;
  parm.parm.capture.timeperframe.denominator = (int)fps;
  if (xioctl(c->fd, VIDIOC_S_PARM, &parm) < 0)
    fprintf(stderr, "v4l2_capturer: VIDIOC_S_PARM failed (continuing)\n");

  /* request MMAP buffers */
  struct v4l2_requestbuffers req = {0};
  req.count  = buf_count;
  req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(c->fd, VIDIOC_REQBUFS, &req) < 0) {
    perror("v4l2_capturer: VIDIOC_REQBUFS");
    close(c->fd); free(c); return NULL;
  }
  c->buf_count = (int)req.count;
  printf("v4l2_capturer: allocated %d buffers\n", c->buf_count);

  /* mmap and export */
  c->bufs = calloc((size_t)c->buf_count, sizeof(struct v4l2_buf_info));
  for (int i = 0; i < c->buf_count; i++) {
    struct v4l2_buffer buf = {0};
    struct v4l2_plane planes[MAX_PLANES] = {0};
    buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory   = V4L2_MEMORY_MMAP;
    buf.index    = i;
    buf.m.planes = planes;
    buf.length   = MAX_PLANES;
    if (xioctl(c->fd, VIDIOC_QUERYBUF, &buf) < 0) {
      perror("v4l2_capturer: VIDIOC_QUERYBUF");
      goto fail;
    }
    c->bufs[i].num_planes = (buf.length < MAX_PLANES) ? buf.length : MAX_PLANES;

    for (int p = 0; p < c->bufs[i].num_planes; p++) {
      c->bufs[i].bytesused[p] = planes[p].length;  /* actual used bytes set at DQBUF */
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
      exp.index = i;
      exp.plane = p;
      exp.flags = O_CLOEXEC;
      if (xioctl(c->fd, VIDIOC_EXPBUF, &exp) < 0) {
        perror("v4l2_capturer: VIDIOC_EXPBUF");
        goto fail;
      }
      c->bufs[i].dma_fd[p] = exp.fd;
    }
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
    buf.index    = i;
    buf.m.planes = planes;
    buf.length   = c->bufs[i].num_planes;
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

/* ---- capture thread ---- */

static void* capture_thread(void* arg) {
  struct v4l2_capturer* c = (struct v4l2_capturer*)arg;

  /* start streaming */
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (xioctl(c->fd, VIDIOC_STREAMON, &type) < 0) {
    perror("v4l2_capturer: VIDIOC_STREAMON");
    return NULL;
  }
  printf("v4l2_capturer: streaming @ %dx%d %.1ffps\n",
         c->width, c->height, (double)c->fps);

  uint64_t frame_seq = 0;
  while (atomic_load(&c->running)) {
    struct v4l2_buffer buf = {0};
    struct v4l2_plane planes[MAX_PLANES] = {0};
    buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory   = V4L2_MEMORY_MMAP;
    buf.m.planes = planes;
    buf.length   = MAX_PLANES;

    if (xioctl(c->fd, VIDIOC_DQBUF, &buf) < 0) {
      if (errno == EINTR) continue;
      perror("v4l2_capturer: VIDIOC_DQBUF");
      break;
    }

    uint64_t ts = gettime_ns();
    struct v4l2_buf_info* bi = &c->bufs[buf.index];
    uint32_t total_size = 0;
    for (int p = 0; p < bi->num_planes; p++)
      total_size += planes[p].bytesused;
    int fd_uv = (bi->num_planes >= 2) ? bi->dma_fd[1] : -1;
    c->cb(c->user_data, bi->dma_fd[0], fd_uv,
          c->plane_stride, total_size, ts, frame_seq);
    frame_seq++;

    /* re-queue */
    if (xioctl(c->fd, VIDIOC_QBUF, &buf) < 0) {
      perror("v4l2_capturer: VIDIOC_QBUF");
      break;
    }
  }

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
