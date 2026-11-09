/* hdmi_passthrough.c — RK3588 HDMI 直通显示冒烟。
 *
 * V4L2 HDMI-IN (/dev/video0, BGR3) MMAP 采集 → CPU 转 NV12 → DRM 全屏显示。
 * 不经过 MPP 编解码与 ST2110 网络，用于与完整链路显示做 A/B 对比：
 * 直通正常而链路异常 → 查编码/网络/解码/呈现；直通同样异常 → 查采集源/驱动。
 *
 * 板端编译（持久目录 ~/code/mtl）：
 *   gcc ~/code/mtl/src/../debug/tests/hdmi_passthrough.c \
 *       -o ~/code/mtl/bin/hdmi_passthrough -I/usr/include/libdrm -ldrm
 *
 * 运行（建议文本 VT，避免 DRM master 冲突）：
 *   ~/code/mtl/bin/hdmi_passthrough --duration 20
 *
 * 退出时恢复原 CRTC，与 drm_smoke.c 相同的善后约定。
 */
#include <libdrm/drm.h>
#include <libdrm/drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define NBUF 4

static volatile bool g_run = true;
static void on_sig(int s) {
    (void)s;
    g_run = false;
}

struct args {
    const char* drm_dev;
    const char* connector;
    const char* mode_str;
    const char* vdev;
    int width;
    int height;
    int duration_sec;
    bool verbose;
};

struct vbuf {
    void* start;
    size_t len;
};

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

/* ---- DRM helpers (same convention as debug/tests/drm_smoke.c) ---- */

static int drm_open_dev(const char* path) {
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror(path);
        return -1;
    }
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0) {
        fprintf(stderr, "drmSetClientCap: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static drmModeConnector* find_conn(int fd, const char* want) {
    drmModeRes* res = drmModeGetResources(fd);
    if (!res)
        return NULL;
    drmModeConnector* out = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c)
            continue;
        bool name_ok = !want || (c->connector_type == DRM_MODE_CONNECTOR_HDMIA);
        if (c->connection == DRM_MODE_CONNECTED && name_ok) {
            out = c;
            break;
        }
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);
    return out;
}

static drmModeModeInfo* pick_mode(drmModeConnector* c, const char* want, int* w,
                                  int* h) {
    int ww = 1920, hh = 1080, vv = 60;
    if (want)
        sscanf(want, "%dx%d@%d", &ww, &hh, &vv);
    drmModeModeInfo* best = NULL;
    for (int i = 0; i < c->count_modes; i++) {
        if (c->modes[i].hdisplay == ww && c->modes[i].vdisplay == hh) {
            if (!best)
                best = &c->modes[i];
            if (abs((int)c->modes[i].vrefresh - vv) <
                abs((int)best->vrefresh - vv))
                best = &c->modes[i];
        }
    }
    if (!best)
        return NULL;
    *w = best->hdisplay;
    *h = best->vdisplay;
    return best;
}

struct dumb {
    uint32_t handle;
    uint32_t fb_id;
    uint32_t pitch;
    size_t size;
    void* map;
};

static int dumb_make(int fd, int w, int h, struct dumb* b) {
    memset(b, 0, sizeof(*b));
    struct drm_mode_create_dumb cr = {
        .width = (uint32_t)w,
        .height = (uint32_t)(h + h / 2),
        .bpp = 8,
    };
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cr) < 0)
        return -errno;
    b->handle = cr.handle;
    b->pitch = cr.pitch;
    b->size = cr.size;
    struct drm_mode_map_dumb mp = {.handle = b->handle};
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mp) < 0)
        return -errno;
    b->map = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                  (off_t)mp.offset);
    if (b->map == MAP_FAILED)
        return -errno;
    struct drm_mode_fb_cmd2 fb = {0};
    fb.width = (uint32_t)w;
    fb.height = (uint32_t)h;
    fb.pixel_format = DRM_FORMAT_NV12;
    fb.handles[0] = b->handle;
    fb.pitches[0] = b->pitch;
    fb.handles[1] = b->handle;
    fb.pitches[1] = b->pitch;
    fb.offsets[1] = b->pitch * (uint32_t)h;
    if (drmIoctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0)
        return -errno;
    b->fb_id = fb.fb_id;
    return 0;
}

static void dumb_free(int fd, struct dumb* b) {
    if (!b)
        return;
    if (b->map && b->map != MAP_FAILED)
        munmap(b->map, b->size);
    if (b->fb_id)
        drmModeRmFB(fd, b->fb_id);
    if (b->handle) {
        struct drm_mode_destroy_dumb d = {.handle = b->handle};
        drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    }
    memset(b, 0, sizeof(*b));
}

