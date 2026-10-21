/* SPDX-License-Identifier: BSD-3-Clause
 *
 * test_drm_plane — verify RK3588 VOP overlay plane scaling/positioning (Phase A PiP).
 *
 * Draws a black fullscreen primary CRTC + NV12 test pattern on an overlay plane
 * scaled into a configurable rectangle. Optionally animates the rectangle.
 *
 * Usage:
 *   ./test_drm_plane [options]
 *   ./test_drm_plane --list-planes-only
 *
 * Requires DRM master (no Xorg/Weston on the same card). Run from a VT or stop the
 * display manager first.
 */

#include <drm.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct test_cfg {
    const char* device;
    const char* connector;
    const char* mode_str;
    int         duration_sec;
    int         pip_x;
    int         pip_y;
    int         pip_w;
    int         pip_h;
    int         content_w;
    int         content_h;
    bool        list_planes_only;
    bool        no_animate;
    bool        verbose;
};

struct dumb_buf {
    uint32_t handle;
    uint32_t fb_id;
    uint32_t pitch;
    size_t   size;
    void*    map;
};

struct drm_plane_info {
    uint32_t plane_id;
    uint32_t crtc_id;
    uint32_t crtc_idx;
    uint32_t formats[64];
    uint32_t format_count;
    bool     nv12;
    bool     primary_candidate;
};

static const char* fourcc_name(uint32_t fmt) {
    static char name[5];
    name[0] = (char)(fmt & 0xff);
    name[1] = (char)((fmt >> 8) & 0xff);
    name[2] = (char)((fmt >> 16) & 0xff);
    name[3] = (char)((fmt >> 24) & 0xff);
    name[4] = '\0';
    return name;
}

static int drm_open_device(const char* path) {
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror(path);
        return -1;
    }
    /* Without this, libdrm only lists Cluster overlays — Esmart NV12 planes are hidden. */
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0) {
        fprintf(stderr, "drmSetClientCap(UNIVERSAL_PLANES): %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static drmModeConnector* find_connected_connector(int fd, const char* want) {
    drmModeRes* res = drmModeGetResources(fd);
    if (!res)
        return NULL;

    drmModeConnector* found = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c)
            continue;
        if (c->connection != DRM_MODE_CONNECTED) {
            drmModeFreeConnector(c);
            continue;
        }

        char name[32];
        snprintf(name, sizeof(name), "HDMI-A-%u", c->connector_type_id);

        if (!want || !want[0] || strstr(name, want) || !strcmp(name, want)) {
            printf("connector: %s id=%u modes=%d\n", name, c->connector_id,
                   c->count_modes);
            found = c;
            break;
        }
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);
    return found;
}

static drmModeModeInfo* pick_mode(drmModeConnector* conn, const char* mode_str,
                                  int* out_w, int* out_h) {
    int want_w = 1920, want_h = 1080, want_v = 60;
    if (mode_str && mode_str[0]) {
        int pw = 0, ph = 0, pv = 0;
        if (sscanf(mode_str, "%dx%d@%d", &pw, &ph, &pv) >= 2) {
            if (pw > 0)
                want_w = pw;
            if (ph > 0)
                want_h = ph;
            if (pv > 0)
                want_v = pv;
        }
    }

    drmModeModeInfo* best = NULL;
    int best_vdiff = 100000;
    for (int i = 0; i < conn->count_modes; i++) {
        drmModeModeInfo* m = &conn->modes[i];
        if ((int)m->hdisplay != want_w || (int)m->vdisplay != want_h)
            continue;
        int vd = (int)m->vrefresh - want_v;
        if (vd < 0)
            vd = -vd;
        if (vd == 0) {
            best = m;
            break;
        }
        if (vd < best_vdiff) {
            best_vdiff = vd;
            best = m;
        }
    }
    if (!best && conn->count_modes > 0)
        best = &conn->modes[0];
    if (best) {
        *out_w = best->hdisplay;
        *out_h = best->vdisplay;
    }
    return best;
}

static int crtc_index_for_id(drmModeRes* res, uint32_t crtc_id) {
    for (int i = 0; i < res->count_crtcs; i++) {
        if (res->crtcs[i] == crtc_id)
            return i;
    }
    return -1;
}

