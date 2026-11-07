#include "tx_session.h"

#include <json-c/json.h>

#include <stdio.h>
#include <string.h>

const char* dvs_tx_state_name(dvs_tx_state_t s) {
    switch (s) {
        case DVS_TX_CONFIGURED: return "configured";
        case DVS_TX_RUNNING:    return "running";
        case DVS_TX_FAILED:     return "failed";
        case DVS_TX_IDLE:
        default:                return "idle";
    }
}

void dvs_tx_init(dvs_tx_session_t* s) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->state = DVS_TX_IDLE;
}

void dvs_tx_set_backend(dvs_tx_session_t* s, dvs_tx_backend_fn fn) {
    if (!s) return;
    s->backend = fn;
}

static int valid_ip4(const char* s) {
    if (!s || !s[0]) return 0;
    int parts = 0;
    const char* p = s;
    while (1) {
        if (*p < '0' || *p > '9') return 0;
        int v = 0, digits = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            digits++;
            p++;
            if (digits > 3) return 0;
        }
        if (v > 255) return 0;
        parts++;
        if (*p == '\0') break;
        if (*p != '.') return 0;
        p++;
        if (*p == '\0') return 0;
    }
    return parts == 4;
}

int dvs_tx_validate(const dvs_tx_params_t* p, char* err, size_t errlen) {
    if (!p) {
        if (err && errlen > 0) snprintf(err, errlen, "missing tx params");
        return -1;
    }
    if (!p->port[0] || strlen(p->port) >= sizeof(p->port)) {
        if (err && errlen > 0) snprintf(err, errlen, "bad capture port");
        return -1;
    }
    if (strcmp(p->codec, "h264") != 0 && strcmp(p->codec, "h265") != 0) {
        if (err && errlen > 0) snprintf(err, errlen, "bad codec (h264|h265)");
        return -1;
    }
    if (p->width < DVS_TX_MIN_W || p->width > DVS_TX_MAX_W ||
        p->height < DVS_TX_MIN_H || p->height > DVS_TX_MAX_H) {
        if (err && errlen > 0) snprintf(err, errlen, "resolution out of range");
        return -1;
    }
    if (p->fps < DVS_TX_MIN_FPS || p->fps > DVS_TX_MAX_FPS) {
        if (err && errlen > 0) snprintf(err, errlen, "fps out of range");
        return -1;
    }
    if (p->bitrate_kbps < DVS_TX_MIN_BR_KBPS || p->bitrate_kbps > DVS_TX_MAX_BR_KBPS) {
        if (err && errlen > 0) snprintf(err, errlen, "bitrate out of range");
        return -1;
    }
    if (!valid_ip4(p->target_sip)) {
        if (err && errlen > 0) snprintf(err, errlen, "bad target ip");
        return -1;
    }
    if (p->target_port < 1 || p->target_port > 65535) {
        if (err && errlen > 0) snprintf(err, errlen, "bad target port");
        return -1;
    }
    return 0;
}

int dvs_tx_validate_against_capability(const dvs_tx_params_t* p,
                                       const dvs_tx_cap_t* cap,
                                       char* err, size_t errlen) {
    if (!p) {
        if (err && errlen > 0) snprintf(err, errlen, "missing tx params");
        return -1;
    }
    if (!cap) return 0;
    if (cap->codecs[0]) {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "%s", cap->codecs);
        int ok = 0;
        for (char* tok = strtok(tmp, ","); tok; tok = strtok(NULL, ",")) {
            while (*tok == ' ') tok++;
            if (strcmp(tok, p->codec) == 0) {
                ok = 1;
                break;
            }
        }
        if (!ok) {
            if (err && errlen > 0)
                snprintf(err, errlen, "codec %s not in capability", p->codec);
            return -1;
        }
    }
    if ((cap->max_w > 0 && p->width > cap->max_w) ||
        (cap->max_h > 0 && p->height > cap->max_h)) {
        if (err && errlen > 0)
            snprintf(err, errlen, "resolution exceeds capability");
        return -1;
    }
    if (cap->max_fps > 0 && p->fps > cap->max_fps) {
        if (err && errlen > 0) snprintf(err, errlen, "fps exceeds capability");
        return -1;
    }
    if (cap->max_br_kbps > 0 && p->bitrate_kbps > cap->max_br_kbps) {
        if (err && errlen > 0)
            snprintf(err, errlen, "bitrate exceeds capability");
        return -1;
    }
    return 0;
}