static bool flip_done = false;
static void flip_handler(int fd, unsigned int seq, unsigned int tv_sec,
                         unsigned int tv_usec, void* data) {
    (void)fd;
    (void)seq;
    (void)tv_sec;
    (void)tv_usec;
    (void)data;
    flip_done = true;
}

/* ---- BGR3 -> NV12 (BT.601 integer, 2x2 box for UV) ---- */
static inline uint8_t clamp8(int v) {
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (uint8_t)v;
}

static void bgr3_to_nv12(const uint8_t* src, int src_stride, uint8_t* y,
                         uint8_t* uv, uint32_t dst_pitch, int w, int h) {
    for (int r = 0; r < h; r++) {
        const uint8_t* s = src + (size_t)r * (size_t)src_stride;
        uint8_t* yd = y + (size_t)r * dst_pitch;
        for (int c = 0; c < w; c++) {
            int b = s[c * 3 + 0], g = s[c * 3 + 1], rr = s[c * 3 + 2];
            yd[c] = clamp8((77 * rr + 150 * g + 29 * b) >> 8);
        }
    }
    for (int r = 0; r < h / 2; r++) {
        uint8_t* uvd = uv + (size_t)r * dst_pitch;
        const uint8_t* s0 = src + (size_t)(r * 2) * (size_t)src_stride;
        const uint8_t* s1 = src + (size_t)(r * 2 + 1) * (size_t)src_stride;
        for (int c = 0; c < w; c += 2) {
            int b = (s0[c * 3] + s0[c * 3 + 3] + s1[c * 3] + s1[c * 3 + 3]) / 4;
            int g = (s0[c * 3 + 1] + s0[c * 3 + 4] + s1[c * 3 + 1] +
                     s1[c * 3 + 4]) / 4;
            int rr = (s0[c * 3 + 2] + s0[c * 3 + 5] + s1[c * 3 + 2] +
                      s1[c * 3 + 5]) / 4;
            uvd[c] = clamp8(((-43 * rr - 84 * g + 127 * b) >> 8) + 128);
            uvd[c + 1] = clamp8(((127 * rr - 106 * g - 21 * b) >> 8) + 128);
        }
    }
}

static void usage(const char* p) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --device PATH     DRM card (default /dev/dri/card0)\n"
            "  --connector NAME  (default HDMI-A, HDMIA type match)\n"
            "  --mode WxH@Hz     (default 1920x1080@60)\n"
            "  --vdev PATH       V4L2 capture (default /dev/video0)\n"
            "  --width W --height H  capture size (default 1920 1080)\n"
            "  --duration SEC    run time (default 20)\n"
            "  --verbose         extra logging\n",
            p);
}

