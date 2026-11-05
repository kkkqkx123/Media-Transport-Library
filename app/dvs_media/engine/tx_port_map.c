#include "tx_port_map.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#endif

int dvs_tx_table_build(dvs_tx_port_entry* out, int cap,
                       const char devices[][DVS_TX_DEV_LEN],
                       const char stable_ids[][DVS_TX_STABLE_LEN],
                       const int* has_signal, int n) {
    if (!out || cap <= 0 || n <= 0) return 0;
    if (n > cap) n = cap;
    if (n > DVS_TX_PORT_MAX) n = DVS_TX_PORT_MAX;
    for (int i = 0; i < n; i++) {
        dvs_tx_port_entry* e = &out[i];
        snprintf(e->port, sizeof(e->port), "HDMI%d", i + 1);
        if (devices && devices[i][0])
            snprintf(e->device, sizeof(e->device), "%s", devices[i]);
        else
            e->device[0] = '\0';
        if (stable_ids && stable_ids[i][0])
            snprintf(e->stable_id, sizeof(e->stable_id), "%s", stable_ids[i]);
        else if (devices && devices[i][0])
            snprintf(e->stable_id, sizeof(e->stable_id), "%s", devices[i]);
        else
            e->stable_id[0] = '\0';
        e->has_signal = has_signal ? (has_signal[i] != 0) : 0;
    }
    return n;
}

int dvs_tx_port_index(const char* port) {
    if (!port) return -1;
    if (strncmp(port, "HDMI", 4) != 0) return -1;
    const char* d = port + 4;
    if (*d < '1' || *d > '8' || d[1] != '\0') return -1;
    return *d - '0';
}

int dvs_tx_format_available(const dvs_tx_port_entry* table, int n,
                            char* buf, size_t len) {
    if (!buf || len == 0) return -1;
    buf[0] = '\0';
    if (!table || n <= 0) {
        snprintf(buf, len, "(no capture devices)");
        return 0;
    }
    size_t pos = 0;
    for (int i = 0; i < n && pos + 1 < len; i++) {
        char item[160];
        snprintf(item, sizeof(item), "%s%s(%s)%s", i ? ", " : "",
                 table[i].port,
                 table[i].device[0] ? table[i].device : "?",
                 table[i].has_signal ? "" : "[no-signal]");
        size_t k = strlen(item);
        if (pos + k >= len) break;
        memcpy(buf + pos, item, k);
        pos += k;
    }
    buf[pos] = '\0';
    return 0;
}

static int stable_equal(const char* a, const char* b) {
    if (!a || !b || !a[0] || !b[0]) return 0;
    return strcmp(a, b) == 0;
}

int dvs_tx_resolve_device(const dvs_tx_port_entry* table, int table_n,
                          const char devices[][DVS_TX_DEV_LEN],
                          const char stable_ids[][DVS_TX_STABLE_LEN],
                          const int* has_signal, int m,
                          const char* requested,
                          char* out_device, size_t out_len,
                          char* err, size_t errlen) {
    char avail[512];
    dvs_tx_format_available(table, table_n, avail, sizeof(avail));
    if (!requested || !requested[0]) {
        if (err && errlen > 0)
            snprintf(err, errlen, "bad capture port (available: %s)", avail);
        return -1;
    }
    /* Direct device path: accept when the rescan still sees it. */
    if (strncmp(requested, "/dev/", 5) == 0) {
        for (int i = 0; i < m; i++) {
            if (devices && strcmp(devices[i], requested) == 0) {
                if (has_signal && !has_signal[i]) {
                    if (err && errlen > 0)
                        snprintf(err, errlen,
                                 "capture %s has no signal (available: %s)",
                                 requested, avail);
                    return -1;
                }
                if (out_device && out_len > 0)
                    snprintf(out_device, out_len, "%s", requested);
                return 0;
            }
        }
        if (err && errlen > 0)
            snprintf(err, errlen, "capture %s not present (available: %s)",
                     requested, avail);
        return -1;
    }
    /* Stable ID: match the fresh scan, then report its current path. */
    if (table && table_n > 0) {
        for (int i = 0; i < table_n; i++) {
            if (stable_equal(table[i].stable_id, requested)) {
                for (int j = 0; j < m; j++) {
                    if (stable_ids && stable_equal(stable_ids[j], requested)) {
                        if (has_signal && !has_signal[j]) {
                            if (err && errlen > 0)
                                snprintf(err, errlen,
                                         "capture %s has no signal (available: %s)",
                                         table[i].port, avail);
                            return -1;
                        }
                        if (out_device && out_len > 0)
                            snprintf(out_device, out_len, "%s", devices[j]);
                        return 0;
                    }
                }
                if (err && errlen > 0)
                    snprintf(err, errlen,
                             "capture %s not present (available: %s)",
                             table[i].port, avail);
                return -1;
            }
        }
    }
    /* HDMIk: startup stable ID first, startup path as fallback. */
    int k = dvs_tx_port_index(requested);
    if (k > 0 && table && k <= table_n) {
        const dvs_tx_port_entry* e = &table[k - 1];
        if (e->stable_id[0] && stable_ids) {
            for (int j = 0; j < m; j++) {
                if (stable_equal(stable_ids[j], e->stable_id)) {
                    if (has_signal && !has_signal[j]) {
                        if (err && errlen > 0)
                            snprintf(err, errlen,
                                     "capture %s has no signal (available: %s)",
                                     requested, avail);
                        return -1;
                    }
                    if (out_device && out_len > 0)
                        snprintf(out_device, out_len, "%s", devices[j]);
                    return 0;
                }
            }
        }
        for (int j = 0; j < m; j++) {
            if (devices && e->device[0] && strcmp(devices[j], e->device) == 0) {
                if (has_signal && !has_signal[j]) {
                    if (err && errlen > 0)
                        snprintf(err, errlen,
                                 "capture %s has no signal (available: %s)",
                                 requested, avail);
                    return -1;
                }
                if (out_device && out_len > 0)
                    snprintf(out_device, out_len, "%s", devices[j]);
                return 0;
            }
        }
        if (err && errlen > 0)
            snprintf(err, errlen, "capture %s not present (available: %s)",
                     requested, avail);
        return -1;
    }
    if (err && errlen > 0)
        snprintf(err, errlen, "bad capture port '%s' (available: %s)",
                 requested, avail);
    return -1;
}

