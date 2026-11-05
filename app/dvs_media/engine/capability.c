#include "capability.h"
#include "engine/tx_port_map.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#ifdef HAVE_LIBDRM
#include <fcntl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#endif

#include <json-c/json.h>

static void cap_defaults(dvs_capability* cap) {
    cap->output_ports = NULL;
    cap->output_mode_counts = NULL;
    cap->output_max_w = NULL;
    cap->output_max_h = NULL;
    cap->output_max_hz = NULL;
    cap->output_port_count = 0;
    cap->input_ports = NULL;
    cap->input_devices = NULL;
    cap->input_stable = NULL;
    cap->input_signal = NULL;
    cap->input_port_count = 0;
    /* Conservative default; real MPP-measured value is filled at S1 end and
     * may be overridden via DVS_MAX_DECODE_SESSIONS for bring-up. */
    cap->max_decode_sessions = 4;
    cap->max_layers = 4; /* overridden by DRM plane probe when available */
    dvs_capability_tx_defaults(cap);
    snprintf(cap->ptp_state, sizeof(cap->ptp_state), "unknown");
    cap->ptp_offset_us = 0;
    snprintf(cap->mtl_version, sizeof(cap->mtl_version), "unknown");
    snprintf(cap->mtl_backend, sizeof(cap->mtl_backend), "dpdk");
}

void dvs_capability_tx_defaults(dvs_capability* cap) {
    if (!cap) return;
    snprintf(cap->tx_codecs, sizeof(cap->tx_codecs), "h264,h265");
    cap->tx_max_w = 3840;
    cap->tx_max_h = 2160;
    cap->tx_max_fps = 60;
    cap->tx_max_br_kbps = 20000;
}

void dvs_capability_free(dvs_capability* cap) {
    if (!cap) return;
    for (int i = 0; i < cap->output_port_count; i++) free(cap->output_ports[i]);
    free(cap->output_ports);
    free(cap->output_mode_counts);
    free(cap->output_max_w);
    free(cap->output_max_h);
    free(cap->output_max_hz);
    cap->output_ports = NULL;
    cap->output_mode_counts = NULL;
    cap->output_max_w = NULL;
    cap->output_max_h = NULL;
    cap->output_max_hz = NULL;
    cap->output_port_count = 0;
    for (int i = 0; i < cap->input_port_count; i++) {
        free(cap->input_ports[i]);
        free(cap->input_devices[i]);
        free(cap->input_stable[i]);
    }
    free(cap->input_ports);
    free(cap->input_devices);
    free(cap->input_stable);
    free(cap->input_signal);
    cap->input_ports = NULL;
    cap->input_devices = NULL;
    cap->input_stable = NULL;
    cap->input_signal = NULL;
    cap->input_port_count = 0;
}

#ifdef HAVE_LIBDRM
static void probe_drm(dvs_capability* cap) {
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "capability: cannot open /dev/dri/card0\n");
        return;
    }
    drmModeRes* res = drmModeGetResources(fd);
    if (!res) {
        close(fd);
        return;
    }
    int n = res->count_connectors;
    cap->output_ports = calloc((size_t)(n > 0 ? n : 1), sizeof(char*));
    cap->output_mode_counts = calloc((size_t)(n > 0 ? n : 1), sizeof(int));
    cap->output_max_w = calloc((size_t)(n > 0 ? n : 1), sizeof(int));
    cap->output_max_h = calloc((size_t)(n > 0 ? n : 1), sizeof(int));
    cap->output_max_hz = calloc((size_t)(n > 0 ? n : 1), sizeof(int));
    int k = 0;
    for (int i = 0; i < n; i++) {
        drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        if (c->connection != DRM_MODE_CONNECTED && c->count_modes == 0) {
            drmModeFreeConnector(c);
            continue;
        }
        char cname[32];
        snprintf(cname, sizeof(cname), "HDMI-A-%u", c->connector_type_id);
        cap->output_ports[k] = strdup(cname);
        cap->output_mode_counts[k] = c->count_modes;
        int mw = 0, mh = 0, mz = 0;
        for (int m = 0; m < c->count_modes; m++) {
            int w = (int)c->modes[m].hdisplay;
            int h = (int)c->modes[m].vdisplay;
            int z = (int)c->modes[m].vrefresh;
            if (w > mw) mw = w;
            if (h > mh) mh = h;
            if (z > mz) mz = z;
        }
        cap->output_max_w[k] = mw > 0 ? mw : 1920;
        cap->output_max_h[k] = mh > 0 ? mh : 1080;
        cap->output_max_hz[k] = mz > 0 ? mz : 60;
        k++;
        drmModeFreeConnector(c);
    }
    cap->output_port_count = k;
    drmModeFreeResources(res);

    drmModePlaneRes* pr = drmModeGetPlaneResources(fd);
    if (pr) {
        cap->max_layers = pr->count_planes;
        drmModeFreePlaneResources(pr);
    }
    close(fd);
}
#endif /* HAVE_LIBDRM */