int main(int argc, char** argv) {
    struct args a = {
        .drm_dev = "/dev/dri/card0",
        .connector = "HDMI-A",
        .mode_str = "1920x1080@60",
        .vdev = "/dev/video0",
        .width = 1920,
        .height = 1080,
        .duration_sec = 20,
        .verbose = false,
    };
    static struct option opts[] = {
        {"device", required_argument, NULL, 'd'},
        {"connector", required_argument, NULL, 'c'},
        {"mode", required_argument, NULL, 'm'},
        {"vdev", required_argument, NULL, 'D'},
        {"width", required_argument, NULL, 'W'},
        {"height", required_argument, NULL, 'H'},
        {"duration", required_argument, NULL, 't'},
        {"verbose", no_argument, NULL, 'v'},
        {NULL, 0, NULL, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "d:c:m:D:W:H:t:v", opts, NULL)) != -1) {
        switch (c) {
        case 'd':
            a.drm_dev = optarg;
            break;
        case 'c':
            a.connector = optarg;
            break;
        case 'm':
            a.mode_str = optarg;
            break;
        case 'D':
            a.vdev = optarg;
            break;
        case 'W':
            a.width = atoi(optarg);
            break;
        case 'H':
            a.height = atoi(optarg);
            break;
        case 't':
            a.duration_sec = atoi(optarg);
            break;
        case 'v':
            a.verbose = true;
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    /* ---- V4L2 MPLANE MMAP capture, BGR3 ---- */
    int vfd = open(a.vdev, O_RDWR);
    if (vfd < 0) {
        perror(a.vdev);
        return 1;
    }
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = (uint32_t)a.width;
    fmt.fmt.pix_mp.height = (uint32_t)a.height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_BGR24;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;
    if (xioctl(vfd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT BGR3");
        close(vfd);
        return 1;
    }
    struct v4l2_format got = {0};
    got.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(vfd, VIDIOC_G_FMT, &got) < 0) {
        perror("VIDIOC_G_FMT");
        close(vfd);
        return 1;
    }
    if (got.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_BGR24) {
        fprintf(stderr, "FAIL: BGR3 refused, got fourcc 0x%08x\n",
                got.fmt.pix_mp.pixelformat);
        close(vfd);
        return 1;
    }
    int stride = (int)got.fmt.pix_mp.plane_fmt[0].bytesperline;
    printf("v4l2: %ux%u BGR3 stride=%d\n", got.fmt.pix_mp.width,
           got.fmt.pix_mp.height, stride);

    struct v4l2_requestbuffers req = {0};
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    req.count = NBUF;
    if (xioctl(vfd, VIDIOC_REQBUFS, &req) < 0 || req.count == 0) {
        perror("VIDIOC_REQBUFS");
        close(vfd);
        return 1;
    }
    struct vbuf vb[NBUF] = {0};
    for (unsigned i = 0; i < req.count && i < NBUF; i++) {
        struct v4l2_buffer b = {0};
        struct v4l2_plane p = {0};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        b.length = 1;
        b.m.planes = &p;
        if (xioctl(vfd, VIDIOC_QUERYBUF, &b) < 0) {
            perror("VIDIOC_QUERYBUF");
            close(vfd);
            return 1;
        }
        vb[i].len = p.length;
        vb[i].start =
            mmap(NULL, p.length, PROT_READ, MAP_SHARED, vfd, (off_t)p.m.mem_offset);
        if (vb[i].start == MAP_FAILED) {
            perror("mmap");
            close(vfd);
            return 1;
        }
        struct v4l2_buffer q = {0};
        struct v4l2_plane qp = {0};
        q.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        q.memory = V4L2_MEMORY_MMAP;
        q.index = i;
        q.length = 1;
        q.m.planes = &qp;
        if (xioctl(vfd, VIDIOC_QBUF, &q) < 0) {
            perror("VIDIOC_QBUF");
            close(vfd);
            return 1;
        }
    }
    enum v4l2_buf_type tp = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(vfd, VIDIOC_STREAMON, &tp) < 0) {
        perror("VIDIOC_STREAMON");
        close(vfd);
        return 1;
    }

    /* ---- DRM fullscreen ---- */
    int dfd = drm_open_dev(a.drm_dev);
    if (dfd < 0) {
        xioctl(vfd, VIDIOC_STREAMOFF, &tp);
        close(vfd);
        return 1;
    }
    if (drmSetMaster(dfd) < 0 && a.verbose)
        fprintf(stderr, "note: drmSetMaster: %s\n", strerror(errno));
    drmModeConnector* conn = find_conn(dfd, a.connector);
    if (!conn) {
        fprintf(stderr, "No connected connector\n");
        close(dfd);
        xioctl(vfd, VIDIOC_STREAMOFF, &tp);
        close(vfd);
        return 1;
    }
    drmModeEncoder* enc = drmModeGetEncoder(dfd, conn->encoder_id);
    if (!enc || !enc->crtc_id) {
        fprintf(stderr, "No active CRTC\n");
        drmModeFreeConnector(conn);
        close(dfd);
        xioctl(vfd, VIDIOC_STREAMOFF, &tp);
        close(vfd);
        return 1;
    }
    uint32_t crtc_id = enc->crtc_id;
    drmModeFreeEncoder(enc);
    int mw = 0, mh = 0;
    drmModeModeInfo* mode = pick_mode(conn, a.mode_str, &mw, &mh);
    if (!mode) {
        fprintf(stderr, "No display mode\n");
        drmModeFreeConnector(conn);
        close(dfd);
        xioctl(vfd, VIDIOC_STREAMOFF, &tp);
        close(vfd);
        return 1;
    }
    printf("drm: %dx%d@%d crtc=%u\n", mw, mh, mode->vrefresh, crtc_id);
    drmModeCrtc* saved = drmModeGetCrtc(dfd, crtc_id);

    struct dumb fb[2] = {0};
    for (int i = 0; i < 2; i++) {
        if (dumb_make(dfd, a.width, a.height, &fb[i]) < 0) {
            perror("dumb_make NV12");
            goto out;
        }
    }
    if (drmModeSetCrtc(dfd, crtc_id, fb[0].fb_id, 0, 0, &conn->connector_id, 1,
                       mode) < 0) {
        perror("SetCrtc");
        goto out;
    }
    printf("passthrough: %dx%d BGR3 stride=%d -> NV12 fullscreen, %ds\n", a.width,
           a.height, stride, a.duration_sec);
    printf("watch: should be 1:1 copy of HDMI-IN (compare with full-chain run)\n");

    uint64_t t0 = now_ms();
    uint64_t last_rep = t0;
    int frames = 0, conv_ms_ema = 0, back = 1, setcrtc_fallback = 0;
    bool first_flip = true;
    while (g_run) {
        if ((int)(now_ms() - t0) / 1000 >= a.duration_sec)
            break;
        struct pollfd pfd = {.fd = vfd, .events = POLLIN};
        if (poll(&pfd, 1, 1000) <= 0)
            continue;
        struct v4l2_buffer dq = {0};
        struct v4l2_plane dqp = {0};
        dq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        dq.memory = V4L2_MEMORY_MMAP;
        dq.length = 1;
        dq.m.planes = &dqp;
        if (xioctl(vfd, VIDIOC_DQBUF, &dq) < 0)
            continue;
        if (dqp.bytesused == 0) {
            xioctl(vfd, VIDIOC_QBUF, &dq);
            continue;
        }
        uint64_t c0 = now_ms();
        uint8_t* y = (uint8_t*)fb[back].map;
        uint8_t* uv = y + (size_t)fb[back].pitch * (size_t)a.height;
        bgr3_to_nv12((const uint8_t*)vb[dq.index].start, stride, y, uv,
                     fb[back].pitch, a.width, a.height);
        conv_ms_ema = (int)((uint64_t)conv_ms_ema * 9 + (now_ms() - c0) + 5) / 10;
        if (first_flip) {
            drmModeSetCrtc(dfd, crtc_id, fb[back].fb_id, 0, 0,
                           &conn->connector_id, 1, mode);
            first_flip = false;
        } else if (!setcrtc_fallback) {
            flip_done = false;
            if (drmModePageFlip(dfd, crtc_id, fb[back].fb_id,
                                DRM_MODE_PAGE_FLIP_EVENT, NULL) == 0) {
                struct pollfd p2 = {.fd = dfd, .events = POLLIN};
                uint64_t w0 = now_ms();
                while (!flip_done && now_ms() - w0 < 100) {
                    if (poll(&p2, 1, 100) <= 0)
                        break;
                    drmEventContext ev = {0};
                    ev.version = DRM_EVENT_CONTEXT_VERSION;
                    ev.page_flip_handler = flip_handler;
                    drmHandleEvent(dfd, &ev);
                }
            } else {
                setcrtc_fallback++;
                drmModeSetCrtc(dfd, crtc_id, fb[back].fb_id, 0, 0,
                               &conn->connector_id, 1, mode);
                if (setcrtc_fallback > 2)
                    fprintf(stderr,
                            "note: PageFlip unavailable, staying on SetCrtc "
                            "(tearing possible)\n");
            }
        } else {
            drmModeSetCrtc(dfd, crtc_id, fb[back].fb_id, 0, 0,
                           &conn->connector_id, 1, mode);
        }
        back ^= 1;
        xioctl(vfd, VIDIOC_QBUF, &dq);
        frames++;
        if (frames % 60 == 0 && a.verbose) {
            uint64_t el = now_ms() - t0;
            printf("  %d frames in %llus (%.1ffps) conv_ema=%dms\n", frames,
                   (unsigned long long)el / 1000, frames * 1000.0 / (el + 1),
                   conv_ms_ema);
            last_rep = now_ms();
            (void)last_rep;
        }
    }
    {
        uint64_t el = now_ms() - t0;
        printf("PASS: passthrough %d frames in %llu.%03us (%.1ffps) conv_ema=%dms%s\n",
               frames, (unsigned long long)el / 1000, (unsigned)el % 1000,
               frames * 1000.0 / (el + 1), conv_ms_ema,
               setcrtc_fallback > 2 ? " [SetCrtc fallback]" : "");
    }

out:
    if (saved) {
        drmModeSetCrtc(dfd, saved->crtc_id, saved->buffer_id, saved->x, saved->y,
                       &conn->connector_id, 1, &saved->mode);
        drmModeFreeCrtc(saved);
    }
    dumb_free(dfd, &fb[0]);
    dumb_free(dfd, &fb[1]);
    drmModeFreeConnector(conn);
    close(dfd);
    xioctl(vfd, VIDIOC_STREAMOFF, &tp);
    for (unsigned i = 0; i < NBUF; i++)
        if (vb[i].start && vb[i].start != MAP_FAILED)
            munmap(vb[i].start, vb[i].len);
    close(vfd);
    return 0;
}
