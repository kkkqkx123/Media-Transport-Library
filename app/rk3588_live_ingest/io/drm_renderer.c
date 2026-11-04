#include "drm_renderer.h"
#include <drm.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct drm_renderer {
  int fd, crtc_id, width, height, buf_count, fb_idx;
  drmModeConnector* conn;
  drmModeModeInfo    mode;
  drmModeCrtc*       saved_crtc;
  struct { uint32_t fb_id; }* fbs;
  bool first_flip;
  atomic_int         flip_pending;
  drmEventContext    ev;
  /* observability (decode thread writes, supervisor polls) */
  uint64_t stat_displays;
  uint64_t stat_display_ema_us;
  uint64_t stat_flip_wait_ema_us;
  uint64_t stat_flip_timeouts;
  int      stat_flip_pending_peak;
  uint64_t stat_addfb2_fails;
  uint64_t stat_flip_fails;
  uint64_t stat_bad_args;
  int      stat_mode_fps;
  uint32_t poll_fps;
  uint64_t poll_last_ns;
  uint64_t poll_last_displays;
};

static inline uint64_t drm_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec,
                              unsigned int usec, void* user_data) {
  (void)fd;
  (void)frame;
  (void)sec;
  (void)usec;
  struct drm_renderer* r = (struct drm_renderer*)user_data;
  atomic_store(&r->flip_pending, 0);
}

/* Close a GEM handle imported via drmPrimeFDToHandle. ADDFB2 takes its own
 * reference on the underlying object, so closing right after ADDFB2 neither
 * affects the displayed frame nor the stored fb_id. Best-effort: a failed
 * close is harmless (handle dies with the fd) and must not fail display. */
static void gem_close(int fd, uint32_t handle) {
  if (!handle) return;
  struct drm_gem_close c;
  memset(&c, 0, sizeof(c));
  c.handle = handle;
  drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &c);
}

/* Returns wait time in us; increments the flip timeout counter on expiry. */
static uint64_t wait_flip_done(struct drm_renderer* r) {
  uint64_t t0 = drm_now_ns();
  if (!atomic_load(&r->flip_pending)) return 0;

  struct pollfd pfd = {.fd = r->fd, .events = POLLIN};
  for (int i = 0; i < 100; i++) {
    drmHandleEvent(r->fd, &r->ev);
    if (!atomic_load(&r->flip_pending)) break;
    poll(&pfd, 1, 1);
  }
  uint64_t wait_us = (drm_now_ns() - t0) / 1000;
  if (atomic_load(&r->flip_pending)) {
    fprintf(stderr, "drm: page flip wait timeout\n");
    atomic_store(&r->flip_pending, 0);
    r->stat_flip_timeouts++;
  }
  return wait_us;
}

/* lookup connector by partial name match */
static drmModeConnector* find_conn(int fd, const char* name) {
  drmModeRes* res = drmModeGetResources(fd);
  if (!res) return NULL;
  for (int i = 0; i < res->count_connectors; i++) {
    drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
    if (!c) continue;
    if (c->connection == DRM_MODE_CONNECTED) {
      char cname[32];
      snprintf(cname, sizeof(cname), "HDMI-A-%u", c->connector_type_id);
      if (strstr(cname, name) || strcmp(cname, name) == 0) {
        printf("drm: connector %s (id=%d)\n", cname, c->connector_id);
        drmModeFreeResources(res);
        return c;
      }
    }
    drmModeFreeConnector(c);
  }
  drmModeFreeResources(res);
  return NULL;
}