int dvs_tx_codec_from_name(const char* codec) {
    if (!codec) return -1;
    if (strcmp(codec, "h264") == 0) return 0;
    if (strcmp(codec, "h265") == 0) return 1;
    return -1;
}

int dvs_tx_bitrate_to_bps(int kbps, int* bps_out) {
    if (kbps <= 0 || !bps_out) return -1;
    *bps_out = kbps * 1000;
    return 0;
}

int dvs_tx_default_gop(int fps) {
    if (fps <= 0) return 60;
    if (fps > 120) return 120;
    return fps;
}

#ifdef __linux__
static int query_signal(int fd) {
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd, VIDIOC_G_FMT, &fmt) == 0) {
        int w = (int)fmt.fmt.pix_mp.width;
        int h = (int)fmt.fmt.pix_mp.height;
        if (w <= 0 || h <= 0 || (w <= 128 && h <= 128)) return 0;
        return 1;
    }
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_FMT, &fmt) == 0) {
        int w = (int)fmt.fmt.pix.width;
        int h = (int)fmt.fmt.pix.height;
        if (w <= 0 || h <= 0 || (w <= 128 && h <= 128)) return 0;
        return 1;
    }
    return 0;
}
#endif

int dvs_tx_scan_devices(char devices[][DVS_TX_DEV_LEN],
                        char stable_ids[][DVS_TX_STABLE_LEN],
                        int* has_signal, int cap) {
    if (cap <= 0) return 0;
    if (cap > DVS_TX_PORT_MAX) cap = DVS_TX_PORT_MAX;
    int n = 0;
    for (int i = 0; i < 16 && n < cap; i++) {
        char path[DVS_TX_DEV_LEN];
        snprintf(path, sizeof(path), "/dev/video%d", i);
        if (access(path, F_OK) != 0) continue;
        char stable[DVS_TX_STABLE_LEN];
        int signal = 0;
        snprintf(stable, sizeof(stable), "%s", path);
#ifdef __linux__
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            struct v4l2_capability qcap;
            memset(&qcap, 0, sizeof(qcap));
            if (ioctl(fd, VIDIOC_QUERYCAP, &qcap) == 0 &&
                qcap.card[0] && qcap.bus_info[0]) {
                snprintf(stable, sizeof(stable), "%s|%s|in0",
                         qcap.card, qcap.bus_info);
            }
            signal = query_signal(fd);
            close(fd);
        }
#else
        (void)signal;
#endif
        if (devices) snprintf(devices[n], DVS_TX_DEV_LEN, "%s", path);
        if (stable_ids) snprintf(stable_ids[n], DVS_TX_STABLE_LEN, "%s", stable);
        if (has_signal) has_signal[n] = signal;
        n++;
    }
    return n;
}