static int enumerate_planes(int fd, uint32_t crtc_id, struct drm_plane_info* out,
                            int out_cap, bool verbose) {
    drmModeRes* res = drmModeGetResources(fd);
    if (!res)
        return -1;

    int crtc_idx = crtc_index_for_id(res, crtc_id);
    if (crtc_idx < 0) {
        drmModeFreeResources(res);
        return -1;
    }

    drmModePlaneRes* pres = drmModeGetPlaneResources(fd);
    if (!pres) {
        drmModeFreeResources(res);
        return -1;
    }

    int n = 0;
    for (uint32_t i = 0; i < pres->count_planes && n < out_cap; i++) {
        drmModePlane* p = drmModeGetPlane(fd, pres->planes[i]);
        if (!p)
            continue;

        if (!(p->possible_crtcs & (1u << crtc_idx))) {
            drmModeFreePlane(p);
            continue;
        }

        struct drm_plane_info* info = &out[n++];
        memset(info, 0, sizeof(*info));
        info->plane_id = p->plane_id;
        info->crtc_id = crtc_id;
        info->crtc_idx = (uint32_t)crtc_idx;
        info->format_count = p->count_formats;
        if (info->format_count > 64)
            info->format_count = 64;

        for (uint32_t f = 0; f < info->format_count; f++) {
            info->formats[f] = p->formats[f];
            if (p->formats[f] == DRM_FORMAT_NV12)
                info->nv12 = true;
        }

        if (verbose) {
            printf("  plane id=%u possible_crtcs=0x%x formats=",
                   p->plane_id, p->possible_crtcs);
            for (uint32_t f = 0; f < p->count_formats; f++)
                printf("%s%s", f ? "," : "", fourcc_name(p->formats[f]));
            printf("%s\n", info->nv12 ? " [NV12]" : "");
        }

        drmModeFreePlane(p);
    }

    drmModeFreePlaneResources(pres);
    drmModeFreeResources(res);
    return n;
}

static int find_nv12_overlay_plane(const struct drm_plane_info* planes, int count,
                                   uint32_t skip_plane_id) {
    /* Prefer Esmart1+ (typ. id 79+) so Esmart0 primary stays for SetCrtc background. */
    int fallback = -1;
    for (int i = 0; i < count; i++) {
        if (!planes[i].nv12)
            continue;
        if (skip_plane_id && planes[i].plane_id == skip_plane_id)
            continue;
        if (planes[i].plane_id >= 79)
            return i;
        if (fallback < 0)
            fallback = i;
    }
    if (fallback >= 0)
        return fallback;
    for (int i = 0; i < count; i++) {
        if (planes[i].nv12)
            return i;
    }
    return -1;
}

static int dumb_create(int fd, int width, int height, int bpp, bool nv12,
                       struct dumb_buf* buf) {
    memset(buf, 0, sizeof(*buf));

    uint32_t dumb_h = (uint32_t)height;
    if (nv12)
        dumb_h = (uint32_t)height + (uint32_t)height / 2;

    struct drm_mode_create_dumb create = {
        .width = (uint32_t)width,
        .height = dumb_h,
        .bpp = (uint32_t)bpp,
    };
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0)
        return -errno;

    buf->handle = create.handle;
    buf->pitch = create.pitch;
    buf->size = create.size;

    struct drm_mode_map_dumb map = {.handle = buf->handle};
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0)
        return -errno;

    buf->map = mmap(NULL, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                    (off_t)map.offset);
    if (buf->map == MAP_FAILED)
        return -errno;

    return 0;
}