static void probe_input(dvs_capability* cap) {
    /* Shared table construction with the TX backend: scan once (physical
     * identity via QUERYCAP, device fallback), bind HDMIk in scan order.
     * HDMIk stays agent-compatible; device/stableId/signal ride along. */
    char devs[DVS_TX_PORT_MAX][DVS_TX_DEV_LEN];
    char stables[DVS_TX_PORT_MAX][DVS_TX_STABLE_LEN];
    int sig[DVS_TX_PORT_MAX];
    memset(devs, 0, sizeof(devs));
    memset(stables, 0, sizeof(stables));
    memset(sig, 0, sizeof(sig));
    int n = dvs_tx_scan_devices(devs, stables, sig, DVS_TX_PORT_MAX);
    if (n <= 0) return;
    dvs_tx_port_entry table[DVS_TX_PORT_MAX];
    int m = dvs_tx_table_build(table, DVS_TX_PORT_MAX, devs, stables, sig, n);
    if (m <= 0) return;
    cap->input_ports = calloc((size_t)m, sizeof(char*));
    cap->input_devices = calloc((size_t)m, sizeof(char*));
    cap->input_stable = calloc((size_t)m, sizeof(char*));
    cap->input_signal = calloc((size_t)m, sizeof(int));
    if (!cap->input_ports || !cap->input_devices || !cap->input_stable ||
        !cap->input_signal) {
        free(cap->input_ports);
        free(cap->input_devices);
        free(cap->input_stable);
        free(cap->input_signal);
        cap->input_ports = NULL;
        cap->input_devices = NULL;
        cap->input_stable = NULL;
        cap->input_signal = NULL;
        return;
    }
    for (int i = 0; i < m; i++) {
        cap->input_ports[i] = strdup(table[i].port);
        cap->input_devices[i] = strdup(table[i].device);
        cap->input_stable[i] = strdup(table[i].stable_id);
        cap->input_signal[i] = table[i].has_signal;
        if (!cap->input_ports[i] || !cap->input_devices[i] ||
            !cap->input_stable[i]) {
            for (int j = 0; j <= i; j++) {
                free(cap->input_ports[j]);
                free(cap->input_devices[j]);
                free(cap->input_stable[j]);
            }
            free(cap->input_ports);
            free(cap->input_devices);
            free(cap->input_stable);
            free(cap->input_signal);
            cap->input_ports = NULL;
            cap->input_devices = NULL;
            cap->input_stable = NULL;
            cap->input_signal = NULL;
            return;
        }
    }
    cap->input_port_count = m;
}

int dvs_capability_probe(dvs_capability* cap) {
    if (!cap) return -1;
    cap_defaults(cap);
#ifdef HAVE_LIBDRM
    probe_drm(cap);
#endif
    probe_input(cap);

    const char* m = getenv("DVS_MAX_DECODE_SESSIONS");
    if (m) {
        int v = atoi(m);
        if (v > 0) cap->max_decode_sessions = v;
    }
    return 0;
}