int dvs_tx_start(dvs_tx_session_t* s, const dvs_tx_params_t* p,
                 const char* role, char* err, size_t errlen) {
    if (!s || !p) {
        if (err && errlen > 0) snprintf(err, errlen, "missing tx params");
        return -1;
    }
    if (!role || strcmp(role, "input") != 0) {
        s->state = DVS_TX_FAILED;
        snprintf(s->error, sizeof(s->error), "TX_ROLE_MISMATCH: tx needs input role");
        if (err && errlen > 0)
            snprintf(err, errlen, "TX_ROLE_MISMATCH: tx needs input role");
        return -1;
    }
    if (dvs_tx_validate(p, err, errlen) != 0) {
        s->state = DVS_TX_FAILED;
        snprintf(s->error, sizeof(s->error), "%s",
                 (err && err[0]) ? err : "invalid tx params");
        return -1;
    }
    s->params = *p;
    s->error[0] = '\0';
    s->backend_pending = 0;

    if (!s->backend) {
        /* Backend-less build: params are stored and validated, but nothing
         * flows. Report configured, not running. */
        s->state = DVS_TX_CONFIGURED;
        s->backend_pending = 1;
        snprintf(s->error, sizeof(s->error),
                 "TX_BACKEND_PENDING: tx data plane not linked on this build");
        return 0;
    }
    int rc = s->backend(p, s->error, sizeof(s->error));
    if (rc == 0) {
        s->state = DVS_TX_RUNNING;
        s->error[0] = '\0';
        return 0;
    }
    if (rc > 0) {
        s->state = DVS_TX_CONFIGURED;
        s->backend_pending = 1;
        if (!s->error[0])
            snprintf(s->error, sizeof(s->error),
                     "TX_BACKEND_PENDING: tx data plane not linked on this build");
        return 0;
    }
    s->state = DVS_TX_FAILED;
    if (!s->error[0]) snprintf(s->error, sizeof(s->error), "backend start failed");
    if (err && errlen > 0) snprintf(err, errlen, "%s", s->error);
    return -1;
}

void dvs_tx_stop(dvs_tx_session_t* s) {
    if (!s) return;
    /* State only: data-plane teardown is owned by the engine, which stops
     * the backend before calling here on every teardown path. */
    memset(&s->params, 0, sizeof(s->params));
    s->error[0] = '\0';
    s->backend_pending = 0;
    s->state = DVS_TX_IDLE;
}

struct json_object* dvs_tx_status_json(const dvs_tx_session_t* s) {
    struct json_object* o = json_object_new_object();
    if (!o) return NULL;
    dvs_tx_state_t st = s ? s->state : DVS_TX_IDLE;
    json_object_object_add(o, "state",
                           json_object_new_string(dvs_tx_state_name(st)));
    json_object_object_add(o, "port",
                           json_object_new_string(s ? s->params.port : ""));
    json_object_object_add(o, "codec",
                           json_object_new_string(s ? s->params.codec : ""));
    json_object_object_add(o, "width", json_object_new_int(s ? s->params.width : 0));
    json_object_object_add(o, "height", json_object_new_int(s ? s->params.height : 0));
    json_object_object_add(o, "fps", json_object_new_int(s ? s->params.fps : 0));
    json_object_object_add(o, "bitrateKbps",
                           json_object_new_int(s ? s->params.bitrate_kbps : 0));
    struct json_object* t = json_object_new_object();
    json_object_object_add(t, "sip",
                           json_object_new_string(s ? s->params.target_sip : ""));
    json_object_object_add(t, "udpPort",
                           json_object_new_int(s ? s->params.target_port : 0));
    json_object_object_add(o, "target", t);
    json_object_object_add(o, "error",
                           json_object_new_string(s ? s->error : ""));
    json_object_object_add(o, "backendPending",
                           json_object_new_boolean(s ? s->backend_pending : 0));
    return o;
}