static void dumb_destroy(int fd, struct dumb_buf* buf) {
    if (!buf)
        return;
    if (buf->map && buf->map != MAP_FAILED)
        munmap(buf->map, buf->size);
    if (buf->fb_id)
        drmModeRmFB(fd, buf->fb_id);
    if (buf->handle) {
        struct drm_mode_destroy_dumb destroy = {.handle = buf->handle};
        drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
    memset(buf, 0, sizeof(*buf));
}

static int dumb_addfb_xrgb(int fd, struct dumb_buf* buf, int width, int height) {
    struct drm_mode_fb_cmd fb = {
        .width = (uint32_t)width,
        .height = (uint32_t)height,
        .pitch = buf->pitch,
        .bpp = 32,
        .depth = 24,
        .handle = buf->handle,
    };
    if (drmIoctl(fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0)
        return -errno;
    buf->fb_id = fb.fb_id;
    return 0;
}

static int dumb_addfb_nv12(int fd, struct dumb_buf* buf, int width, int height) {
    struct drm_mode_fb_cmd2 fb = {
        .width = (uint32_t)width,
        .height = (uint32_t)height,
        .pixel_format = DRM_FORMAT_NV12,
        .handles[0] = buf->handle,
        .pitches[0] = buf->pitch,
        .offsets[0] = 0,
        .handles[1] = buf->handle,
        .pitches[1] = buf->pitch,
        .offsets[1] = buf->pitch * (uint32_t)height,
    };
    if (drmIoctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0)
        return -errno;
    buf->fb_id = fb.fb_id;
    return 0;
}

static void fill_black_xrgb(struct dumb_buf* buf, int width, int height) {
    uint32_t* px = (uint32_t*)buf->map;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            px[y * (buf->pitch / 4) + x] = 0x00000000;
    }
}

/* Simple horizontal color bars in NV12 (BT.601-ish). */
static void fill_nv12_bars(struct dumb_buf* buf, int width, int height, uint32_t pitch) {
    static const uint8_t y_vals[] = {235, 210, 170, 145, 106, 81, 41, 16};
    static const uint8_t u_vals[] = {128,  16, 166,  54, 202, 240,  92, 128};
    static const uint8_t v_vals[] = {128, 146,  16,  34, 222, 118, 240, 128};

    uint8_t* y = (uint8_t*)buf->map;
    uint8_t* uv = y + pitch * (uint32_t)height;
    int bars = (int)(sizeof(y_vals) / sizeof(y_vals[0]));
    int bar_w = width / bars;

    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col++) {
            int b = col / bar_w;
            if (b >= bars)
                b = bars - 1;
            y[row * pitch + col] = y_vals[b];
        }
    }
    for (int row = 0; row < height / 2; row++) {
        for (int col = 0; col < width; col += 2) {
            int b = col / bar_w;
            if (b >= bars)
                b = bars - 1;
            uv[row * pitch + col] = u_vals[b];
            uv[row * pitch + col + 1] = v_vals[b];
        }
    }
}

static int set_plane_scaled(int fd, uint32_t plane_id, uint32_t crtc_id, uint32_t fb_id,
                            int dst_x, int dst_y, int dst_w, int dst_h, int src_w,
                            int src_h) {
    return drmModeSetPlane(fd, plane_id, crtc_id, fb_id, 0, (uint32_t)dst_x,
                           (uint32_t)dst_y, (uint32_t)dst_w, (uint32_t)dst_h, 0, 0,
                           (uint32_t)src_w << 16, (uint32_t)src_h << 16);
}

static void usage(const char* prog) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --device PATH         DRM card (default /dev/dri/card0)\n"
            "  --connector NAME      HDMI-A-N substring (default HDMI-A)\n"
            "  --mode WxH@Hz         e.g. 1920x1080@60\n"
            "  --duration SEC        run time (default 10)\n"
            "  --pip-x --pip-y       overlay top-left (default 1420,770)\n"
            "  --pip-w --pip-h       overlay size (default 480,270)\n"
            "  --content-w --content-h  NV12 source size (default 1920,1080)\n"
            "  --list-planes-only    enumerate planes and exit\n"
            "  --no-animate          keep PiP rect fixed\n"
            "  --verbose             extra logging\n",
            prog);
}