struct drm_renderer* drm_renderer_create(const char* device,
                                          const char* connector,
                                          const char* mode_str,
                                          int width, int height,
                                          uint32_t fourcc, int buf_count) {
  (void)fourcc;
  struct drm_renderer* r = calloc(1, sizeof(*r));
  if (!r) return NULL;
  r->width = width;
  r->height = height;
  r->buf_count = buf_count;
  r->first_flip = true;

  r->fd = open(device, O_RDWR | O_CLOEXEC);
  if (r->fd < 0) {
    perror("drm open");
    free(r);
    return NULL;
  }

  r->conn = find_conn(r->fd, connector);
  if (!r->conn) {
    fprintf(stderr, "drm: connector %s not found\n", connector);
    close(r->fd);
    free(r);
    return NULL;
  }

  drmModeEncoder* enc = drmModeGetEncoder(r->fd, r->conn->encoders[0]);
  r->crtc_id = enc ? (int)enc->crtc_id : -1;
  if (enc) drmModeFreeEncoder(enc);

  int w = 0, h = 0, v = 60;
  sscanf(mode_str, "%dx%d@%d", &w, &h, &v);
  drmModeModeInfo* m = NULL;
  for (int i = 0; i < r->conn->count_modes; i++)
    if (r->conn->modes[i].hdisplay == w && r->conn->modes[i].vdisplay == h &&
        (int)r->conn->modes[i].vrefresh == v) {
      m = &r->conn->modes[i];
      break;
    }
  if (!m) m = &r->conn->modes[0];
  memcpy(&r->mode, m, sizeof(r->mode));

  r->saved_crtc = drmModeGetCrtc(r->fd, r->crtc_id);
  r->fbs = calloc((size_t)buf_count, sizeof(*r->fbs));

  r->ev.version = DRM_EVENT_CONTEXT_VERSION;
  r->ev.page_flip_handler = page_flip_handler;
  atomic_init(&r->flip_pending, 0);
  r->stat_mode_fps = (int)m->vrefresh ? (int)m->vrefresh : v;

  printf("drm: ready CRTC=%d %dx%d@%d (page-flip event)\n", r->crtc_id,
         r->mode.hdisplay, r->mode.vdisplay, r->mode.vrefresh);
  return r;
}

void drm_renderer_free(struct drm_renderer* r) {
  if (!r) return;

  wait_flip_done(r);

  if (r->saved_crtc) {
    drmModeSetCrtc(r->fd, r->saved_crtc->crtc_id, r->saved_crtc->buffer_id,
                   r->saved_crtc->x, r->saved_crtc->y, &r->conn->connector_id, 1,
                   &r->saved_crtc->mode);
    drmModeFreeCrtc(r->saved_crtc);
  }
  for (int i = 0; i < r->buf_count; i++) {
    if (r->fbs[i].fb_id) drmModeRmFB(r->fd, r->fbs[i].fb_id);
  }
  free(r->fbs);
  drmModeFreeConnector(r->conn);
  if (r->fd >= 0) close(r->fd);
  free(r);
}

