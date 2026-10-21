/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

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
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct bg_buf {
    uint32_t handle;
    uint32_t fb_id;
    uint32_t pitch;
    size_t   size;
    void*    map;
};

struct plane_slot {
    uint32_t fb_id;
};

struct drm_renderer {
    int fd, crtc_id, width, height, buf_count, fb_idx;
    int mode_w, mode_h;
    drmModeConnector* conn;
    drmModeModeInfo    mode;
    drmModeCrtc*       saved_crtc;
    struct plane_slot* fbs;
    bool first_flip;
    atomic_int         flip_pending;
    drmEventContext    ev;

    display_layout layout;
    uint32_t       video_plane_id;
    bool           viewport_active;
    bool           compositor_active;
    bool           bg_crtc_set;
    struct bg_buf  bg;

    int            compositor_layers;
    int            layer_fb_idx[DISPLAY_LAYOUT_MAX_LAYERS];
    struct plane_slot* layer_fbs;
};

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec,
                              unsigned int usec, void* user_data) {
    (void)fd;
    (void)frame;
    (void)sec;
    (void)usec;
    struct drm_renderer* r = (struct drm_renderer*)user_data;
    atomic_store(&r->flip_pending, 0);
}

static void wait_flip_done(struct drm_renderer* r) {
    if (!atomic_load(&r->flip_pending)) return;

    struct pollfd pfd = {.fd = r->fd, .events = POLLIN};
    for (int i = 0; i < 100; i++) {
        drmHandleEvent(r->fd, &r->ev);
        if (!atomic_load(&r->flip_pending)) return;
        poll(&pfd, 1, 1);
    }
    fprintf(stderr, "drm: page flip wait timeout\n");
    atomic_store(&r->flip_pending, 0);
}

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

static int crtc_index_for_id(drmModeRes* res, uint32_t crtc_id) {
    for (int i = 0; i < res->count_crtcs; i++) {
        if (res->crtcs[i] == crtc_id)
            return i;
    }
    return -1;
}