static int parse_args(int argc, char** argv, struct test_cfg* cfg) {
    static struct option opts[] = {
        {"device", required_argument, NULL, 'd'},
        {"connector", required_argument, NULL, 'c'},
        {"mode", required_argument, NULL, 'm'},
        {"duration", required_argument, NULL, 't'},
        {"pip-x", required_argument, NULL, 'x'},
        {"pip-y", required_argument, NULL, 'y'},
        {"pip-w", required_argument, NULL, 'w'},
        {"pip-h", required_argument, NULL, 1002},
        {"content-w", required_argument, NULL, 'W'},
        {"content-h", required_argument, NULL, 'H'},
        {"list-planes-only", no_argument, NULL, 'l'},
        {"no-animate", no_argument, NULL, 'n'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, '?'},
        {NULL, 0, NULL, 0},
    };

    int c;
    while ((c = getopt_long(argc, argv, "d:c:m:t:x:y:w:W:H:lnv?", opts, NULL)) != -1) {
        switch (c) {
        case 'd':
            cfg->device = optarg;
            break;
        case 'c':
            cfg->connector = optarg;
            break;
        case 'm':
            cfg->mode_str = optarg;
            break;
        case 't':
            cfg->duration_sec = atoi(optarg);
            break;
        case 'x':
            cfg->pip_x = atoi(optarg);
            break;
        case 'y':
            cfg->pip_y = atoi(optarg);
            break;
        case 'w':
            cfg->pip_w = atoi(optarg);
            break;
        case 1002:
            cfg->pip_h = atoi(optarg);
            break;
        case 'W':
            cfg->content_w = atoi(optarg);
            break;
        case 'H':
            cfg->content_h = atoi(optarg);
            break;
        case 'l':
            cfg->list_planes_only = true;
            break;
        case 'n':
            cfg->no_animate = true;
            break;
        case 'v':
            cfg->verbose = true;
            break;
        case '?':
        default:
            usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    struct test_cfg cfg = {
        .device = "/dev/dri/card0",
        .connector = "HDMI-A",
        .mode_str = "1920x1080@60",
        .duration_sec = 10,
        .pip_x = 1420,
        .pip_y = 770,
        .pip_w = 480,
        .pip_h = 270,
        .content_w = 1920,
        .content_h = 1080,
    };

    if (parse_args(argc, argv, &cfg) < 0)
        return 1;

    int fd = drm_open_device(cfg.device);
    if (fd < 0)
        return 1;

    if (drmSetMaster(fd) < 0 && cfg.verbose)
        fprintf(stderr, "note: drmSetMaster: %s (may need root or no compositor)\n",
                strerror(errno));

    drmModeConnector* conn = find_connected_connector(fd, cfg.connector);
    if (!conn) {
        fprintf(stderr, "No connected connector matching '%s'\n", cfg.connector);
        close(fd);
        return 1;
    }

    if (conn->encoder_id == 0) {
        fprintf(stderr, "Connector has no encoder\n");
        drmModeFreeConnector(conn);
        close(fd);
        return 1;
    }

    drmModeEncoder* enc = drmModeGetEncoder(fd, conn->encoder_id);
    if (!enc) {
        drmModeFreeConnector(conn);
        close(fd);
        return 1;
    }
    uint32_t crtc_id = enc->crtc_id;
    drmModeFreeEncoder(enc);

    if (!crtc_id) {
        fprintf(stderr, "Encoder has no active CRTC\n");
        drmModeFreeConnector(conn);
        close(fd);
        return 1;
    }

    int mode_w = 0, mode_h = 0;
    drmModeModeInfo* mode = pick_mode(conn, cfg.mode_str, &mode_w, &mode_h);
    if (!mode) {
        fprintf(stderr, "No display mode available\n");
        drmModeFreeConnector(conn);
        close(fd);
        return 1;
    }

    printf("mode: %dx%d@%d crtc=%u\n", mode_w, mode_h, mode->vrefresh, crtc_id);

    struct drm_plane_info planes[32];
    printf("planes for crtc %u:\n", crtc_id);
    int plane_count =
        enumerate_planes(fd, crtc_id, planes, (int)(sizeof(planes) / sizeof(planes[0])),
                         true);
    if (plane_count <= 0) {
        fprintf(stderr, "FAIL: no planes for CRTC %u\n", crtc_id);
        drmModeFreeConnector(conn);
        close(fd);
        return 1;
    }

    int nv12_idx = find_nv12_overlay_plane(planes, plane_count, 0);
    if (nv12_idx < 0) {
        fprintf(stderr,
                "FAIL: no NV12-capable plane found — Phase A needs VOP overlay + NV12\n");
        drmModeFreeConnector(conn);
        close(fd);
        return 1;
    }

    printf("selected NV12 plane: id=%u\n", planes[nv12_idx].plane_id);

    if (cfg.list_planes_only) {
        printf("PASS: %d plane(s), NV12 overlay available\n", plane_count);
        drmModeFreeConnector(conn);
        close(fd);
        return 0;
    }

    /* Clamp PiP rect to mode. */
    if (cfg.pip_w <= 0)
        cfg.pip_w = mode_w / 4;
    if (cfg.pip_h <= 0)
        cfg.pip_h = mode_h / 4;
    if (cfg.pip_x + cfg.pip_w > mode_w)
        cfg.pip_x = mode_w - cfg.pip_w;
    if (cfg.pip_y + cfg.pip_h > mode_h)
        cfg.pip_y = mode_h - cfg.pip_h;
    if (cfg.pip_x < 0)
        cfg.pip_x = 0;
    if (cfg.pip_y < 0)
        cfg.pip_y = 0;

    drmModeCrtc* saved = drmModeGetCrtc(fd, crtc_id);

    struct dumb_buf bg = {0};
    struct dumb_buf video = {0};
    int ret = 0;

    if (dumb_create(fd, mode_w, mode_h, 32, false, &bg) < 0) {
        perror("create bg dumb");
        ret = 1;
        goto out;
    }
    fill_black_xrgb(&bg, mode_w, mode_h);
    if (dumb_addfb_xrgb(fd, &bg, mode_w, mode_h) < 0) {
        perror("ADDFB bg");
        ret = 1;
        goto out;
    }

    if (dumb_create(fd, cfg.content_w, cfg.content_h, 8, true, &video) < 0) {
        perror("create video dumb");
        ret = 1;
        goto out;
    }
    fill_nv12_bars(&video, cfg.content_w, cfg.content_h, video.pitch);
    if (dumb_addfb_nv12(fd, &video, cfg.content_w, cfg.content_h) < 0) {
        perror("ADDFB2 NV12");
        ret = 1;
        goto out;
    }

    ret = drmModeSetCrtc(fd, crtc_id, bg.fb_id, 0, 0, &conn->connector_id, 1, mode);
    if (ret < 0) {
        perror("SetCrtc (black background)");
        ret = 1;
        goto out;
    }
    printf("SetCrtc: black %dx%d background ok\n", mode_w, mode_h);

    int dst_x = cfg.pip_x, dst_y = cfg.pip_y;
    ret = set_plane_scaled(fd, planes[nv12_idx].plane_id, crtc_id, video.fb_id, dst_x,
                           dst_y, cfg.pip_w, cfg.pip_h, cfg.content_w, cfg.content_h);
    if (ret < 0) {
        fprintf(stderr, "FAIL: SetPlane NV12 overlay: %s\n", strerror(errno));
        ret = 1;
        goto out;
    }
    printf("SetPlane: NV12 %dx%d -> dst (%d,%d %dx%d) ok\n", cfg.content_w,
           cfg.content_h, dst_x, dst_y, cfg.pip_w, cfg.pip_h);

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int frame = 0;

    while (1) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed =
            (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed >= cfg.duration_sec)
            break;

        if (!cfg.no_animate && (frame % 60) == 0 && frame > 0) {
            int step = (frame / 60) % 4;
            switch (step) {
            case 0:
                dst_x = cfg.pip_x;
                dst_y = cfg.pip_y;
                break;
            case 1:
                dst_x = mode_w - cfg.pip_w - cfg.pip_x;
                dst_y = cfg.pip_y;
                break;
            case 2:
                dst_x = mode_w - cfg.pip_w - cfg.pip_x;
                dst_y = mode_h - cfg.pip_h - cfg.pip_y;
                break;
            default:
                dst_x = cfg.pip_x;
                dst_y = mode_h - cfg.pip_h - cfg.pip_y;
                break;
            }
            if (set_plane_scaled(fd, planes[nv12_idx].plane_id, crtc_id, video.fb_id,
                                 dst_x, dst_y, cfg.pip_w, cfg.pip_h, cfg.content_w,
                                 cfg.content_h) < 0) {
                fprintf(stderr, "WARN: SetPlane animate failed: %s\n", strerror(errno));
            } else if (cfg.verbose) {
                printf("animate: dst (%d,%d %dx%d)\n", dst_x, dst_y, cfg.pip_w,
                       cfg.pip_h);
            }
        }

        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        poll(&pfd, 1, 16);
        frame++;
    }

    printf("PASS: DRM plane scaling test completed (%d frames)\n", frame);
    printf("Expected: black fullscreen + color-bar PiP at %dx%d (animated unless "
           "--no-animate)\n",
           cfg.pip_w, cfg.pip_h);
    ret = 0;

out:
    if (nv12_idx >= 0 && plane_count > 0) {
        drmModeSetPlane(fd, planes[nv12_idx].plane_id, crtc_id, 0, 0, 0, 0, 0, 0, 0,
                        0, 0, 0);
    }
    if (saved) {
        drmModeSetCrtc(fd, saved->crtc_id, saved->buffer_id, saved->x, saved->y,
                       &conn->connector_id, 1, &saved->mode);
        drmModeFreeCrtc(saved);
    }
    dumb_destroy(fd, &video);
    dumb_destroy(fd, &bg);
    drmModeFreeConnector(conn);
    close(fd);
    return ret;
}
