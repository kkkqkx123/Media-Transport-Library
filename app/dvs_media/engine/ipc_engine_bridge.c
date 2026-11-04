/* Engine <-> IPC bridge.
 *
 * Compiled into the real dvs_media binary only (NOT the no-MTL unit test).
 * Wires the IPC server's engine hooks to the actual media engine so the test
 * target links without MTL/MPP. */

#include "ipc/ipc_server.h"
#include "engine_ctx.h"
#include "engine/layout_executor.h"
#include "engine/session_mgr.h"
#include <mtl_api.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static dvs_engine* s_eng = NULL;

static const char* json_str(struct json_object* o, const char* key) {
    struct json_object* v = NULL;
    if (o && json_object_object_get_ex(o, key, &v) && v) return json_object_get_string(v);
    return NULL;
}

static int apply_whitelist(struct json_object* wv, char* err, size_t errlen) {
    char csv[512] = {0};
    if (!wv) { dvs_session_mgr_set_whitelist(NULL); return 0; }
    if (json_object_is_type(wv, json_type_string)) {
        snprintf(csv, sizeof(csv), "%s", json_object_get_string(wv));
    } else if (json_object_is_type(wv, json_type_array)) {
        size_t pos = 0;
        int n = json_object_array_length(wv);
        for (int i = 0; i < n; i++) {
            struct json_object* e = json_object_array_get_idx(wv, i);
            if (!e) continue;
            const char* s = json_object_is_type(e, json_type_string) ? json_object_get_string(e) : NULL;
            char tmp[96] = {0};
            if (s) {
                snprintf(tmp, sizeof(tmp), "%s", s);
            } else {
                const char* ip = json_str(e, "sip");
                if (!ip) ip = json_str(e, "ip");
                struct json_object* pv = NULL;
                int port = 0;
                if (json_object_object_get_ex(e, "udp_port", &pv) && pv) port = json_object_get_int(pv);
                else if (json_object_object_get_ex(e, "udpPort", &pv) && pv) port = json_object_get_int(pv);
                if (!ip || !ip[0]) continue;
                if (port > 0) snprintf(tmp, sizeof(tmp), "%s:%d", ip, port);
                else snprintf(tmp, sizeof(tmp), "%s", ip);
            }
            if (!tmp[0]) continue;
            if (pos > 0 && pos + 1 < sizeof(csv)) csv[pos++] = ',';
            size_t k = strlen(tmp);
            if (pos + k >= sizeof(csv)) break;
            memcpy(csv + pos, tmp, k);
            pos += k;
        }
        csv[pos] = '\0';
    } else if (json_object_is_type(wv, json_type_object)) {
        const char* port = json_str(wv, "port");
        struct json_object* inner = NULL;
        if (json_object_object_get_ex(wv, "whitelist", &inner) && inner)
            return apply_whitelist(inner, err, errlen);
        if (json_object_object_get_ex(wv, "sources", &inner) && inner)
            return apply_whitelist(inner, err, errlen);
        if (port) return 0;
        snprintf(err, errlen, "bad whitelist object");
        return -1;
    } else {
        snprintf(err, errlen, "bad whitelist value");
        return -1;
    }
    dvs_session_mgr_set_whitelist(csv[0] ? csv : NULL);
    return 0;
}

static int hook_prepare(struct json_object* req, char* err, size_t errlen) {
    struct json_object* wv = NULL;
    if (req && json_object_object_get_ex(req, "whitelist", &wv)) return apply_whitelist(wv, err, errlen);
    struct json_object* scopev = NULL;
    if (req && json_object_object_get_ex(req, "scope", &scopev) && scopev) {
        const char* scope = json_object_get_string(scopev);
        struct json_object* value = NULL;
        json_object_object_get_ex(req, "value", &value);
        if (scope && strcmp(scope, "output") == 0) {
            if (!value) { snprintf(err, errlen, "missing output value"); return -1; }
            struct json_object* t = NULL;
            int w = 0, h = 0, z = 0;
            if (json_object_object_get_ex(value, "width", &t) && t) w = json_object_get_int(t);
            if (json_object_object_get_ex(value, "height", &t) && t) h = json_object_get_int(t);
            if (json_object_object_get_ex(value, "refreshHz", &t) && t) z = json_object_get_int(t);
            const char* mode = json_str(value, "mode");
            const char* conn = json_str(value, "connector");
            if (dvs_engine_set_output(s_eng, conn ? conn : "", w, h, z, mode ? mode : "force") != 0) {
                snprintf(err, errlen, "output out of range");
                return -1;
            }
            return 0;
        }
        if (scope && strcmp(scope, "role") == 0) {
            const char* role = value ? json_str(value, "role") : NULL;
            if (!role && value && json_object_is_type(value, json_type_string)) role = json_object_get_string(value);
            if (!role) { snprintf(err, errlen, "missing role"); return -1; }
            if (dvs_engine_set_role(s_eng, role) != 0) { snprintf(err, errlen, "bad role"); return -1; }
            return 0;
        }
    }
    if (dvs_layout_prepare_from_json(req) != 0) { snprintf(err, errlen, "bad layout"); return -1; }
    return 0;
}

static int hook_commit(void) {
    return dvs_layout_commit(s_eng);
}

static void hook_clear(void) {
    dvs_layout_clear(s_eng);
}

static void hook_abort(void) {
    dvs_layout_abort();
}

static void hook_counts(int* rx_sessions, int* decoding, int* layers) {
    dvs_engine_counts(s_eng, rx_sessions, decoding, layers);
}

static void hook_ptp_state(char* state, size_t n, int* offset_us) {
    if (!s_eng || !s_eng->st) return;
    uint64_t t = mtl_ptp_read_time(s_eng->st);
    if (t == 0) return;
    if (state && n > 0) snprintf(state, n, "synced");
    if (offset_us) *offset_us = 0;
}

/* Per-lane readiness snapshot. Empty array means zero admitted sessions;
 * entries join the admission table with the running compositor state. */
static json_object* hook_lane_stats(void) {
    if (!s_eng) return json_object_new_array();
    json_object* a = dvs_engine_lane_stats(s_eng);
    return a ? a : json_object_new_array();
}

void dvs_ipc_attach_engine(dvs_engine* e) {
    s_eng = e;
    dvs_ipc_engine_hooks_t h = {
        .prepare = hook_prepare,
        .commit = hook_commit,
        .clear = hook_clear,
        .abort = hook_abort,
        .counts = hook_counts,
        .ptp_state = hook_ptp_state,
        .lane_stats = hook_lane_stats,
    };
    dvs_ipc_install_hooks(&h);
}