static uint32_t find_nv12_overlay_plane(int fd, uint32_t crtc_id) {
    drmModeRes* res = drmModeGetResources(fd);
    if (!res)
        return 0;

    int crtc_idx = crtc_index_for_id(res, (uint32_t)crtc_id);
    if (crtc_idx < 0) {
        drmModeFreeResources(res);
        return 0;
    }

    drmModePlaneRes* pres = drmModeGetPlaneResources(fd);
    if (!pres) {
        drmModeFreeResources(res);
        return 0;
    }

    uint32_t fallback = 0;
    for (uint32_t i = 0; i < pres->count_planes; i++) {
        drmModePlane* p = drmModeGetPlane(fd, pres->planes[i]);
        if (!p)
            continue;
        if (!(p->possible_crtcs & (1u << crtc_idx))) {
            drmModeFreePlane(p);
            continue;
        }
        bool nv12 = false;
        for (uint32_t f = 0; f < p->count_formats; f++) {
            if (p->formats[f] == DRM_FORMAT_NV12) {
                nv12 = true;
                break;
            }
        }
        if (!nv12) {
            drmModeFreePlane(p);
            continue;
        }
        if (p->plane_id >= 79) {
            uint32_t id = p->plane_id;
            drmModeFreePlane(p);
            drmModeFreePlaneResources(pres);
            drmModeFreeResources(res);
            return id;
        }
        if (!fallback)
            fallback = p->plane_id;
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(pres);
    drmModeFreeResources(res);
    return fallback;
}

static int bg_create(struct drm_renderer* r) {
    struct drm_mode_create_dumb create = {
        .width  = (uint32_t)r->mode_w,
        .height = (uint32_t)r->mode_h,
        .bpp    = 32,
    };
    if (drmIoctl(r->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0)
        return -errno;

    r->bg.handle = create.handle;
    r->bg.pitch  = create.pitch;
    r->bg.size   = create.size;

    struct drm_mode_map_dumb map = {.handle = r->bg.handle};
    if (drmIoctl(r->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0)
        return -errno;

    r->bg.map = mmap(NULL, r->bg.size, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd,
                     (off_t)map.offset);
    if (r->bg.map == MAP_FAILED)
        return -errno;

    uint32_t* px = (uint32_t*)r->bg.map;
    for (int y = 0; y < r->mode_h; y++) {
        for (int x = 0; x < r->mode_w; x++)
            px[y * (r->bg.pitch / 4) + x] = 0;
    }

    struct drm_mode_fb_cmd fb = {
        .width  = (uint32_t)r->mode_w,
        .height = (uint32_t)r->mode_h,
        .pitch  = r->bg.pitch,
        .bpp    = 32,
        .depth  = 24,
        .handle = r->bg.handle,
    };
    if (drmIoctl(r->fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0)
        return -errno;
    r->bg.fb_id = fb.fb_id;
    return 0;
}

static void bg_destroy(struct drm_renderer* r) {
    if (r->bg.map && r->bg.map != MAP_FAILED)
        munmap(r->bg.map, r->bg.size);
    if (r->bg.fb_id)
        drmModeRmFB(r->fd, r->bg.fb_id);
    if (r->bg.handle) {
        struct drm_mode_destroy_dumb destroy = {.handle = r->bg.handle};
        drmIoctl(r->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
    memset(&r->bg, 0, sizeof(r->bg));
}

static int ensure_bg_crtc(struct drm_renderer* r) {
    if (r->bg_crtc_set)
        return 0;
    int ret = drmModeSetCrtc(r->fd, r->crtc_id, r->bg.fb_id, 0, 0,
                             &r->conn->connector_id, 1, &r->mode);
    if (ret < 0)
        return -errno;
    r->bg_crtc_set = true;
    return 0;
}

static int set_plane_rect(struct drm_renderer* r, uint32_t plane_id, uint32_t fb_id,
                          int dst_x, int dst_y, int dst_w, int dst_h, int src_w,
                          int src_h) {
    return drmModeSetPlane(r->fd, plane_id, (uint32_t)r->crtc_id, fb_id, 0,
                           (uint32_t)dst_x, (uint32_t)dst_y, (uint32_t)dst_w,
                           (uint32_t)dst_h, 0, 0, (uint32_t)src_w << 16,
                           (uint32_t)src_h << 16);
}

static int pick_connector_mode(struct drm_renderer* r, const drm_renderer_params* params) {
    int want_w = r->width > 0 ? r->width : 1920;
    int want_h = r->height > 0 ? r->height : 1080;
    int want_v = 60;
    if (params->mode_str && params->mode_str[0]) {
        int pw = 0, ph = 0, pv = 0;
        if (sscanf(params->mode_str, "%dx%d@%d", &pw, &ph, &pv) >= 2) {
            if (pw > 0) want_w = pw;
            if (ph > 0) want_h = ph;
            if (pv > 0) want_v = pv;
        }
    }

    drmModeModeInfo* m = NULL;
    int best_vdiff = 100000;
    for (int i = 0; i < r->conn->count_modes; i++) {
        drmModeModeInfo* cand = &r->conn->modes[i];
        if ((int)cand->hdisplay != want_w || (int)cand->vdisplay != want_h)
            continue;
        int vd = (int)cand->vrefresh - want_v;
        if (vd < 0) vd = -vd;
        if (vd == 0) {
            m = cand;
            break;
        }
        if (vd < best_vdiff) {
            best_vdiff = vd;
            m = cand;
        }
    }
    if (!m && r->conn->count_modes > 0) {
        fprintf(stderr, "drm: no connector mode %dx%d@%d, using %dx%d@%d\n",
                want_w, want_h, want_v, r->conn->modes[0].hdisplay,
                r->conn->modes[0].vdisplay, r->conn->modes[0].vrefresh);
        m = &r->conn->modes[0];
    } else if (m && best_vdiff > 0) {
        fprintf(stderr, "drm: no exact %dx%d@%d, using %dx%d@%d\n",
                want_w, want_h, want_v, m->hdisplay, m->vdisplay, m->vrefresh);
    }
    if (!m)
        return -1;
    memcpy(&r->mode, m, sizeof(r->mode));
    r->mode_w = (int)r->mode.hdisplay;
    r->mode_h = (int)r->mode.vdisplay;
    return 0;
}

static int compositor_setup(struct drm_renderer* r) {
    if (bg_create(r) < 0)
        return -1;
    r->compositor_active = true;
    r->compositor_layers = r->layout.layer_count;
    r->layer_fbs = calloc((size_t)r->compositor_layers * (size_t)r->buf_count,
                          sizeof(*r->layer_fbs));
    if (!r->layer_fbs)
        return -ENOMEM;
    for (int i = 0; i < r->compositor_layers; i++)
        r->layer_fb_idx[i] = 0;
    return 0;
}

struct drm_renderer* drm_renderer_create(const drm_renderer_params* params) {
    if (!params || !params->device)
        return NULL;

    struct drm_renderer* r = calloc(1, sizeof(*r));
    if (!r) return NULL;

    r->width     = params->content_width;
    r->height    = params->content_height;
    r->buf_count = params->buf_count > 0 ? params->buf_count : 5;
    r->first_flip = true;

    r->fd = open(params->device, O_RDWR | O_CLOEXEC);
    if (r->fd < 0) {
        perror("drm open");
        free(r);
        return NULL;
    }

    if (drmSetClientCap(r->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0) {
        fprintf(stderr, "drm: UNIVERSAL_PLANES cap failed: %s\n", strerror(errno));
        close(r->fd);
        free(r);
        return NULL;
    }

    r->conn = find_conn(r->fd, params->connector);
    if (!r->conn) {
        fprintf(stderr, "drm: connector %s not found\n",
                params->connector ? params->connector : "");
        close(r->fd);
        free(r);
        return NULL;
    }

    drmModeEncoder* enc = drmModeGetEncoder(r->fd, r->conn->encoders[0]);
    r->crtc_id = enc ? (int)enc->crtc_id : -1;
    if (enc) drmModeFreeEncoder(enc);

    if (pick_connector_mode(r, params) < 0) {
        drmModeFreeConnector(r->conn);
        close(r->fd);
        free(r);
        return NULL;
    }

    mp4_dist_display_viewport vp_def;
    mp4_dist_viewport_defaults(&vp_def);

    if (params->compositor_layout) {
        r->layout = *params->compositor_layout;
        if (r->layout.compositor) {
            if (compositor_setup(r) < 0) {
                fprintf(stderr, "drm: compositor setup failed\n");
                bg_destroy(r);
                free(r->layer_fbs);
                drmModeFreeConnector(r->conn);
                close(r->fd);
                free(r);
                return NULL;
            }
            printf("drm: compositor %d layer(s) mode=%dx%d@%d\n",
                   r->layout.layer_count, r->mode_w, r->mode_h, r->mode.vrefresh);
            for (int i = 0; i < r->layout.layer_count; i++) {
                const display_layer* lay = &r->layout.layers[i];
                printf("drm:   lane %d plane=%u %s %dx%d@(%d,%d)\n", lay->lane,
                       lay->plane_id, lay->main ? "main" : "sub", lay->dst_w,
                       lay->dst_h, lay->dst_x, lay->dst_y);
            }
        } else {
            r->layout = display_layout_resolve(&vp_def, r->mode_w, r->mode_h);
        }
    } else {
        const mp4_dist_display_viewport* vp =
            params->viewport ? params->viewport : &vp_def;
        r->layout = display_layout_resolve(vp, r->mode_w, r->mode_h);
        if (!r->layout.fullscreen) {
            r->video_plane_id = find_nv12_overlay_plane(r->fd, (uint32_t)r->crtc_id);
            if (!r->video_plane_id) {
                fprintf(stderr,
                        "drm: no NV12 overlay plane — viewport disabled, fullscreen\n");
                r->layout = display_layout_resolve(&vp_def, r->mode_w, r->mode_h);
            } else {
                if (bg_create(r) < 0) {
                    fprintf(stderr, "drm: background buffer failed — fullscreen\n");
                    r->layout = display_layout_resolve(&vp_def, r->mode_w, r->mode_h);
                    bg_destroy(r);
                } else {
                    r->viewport_active = true;
                    r->compositor_layers = 1;
                }
            }
        }
    }

    r->saved_crtc = drmModeGetCrtc(r->fd, r->crtc_id);
    r->fbs = calloc((size_t)r->buf_count, sizeof(*r->fbs));

    r->ev.version = DRM_EVENT_CONTEXT_VERSION;
    r->ev.page_flip_handler = page_flip_handler;
    atomic_init(&r->flip_pending, 0);

    if (!r->compositor_active && !r->viewport_active) {
        printf("drm: ready CRTC=%d %dx%d@%d fullscreen (page-flip)\n", r->crtc_id,
               r->mode_w, r->mode_h, r->mode.vrefresh);
    } else if (r->viewport_active && !params->compositor_layout) {
        const display_layer* lay = &r->layout.layers[0];
        printf("drm: viewport %dx%d@(%d,%d) plane=%u mode=%dx%d@%d\n", lay->dst_w,
               lay->dst_h, lay->dst_x, lay->dst_y, r->video_plane_id, r->mode_w,
               r->mode_h, r->mode.vrefresh);
    }
    return r;
}

const display_layout* drm_renderer_layout(const struct drm_renderer* r) {
    return r ? &r->layout : NULL;
}

void drm_renderer_free(struct drm_renderer* r) {
    if (!r) return;

    wait_flip_done(r);

    if (r->compositor_active || r->viewport_active) {
        for (int i = 0; i < r->compositor_layers; i++) {
            uint32_t pid = r->layout.layers[i].plane_id;
            if (!pid && r->viewport_active)
                pid = r->video_plane_id;
            if (pid)
                drmModeSetPlane(r->fd, pid, (uint32_t)r->crtc_id, 0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0);
        }
    }

    if (r->saved_crtc) {
        drmModeSetCrtc(r->fd, r->saved_crtc->crtc_id, r->saved_crtc->buffer_id,
                       r->saved_crtc->x, r->saved_crtc->y, &r->conn->connector_id, 1,
                       &r->saved_crtc->mode);
        drmModeFreeCrtc(r->saved_crtc);
    }
    if (r->layer_fbs) {
        for (int i = 0; i < r->compositor_layers * r->buf_count; i++) {
            if (r->layer_fbs[i].fb_id)
                drmModeRmFB(r->fd, r->layer_fbs[i].fb_id);
        }
        free(r->layer_fbs);
    }
    for (int i = 0; i < r->buf_count; i++) {
        if (r->fbs[i].fb_id) drmModeRmFB(r->fd, r->fbs[i].fb_id);
    }
    free(r->fbs);
    bg_destroy(r);
    drmModeFreeConnector(r->conn);
    if (r->fd >= 0) close(r->fd);
    free(r);
}

static int import_nv12_fb(struct drm_renderer* r, int dma_fd, uint32_t hor_stride,
                          uint32_t ver_stride, int content_w, int content_h,
                          uint32_t* fb_id_out) {
    uint32_t v_stride = ver_stride ? ver_stride : (uint32_t)content_h;

    uint32_t gem = 0;
    if (drmPrimeFDToHandle(r->fd, dma_fd, &gem) < 0)
        return -errno;

    struct drm_mode_fb_cmd2 fb = {0};
    fb.width        = (uint32_t)content_w;
    fb.height       = (uint32_t)content_h;
    fb.pixel_format = DRM_FORMAT_NV12;
    uint32_t uv_offset = hor_stride * v_stride;
    fb.handles[0]   = gem;
    fb.pitches[0]   = hor_stride;
    fb.offsets[0]   = 0;
    fb.handles[1]   = gem;
    fb.pitches[1]   = hor_stride;
    fb.offsets[1]   = uv_offset;

    if (drmIoctl(r->fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0)
        return -errno;
    *fb_id_out = fb.fb_id;
    return 0;
}

static int drm_renderer_display_fullscreen(struct drm_renderer* r, uint32_t fb_id) {
    int ret;
    if (r->first_flip) {
        ret = drmModeSetCrtc(r->fd, r->crtc_id, fb_id, 0, 0, &r->conn->connector_id,
                             1, &r->mode);
        if (ret < 0) {
            perror("SetCrtc");
            return -errno;
        }
        r->first_flip = false;
    } else {
        wait_flip_done(r);
        ret = drmModePageFlip(r->fd, r->crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, r);
        if (ret < 0) {
            ret = drmModeSetCrtc(r->fd, r->crtc_id, fb_id, 0, 0,
                                 &r->conn->connector_id, 1, &r->mode);
            if (ret < 0) {
                perror("PageFlip/SetCrtc");
                return -errno;
            }
        } else {
            atomic_store(&r->flip_pending, 1);
        }
    }
    return 0;
}

int drm_renderer_display_layers(struct drm_renderer* r,
                                const drm_layer_frame* frames, int frame_count) {
    if (!r || !r->compositor_active || !frames || frame_count <= 0)
        return -EINVAL;

    if (ensure_bg_crtc(r) < 0) {
        perror("SetCrtc bg");
        return -errno;
    }

    for (int f = 0; f < frame_count; f++) {
        const drm_layer_frame* fr = &frames[f];
        if (fr->layer_idx < 0 || fr->layer_idx >= r->compositor_layers)
            continue;
        if (fr->dma_fd < 0)
            continue;

        int li = fr->layer_idx;
        const display_layer* lay = &r->layout.layers[li];
        uint32_t plane_id = lay->plane_id;
        if (!plane_id)
            continue;

        uint32_t fb_id = 0;
        int cw = fr->content_w > 0 ? fr->content_w : (int)lay->dst_w;
        int ch = fr->content_h > 0 ? fr->content_h : (int)lay->dst_h;
        if (import_nv12_fb(r, fr->dma_fd, fr->hor_stride, fr->ver_stride, cw, ch,
                           &fb_id) < 0) {
            perror("ADDFB2 layer");
            continue;
        }

        if (set_plane_rect(r, plane_id, fb_id, lay->dst_x, lay->dst_y, lay->dst_w,
                           lay->dst_h, cw, ch) < 0) {
            perror("SetPlane layer");
            drmModeRmFB(r->fd, fb_id);
            continue;
        }

        int recycle = r->layer_fb_idx[li];
        struct plane_slot* slots =
            r->layer_fbs + (size_t)li * (size_t)r->buf_count;
        if (slots[recycle].fb_id)
            drmModeRmFB(r->fd, slots[recycle].fb_id);
        slots[recycle].fb_id = fb_id;
        r->layer_fb_idx[li] = (recycle + 1) % r->buf_count;
    }
    r->first_flip = false;
    return 0;
}

int drm_renderer_display(struct drm_renderer* r, int dma_fd, uint32_t hor_stride,
                         uint32_t ver_stride) {
    if (!r || dma_fd < 0) return -EINVAL;

    if (r->compositor_active && r->layout.layer_count == 1 && !r->viewport_active) {
        drm_layer_frame fr = {
            .layer_idx   = 0,
            .dma_fd      = dma_fd,
            .hor_stride  = hor_stride,
            .ver_stride  = ver_stride,
            .content_w   = r->width,
            .content_h   = r->height,
        };
        return drm_renderer_display_layers(r, &fr, 1);
    }

    uint32_t fb_id = 0;
    if (import_nv12_fb(r, dma_fd, hor_stride, ver_stride, r->width, r->height,
                       &fb_id) < 0) {
        perror("ADDFB2");
        return -errno;
    }

    int ret;
    if (r->viewport_active) {
        if (ensure_bg_crtc(r) < 0) {
            drmModeRmFB(r->fd, fb_id);
            return -errno;
        }
        const display_layer* lay = &r->layout.layers[0];
        if (set_plane_rect(r, r->video_plane_id, fb_id, lay->dst_x, lay->dst_y,
                           lay->dst_w, lay->dst_h, r->width, r->height) < 0) {
            perror("SetPlane");
            drmModeRmFB(r->fd, fb_id);
            return -errno;
        }
        ret = 0;
        r->first_flip = false;
    } else {
        ret = drm_renderer_display_fullscreen(r, fb_id);
    }

    if (ret < 0) {
        drmModeRmFB(r->fd, fb_id);
        return ret;
    }

    int recycle_idx = r->fb_idx;
    if (r->fbs[recycle_idx].fb_id) drmModeRmFB(r->fd, r->fbs[recycle_idx].fb_id);
    r->fbs[r->fb_idx].fb_id = fb_id;
    r->fb_idx = (r->fb_idx + 1) % r->buf_count;
    return 0;
}