char* dvs_capability_to_json(const dvs_capability* cap) {
    if (!cap) return NULL;
    json_object* o = json_object_new_object();

    json_object* in_ports = json_object_new_array();
    for (int i = 0; i < cap->input_port_count; i++) {
        json_object* p = json_object_new_object();
        json_object_object_add(p, "id", json_object_new_string(cap->input_ports[i]));
        json_object_object_add(p, "label", json_object_new_string(cap->input_ports[i]));
        json_object_object_add(p, "kind", json_object_new_string("HDMI RX"));
        json_object_object_add(p, "device",
                               json_object_new_string(cap->input_devices
                                                          ? cap->input_devices[i]
                                                          : ""));
        json_object_object_add(p, "stableId",
                               json_object_new_string(cap->input_stable
                                                          ? cap->input_stable[i]
                                                          : ""));
        json_object_object_add(p, "signal",
                               json_object_new_boolean(cap->input_signal
                                                           ? cap->input_signal[i]
                                                           : 0));
        json_object_object_add(p, "maxWidth", json_object_new_int(3840));
        json_object_object_add(p, "maxHeight", json_object_new_int(2160));
        json_object_object_add(p, "maxRefreshHz", json_object_new_int(60));
        json_object_array_add(in_ports, p);
    }
    json_object_object_add(o, "inputPorts", in_ports);

    json_object* ports = json_object_new_array();
    for (int i = 0; i < cap->output_port_count; i++) {
        json_object* p = json_object_new_object();
        json_object_object_add(p, "id", json_object_new_string(cap->output_ports[i]));
        json_object_object_add(p, "label", json_object_new_string(cap->output_ports[i]));
        json_object_object_add(p, "kind", json_object_new_string("HDMI TX"));
        int mw = cap->output_max_w ? cap->output_max_w[i] : 1920;
        int mh = cap->output_max_h ? cap->output_max_h[i] : 1080;
        int mz = cap->output_max_hz ? cap->output_max_hz[i] : 60;
        json_object_object_add(p, "maxWidth", json_object_new_int(mw));
        json_object_object_add(p, "maxHeight", json_object_new_int(mh));
        json_object_object_add(p, "maxRefreshHz", json_object_new_int(mz));
        json_object_object_add(p, "modeCount", json_object_new_int(cap->output_mode_counts[i]));
        json_object_array_add(ports, p);
    }
    json_object_object_add(o, "outputPorts", ports);

    json_object_object_add(o, "maxDecodeSessions",
                          json_object_new_int(cap->max_decode_sessions));
    json_object_object_add(o, "maxLayers", json_object_new_int(cap->max_layers));

    /* TX static ceiling (SoC limits; capture ports reuse inputPorts). */
    json_object* tx = json_object_new_object();
    json_object* codecs = json_object_new_array();
    {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%s", cap->tx_codecs);
        for (char* tok = strtok(tmp, ","); tok; tok = strtok(NULL, ","))
            json_object_array_add(codecs, json_object_new_string(tok));
    }
    json_object_object_add(tx, "codecs", codecs);
    json_object_object_add(tx, "maxWidth", json_object_new_int(cap->tx_max_w));
    json_object_object_add(tx, "maxHeight", json_object_new_int(cap->tx_max_h));
    json_object_object_add(tx, "maxFps", json_object_new_int(cap->tx_max_fps));
    json_object_object_add(tx, "maxBitrateKbps",
                          json_object_new_int(cap->tx_max_br_kbps));
    json_object_object_add(o, "tx", tx);

    json_object* ptp = json_object_new_object();
    json_object_object_add(ptp, "state", json_object_new_string(cap->ptp_state));
    json_object_object_add(ptp, "offsetUs",
                          json_object_new_int(cap->ptp_offset_us));
    json_object_object_add(o, "ptp", ptp);

    json_object* mtl = json_object_new_object();
    json_object_object_add(mtl, "version",
                          json_object_new_string(cap->mtl_version));
    json_object_object_add(mtl, "backend",
                          json_object_new_string(cap->mtl_backend));
    json_object_object_add(o, "mtl", mtl);

    const char* s = json_object_to_json_string(o);
    char* out = s ? strdup(s) : NULL;
    json_object_put(o);
    return out;
}

int dvs_capability_state_json(char* buf, size_t n, int rx_sessions, int decoding,
                              int layers, const dvs_capability* cap) {
    if (!buf || n == 0) return -1;
    const char* ptp_state = cap ? cap->ptp_state : "unknown";
    int ptp_off = cap ? cap->ptp_offset_us : 0;
    int rc = snprintf(
        buf, n,
        "{\"rxSessions\":%d,\"decoding\":%d,\"layers\":%d,"
        "\"ptp\":{\"state\":\"%s\",\"offsetUs\":%d}}",
        rx_sessions, decoding, layers, ptp_state, ptp_off);
    return (rc < 0 || (size_t)rc >= n) ? -1 : 0;
}
