#include "capability.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef HAVE_LIBDRM
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#endif

#include <json-c/json.h>

static void cap_defaults(dvs_capability* cap) {
    cap->output_ports = NULL;
    cap->output_mode_counts = NULL;
    cap->output_port_count = 0;
    /* Conservative default; real MPP-measured value is filled at S1 end and
     * may be overridden via DVS_MAX_DECODE_SESSIONS for bring-up. */
    cap->max_decode_sessions = 4;
    cap->max_layers = 4; /* overridden by DRM plane probe when available */
    snprintf(cap->ptp_state, sizeof(cap->ptp_state), "unknown");
    cap->ptp_offset_us = 0;
    snprintf(cap->mtl_version, sizeof(cap->mtl_version), "unknown");
    snprintf(cap->mtl_backend, sizeof(cap->mtl_backend), "dpdk");
}

void dvs_capability_free(dvs_capability* cap) {
    if (!cap) return;
    for (int i = 0; i < cap->output_port_count; i++) free(cap->output_ports[i]);
    free(cap->output_ports);
    free(cap->output_mode_counts);
    cap->output_ports = NULL;
    cap->output_mode_counts = NULL;
    cap->output_port_count = 0;
}

static void probe_drm(dvs_capability* cap) {
#ifdef HAVE_LIBDRM
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
    int k = 0;
    for (int i = 0; i < n; i++) {
        drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        if (c->connection != DRM_MODE_CONNECTED && c->count_modes == 0) {
            drmModeFreeConnector(c);
            continue;
        }
        cap->output_ports[k] = strdup(c->name);
        cap->output_mode_counts[k] = c->count_modes;
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
#else
    fprintf(stderr, "capability: built without libdrm, using default layers\n");
#endif
}

int dvs_capability_probe(dvs_capability* cap) {
    if (!cap) return -1;
    cap_defaults(cap);
#ifdef HAVE_LIBDRM
    probe_drm(cap);
#endif

    const char* m = getenv("DVS_MAX_DECODE_SESSIONS");
    if (m) {
        int v = atoi(m);
        if (v > 0) cap->max_decode_sessions = v;
    }
    /* TODO(S1): query MTL PTP portState/offset via mtl_get_port_stats and
     * fill cap->ptp_state / cap->ptp_offset_us; query mtl_version() for
     * cap->mtl_version. Left as defaults until the MTL session is initialized
     * in the engine (T3/T4). */
    return 0;
}

char* dvs_capability_to_json(const dvs_capability* cap) {
    if (!cap) return NULL;
    json_object* o = json_object_new_object();

    json_object* ports = json_object_new_array();
    for (int i = 0; i < cap->output_port_count; i++) {
        json_object* p = json_object_new_object();
        json_object_object_add(p, "name",
                              json_object_new_string(cap->output_ports[i]));
        json_object_object_add(p, "safeModes",
                              json_object_new_int(cap->output_mode_counts[i]));
        json_object_array_add(ports, p);
    }
    json_object_object_add(o, "outputPorts", ports);

    json_object_object_add(o, "maxDecodeSessions",
                          json_object_new_int(cap->max_decode_sessions));
    json_object_object_add(o, "maxLayers", json_object_new_int(cap->max_layers));

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