int drm_renderer_display(struct drm_renderer* r, int dma_fd, uint32_t hor_stride,
                         uint32_t ver_stride) {
  if (!r || dma_fd < 0) {
    if (r) r->stat_bad_args++;
    return -EINVAL;
  }
  uint64_t t0 = drm_now_ns();
  int pending = atomic_load(&r->flip_pending);
  if (pending > r->stat_flip_pending_peak)
    r->stat_flip_pending_peak = pending;

  uint32_t v_stride = ver_stride ? ver_stride : (uint32_t)r->height;

  uint32_t gem = 0;
  if (drmPrimeFDToHandle(r->fd, dma_fd, &gem) < 0) return -errno;

  struct drm_mode_fb_cmd2 fb = {0};
  fb.width = r->width;
  fb.height = r->height;
  fb.pixel_format = DRM_FORMAT_NV12;
  /* MPP NV12: UV at hor_stride * ver_stride (ver_stride often ALIGN(h,16)).
   * RGA NV12: tightly packed, pass ver_stride=0 to use display height. */
  uint32_t uv_offset = hor_stride * v_stride;
  fb.handles[0] = gem;
  fb.pitches[0] = hor_stride;
  fb.offsets[0] = 0;
  fb.handles[1] = gem;
  fb.pitches[1] = hor_stride;
  fb.offsets[1] = uv_offset;

  if (drmIoctl(r->fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0) {
    perror("ADDFB2");
    gem_close(r->fd, gem);
    r->stat_addfb2_fails++;
    return -errno;
  }
  /* ADDFB2 pins its own reference; drop our import handle now so a long
   * stable run does not leak one GEM handle per frame. */
  gem_close(r->fd, gem);

  int ret;
  uint64_t wait_us = 0;
  if (r->first_flip) {
    ret = drmModeSetCrtc(r->fd, r->crtc_id, fb.fb_id, 0, 0,
                         &r->conn->connector_id, 1, &r->mode);
    if (ret < 0) {
      perror("SetCrtc");
      drmModeRmFB(r->fd, fb.fb_id);
      r->stat_flip_fails++;
      return -errno;
    }
    r->first_flip = false;
  } else {
    wait_us = wait_flip_done(r);
    ret = drmModePageFlip(r->fd, r->crtc_id, fb.fb_id, DRM_MODE_PAGE_FLIP_EVENT, r);
    if (ret < 0) {
      ret = drmModeSetCrtc(r->fd, r->crtc_id, fb.fb_id, 0, 0,
                           &r->conn->connector_id, 1, &r->mode);
      if (ret < 0) {
        perror("PageFlip/SetCrtc");
        drmModeRmFB(r->fd, fb.fb_id);
        r->stat_flip_fails++;
        return -errno;
      }
    } else {
      atomic_store(&r->flip_pending, 1);
    }
  }

  /* Recycle fb slot from buf_count frames ago; flip event ensures scanout done. */
  int recycle_idx = r->fb_idx;
  if (r->fbs[recycle_idx].fb_id) drmModeRmFB(r->fd, r->fbs[recycle_idx].fb_id);
  r->fbs[r->fb_idx].fb_id = fb.fb_id;
  r->fb_idx = (r->fb_idx + 1) % r->buf_count;
  {
    uint64_t cost_us = (drm_now_ns() - t0) / 1000;
    if (r->stat_display_ema_us == 0)
      r->stat_display_ema_us = cost_us;
    else
      r->stat_display_ema_us += (cost_us - r->stat_display_ema_us) / 8;
    if (r->stat_flip_wait_ema_us == 0)
      r->stat_flip_wait_ema_us = wait_us;
    else
      r->stat_flip_wait_ema_us += (wait_us - r->stat_flip_wait_ema_us) / 8;
    r->stat_displays++;
  }
  return 0;
}

void drm_renderer_stats(struct drm_renderer* r, struct drm_renderer_stats* out) {
  if (!out) return;
  if (!r) { memset(out, 0, sizeof(*out)); return; }
  uint64_t now = drm_now_ns();
  uint32_t fps = 0;
  if (r->poll_last_ns != 0 && now > r->poll_last_ns) {
    uint64_t dt = now - r->poll_last_ns;
    uint64_t dn = r->stat_displays - r->poll_last_displays;
    if (dt > 0) fps = (uint32_t)((dn * 1000000000ULL) / dt);
  }
  r->poll_last_ns = now;
  r->poll_last_displays = r->stat_displays;
  r->poll_fps = fps;
  out->displays = r->stat_displays;
  out->display_fps = r->poll_fps;
  out->display_ema_us = r->stat_display_ema_us;
  out->flip_wait_ema_us = r->stat_flip_wait_ema_us;
  out->flip_timeouts = r->stat_flip_timeouts;
  out->flip_pending_peak = r->stat_flip_pending_peak;
  out->addfb2_fails = r->stat_addfb2_fails;
  out->flip_fails = r->stat_flip_fails;
  out->bad_args = r->stat_bad_args;
  out->mode_fps = r->stat_mode_fps;
}
