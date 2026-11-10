/* Engine <-> IPC bridge.
 *
 * Compiled into the real dvs_media binary only (NOT the no-MTL unit test).
 * Wires the IPC server's engine hooks to the actual media engine so the test
 * target links without MTL/MPP. */

#include "ipc/ipc_server.h"
#include "engine_ctx.h"
#include "file_playout_config.h" /* file_playout_parse_pacing (in-tree reuse) */
#include "engine/layout_executor.h"
#include "engine/session_mgr.h"
#include "engine/session_admit.h"
#include "engine/stream_validate.h"
#include "engine/capability.h"
#include "display_layout.h"
#include <time.h>
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

static int valid_ip4(const char* s) {
    if (!s || !s[0]) return 0;
    int parts = 0;
    const char* p = s;
    while (1) {
        if (*p < '0' || *p > '9') return 0;
        int v = 0, digits = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); digits++; p++; if (digits > 3) return 0; }
        if (v > 255) return 0;
        parts++;
        if (*p == '\0') break;
        if (*p != '.') return 0;
        p++;
        if (*p == '\0') return 0;
    }
    return parts == 4;
}

/* Trim in place; returns pointer to first non-space. */
static const char* trim_l(const char* s, const char** end_out) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    const char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
    if (end_out) *end_out = e;
    return s;
}

/* Validate one "ip" or "ip:port" item. */
static int valid_endpoint_item(const char* raw, char* err, size_t errlen) {
    const char* e = NULL;
    const char* s = trim_l(raw ? raw : "", &e);
    size_t len = (size_t)(e - s);
    if (len == 0 || len >= 64) { snprintf(err, errlen, "bad whitelist entry"); return -1; }
    char tmp[64];
    memcpy(tmp, s, len);
    tmp[len] = '\0';
    char* colon = strchr(tmp, ':');
    if (!colon) {
        if (!valid_ip4(tmp)) { snprintf(err, errlen, "bad whitelist entry '%s'", tmp); return -1; }
        return 0;
    }
    if (strchr(colon + 1, ':')) { snprintf(err, errlen, "bad whitelist entry '%s'", tmp); return -1; }
    *colon = '\0';
    const char* ips = trim_l(tmp, NULL);
    const char* ps = trim_l(colon + 1, NULL);
    if (!valid_ip4(ips)) { snprintf(err, errlen, "bad whitelist entry '%s'", raw); return -1; }
    if (!ps[0]) { snprintf(err, errlen, "bad whitelist entry '%s'", raw); return -1; }
    for (const char* q = ps; *q; q++) if (*q < '0' || *q > '9') {
        snprintf(err, errlen, "bad whitelist entry '%s'", raw); return -1;
    }
    int p = atoi(ps);
    if (p < 1 || p > 65535) { snprintf(err, errlen, "bad whitelist entry '%s'", raw); return -1; }
    return 0;
}

static int apply_whitelist(struct json_object* wv, char* err, size_t errlen) {
    char csv[512] = {0};
    if (!wv) { dvs_session_mgr_set_whitelist(NULL); return 0; }
    if (json_object_is_type(wv, json_type_string)) {
        const char* s = json_object_get_string(wv);
        if (!s) { snprintf(err, errlen, "empty whitelist"); return -1; }
        /* WL-1: per-item validation before accepting. */
        const char* p = s;
        int any = 0;
        while (1) {
            const char* comma = strchr(p, ',');
            size_t n = comma ? (size_t)(comma - p) : strlen(p);
            char tok[64];
            if (n >= sizeof(tok)) { snprintf(err, errlen, "bad whitelist entry"); return -1; }
            memcpy(tok, p, n);
            tok[n] = '\0';
            const char* e = NULL;
            const char* t = trim_l(tok, &e);
            if ((size_t)(e - t) > 0) {
                char one[64];
                size_t k = (size_t)(e - t);
                if (k >= sizeof(one)) { snprintf(err, errlen, "bad whitelist entry"); return -1; }
                memcpy(one, t, k);
                one[k] = '\0';
                if (valid_endpoint_item(one, err, errlen) != 0) return -1;
                any = 1;
            }
            if (!comma) break;
            p = comma + 1;
        }
        if (!any) { snprintf(err, errlen, "empty whitelist"); return -1; }
        snprintf(csv, sizeof(csv), "%s", s);
    } else if (json_object_is_type(wv, json_type_array)) {
        size_t pos = 0;
        int n = json_object_array_length(wv);
        if (n <= 0) { snprintf(err, errlen, "empty whitelist"); return -1; }
        for (int i = 0; i < n; i++) {
            struct json_object* e = json_object_array_get_idx(wv, i);
            if (!e) { snprintf(err, errlen, "bad whitelist entry"); return -1; }
            const char* s = json_object_is_type(e, json_type_string) ? json_object_get_string(e) : NULL;
            char tmp[96] = {0};
            if (s) {
                /* Element may itself be a comma list; validate each item. */
                const char* p = s;
                while (1) {
                    const char* comma = strchr(p, ',');
                    size_t nn = comma ? (size_t)(comma - p) : strlen(p);
                    char tok[64];
                    if (nn >= sizeof(tok)) { snprintf(err, errlen, "bad whitelist entry"); return -1; }
                    memcpy(tok, p, nn);
                    tok[nn] = '\0';
                    const char* ee = NULL;
                    const char* t = trim_l(tok, &ee);
                    if ((size_t)(ee - t) > 0) {
                        char one[64];
                        size_t k = (size_t)(ee - t);
                        if (k >= sizeof(one)) { snprintf(err, errlen, "bad whitelist entry"); return -1; }
                        memcpy(one, t, k);
                        one[k] = '\0';
                        if (valid_endpoint_item(one, err, errlen) != 0) return -1;
                    }
                    if (!comma) break;
                    p = comma + 1;
                }
                snprintf(tmp, sizeof(tmp), "%s", s);
            } else if (json_object_is_type(e, json_type_object)) {
                /* Canonical object form: exactly {sip, udp_port?}. The same
                 * spelling the engine emits in query_state lanes, so a lane
                 * snapshot pastes back as a whitelist verbatim. */
                const char* ip = json_str(e, "sip");
                struct json_object* pv = NULL;
                int has_p = 0, port = 0;
                if (json_object_object_get_ex(e, "udp_port", &pv) && pv) {
                    if (json_object_is_type(pv, json_type_string)) {
                        const char* ps = json_object_get_string(pv);
                        for (const char* q = ps; *q; q++) if (*q < '0' || *q > '9') {
                            snprintf(err, errlen, "bad whitelist entry '%s'", ps); return -1;
                        }
                        port = atoi(ps);
                    } else port = json_object_get_int(pv);
                    has_p = 1;
                }
                if (!ip || !ip[0] || !valid_ip4(ip)) {
                    snprintf(err, errlen, "bad whitelist entry '%s'", ip ? ip : ""); return -1;
                }
                if (has_p && (port < 1 || port > 65535)) {
                    snprintf(err, errlen, "bad whitelist entry '%s:%d'", ip, port); return -1;
                }
                if (has_p) snprintf(tmp, sizeof(tmp), "%s:%d", ip, port);
                else snprintf(tmp, sizeof(tmp), "%s", ip);
            } else { snprintf(err, errlen, "bad whitelist entry type"); return -1; }
            if (!tmp[0]) continue;
            if (pos > 0 && pos + 1 < sizeof(csv)) csv[pos++] = ',';
            size_t k = strlen(tmp);
            if (pos + k >= sizeof(csv)) break;
            memcpy(csv + pos, tmp, k);
            pos += k;
        }
        csv[pos] = '\0';
    } else if (json_object_is_type(wv, json_type_object)) {
        snprintf(err, errlen, "bad whitelist object");
        return -1;
    } else {
        snprintf(err, errlen, "bad whitelist value");
        return -1;
    }
    dvs_session_mgr_set_whitelist(csv[0] ? csv : NULL);
    return 0;
}

static struct json_object* effective_body(struct json_object* req) {
    struct json_object* p = NULL;
    if (req && json_object_object_get_ex(req, "payload", &p) && p &&
        json_object_is_type(p, json_type_object))
        return p;
    return req;
}

static int hook_prepare(struct json_object* req, char* err, size_t errlen) {
    req = effective_body(req);
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
        if (scope && strcmp(scope, "st21_pacing") == 0) {
            /* ST2110-21 TX pacing profile: applies at the next MTL init
             * (file_playout_mtl_init_dual reads cfg.st21_pacing). */
            const char* pacing = value ? json_str(value, "st21_pacing") : NULL;
            if (!pacing && value && json_object_is_type(value, json_type_string))
                pacing = json_object_get_string(value);
            file_playout_pacing way;
            if (!pacing || file_playout_parse_pacing(pacing, &way) != 0) {
                snprintf(err, errlen, "bad st21_pacing (auto|tsc|tsc_narrow|rl)");
                return -1;
            }
            if (s_eng && s_eng->st) {
                snprintf(err, errlen, "st21_pacing: MTL already initialized, clear first");
                return -1;
            }
            if (s_eng) s_eng->cfg.st21_pacing = way;
            return 0;
        }
    }
    /* Layout staging: only validate when a recognizable layout body is present.
     * Pure operation frames (operationId/revision/applyAtNs only) pass through
     * so the barrier can close without touching the picture. The platform
     * windows/geometry shape is no longer silently accepted: since the agent
     * translates to streams, a windows-shape prepare means a pre-translation
     * agent — refuse it loudly instead of staging nothing. Streams are
     * validated here (whitelist + shape) so prepare fails fast with 400 and
     * never stages a layout the commit could only half-apply. */
    {
        struct json_object* t = NULL;
        if (req && json_object_object_get_ex(req, "streams", &t)) {
            struct json_object* exv = NULL;
            if (json_object_object_get_ex(req, "expiresAtTaiNs", &exv) && exv &&
                json_object_is_type(exv, json_type_int)) {
                int64_t expires = json_object_get_int64(exv);
                if (expires > 0) {
                    struct timespec now_ts;
                    clock_gettime(CLOCK_REALTIME, &now_ts);
                    int64_t now_ns = (int64_t)now_ts.tv_sec * 1000000000LL +
                                     (int64_t)now_ts.tv_nsec;
                    if (now_ns > expires) {
                        snprintf(err, errlen, "expired");
                        return -1;
                    }
                }
            }
            file_playout_pip_stream vs[MP4_PIP_MAX_LANES];
            int vn = 0;
            if (dvs_layout_from_json(req, vs, &vn) != 0) {
                snprintf(err, errlen, "bad layout");
                return -1;
            }
            for (int i = 0; i < vn; i++) {
                if (!vs[i].sip[0] || !vs[i].udp_port || vs[i].width <= 0 ||
                    vs[i].height <= 0 || vs[i].fps <= 0.0f) {
                    snprintf(err, errlen, "bad stream %d", i);
                    return -1;
                }
                {
                    char cerr[128] = {0};
                    if (dvs_stream_src_valid(vs[i].src_ip, cerr, sizeof(cerr)) != 0) {
                        snprintf(err, errlen, "stream %d: %s", i, cerr);
                        return -1;
                    }
                    if (dvs_stream_crop_valid(vs[i].width, vs[i].height,
                                              vs[i].src_x, vs[i].src_y,
                                              vs[i].src_w, vs[i].src_h,
                                              cerr, sizeof(cerr)) != 0) {
                        snprintf(err, errlen, "stream %d: %s", i, cerr);
                        return -1;
                    }
                }
                if (!dvs_session_whitelist_check(vs[i].sip, vs[i].udp_port)) {
                    snprintf(err, errlen, "source %s:%u not whitelisted",
                             vs[i].sip, (unsigned)vs[i].udp_port);
                    return -1;
                }
            }
            /* Prepare-time resource reservation: admit every staged source
             * now so an over-budget or rejected layout fails fast in the
             * prepare worker (blocked barrier) instead of at commit time.
             * The batch is all-or-nothing; commit re-admits idempotently
             * and abort releases what was reserved here. */
            {
                char ips[MP4_PIP_MAX_LANES][DVS_SESSION_KEY_LEN];
                uint16_t ports[MP4_PIP_MAX_LANES];
                for (int i = 0; i < vn; i++) {
                    snprintf(ips[i], sizeof(ips[i]), "%s", vs[i].sip);
                    ports[i] = vs[i].udp_port;
                }
                if (dvs_session_reserve_batch(ips, ports, vn, NULL) != 0) {
                    snprintf(err, errlen, "prepare admission failed");
                    return -1;
                }
            }
            /* Stage via the layout executor so applyAtNs/audioOwner routing
             * stay in the pending shadow; commit applies them atomically. */
            if (dvs_layout_prepare_from_json(req) != 0) {
                snprintf(err, errlen, "bad layout");
                return -1;
            }
            return 0;
        }
        if (req && json_object_object_get_ex(req, "windows", &t) && t) {
            snprintf(err, errlen, "unsupported layout shape: agent must translate windows to streams");
            return -1;
        }
    }
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
    if (s_eng->cfg.ptp_mode == FILE_PLAYOUT_PTP_MONOTONIC) {
        if (state && n > 0) snprintf(state, n, "unsynced");
        if (offset_us) *offset_us = 0;
        return;
    }
    uint64_t t = mtl_ptp_read_time(s_eng->st);
    if (t == 0) {
        dvs_ptp_state_from_delta(state, n, offset_us, 0, 0);
        return;
    }
    struct timespec ts;
#ifdef CLOCK_TAI
    clock_gettime(CLOCK_TAI, &ts);
#else
    clock_gettime(CLOCK_REALTIME, &ts);
#endif
    uint64_t tai = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    int64_t d = (int64_t)(t - tai);
    dvs_ptp_state_from_delta(state, n, offset_us, 1, (long long)d);
}

/* Per-lane readiness snapshot. Empty array means zero admitted sessions;
 * entries join the admission table with the running compositor state. */
static json_object* hook_lane_stats(void) {
    if (!s_eng) return json_object_new_array();
    json_object* a = dvs_engine_lane_stats(s_eng);
    return a ? a : json_object_new_array();
}

static int hook_tx_start(struct json_object* req, char* err, size_t errlen) {
    req = effective_body(req);
    if (!s_eng) {
        if (err && errlen > 0) snprintf(err, errlen, "engine not attached");
        return -1;
    }
    dvs_tx_params_t p;
    memset(&p, 0, sizeof(p));
    const char* port = json_str(req, "port");
    struct json_object* enc = NULL;
    struct json_object* tgt = NULL;
    json_object_object_get_ex(req, "encode", &enc);
    json_object_object_get_ex(req, "target", &tgt);
    const char* codec = enc ? json_str(enc, "codec") : NULL;
    if (port) snprintf(p.port, sizeof(p.port), "%s", port);
    if (codec) snprintf(p.codec, sizeof(p.codec), "%s", codec);
    struct json_object* v = NULL;
    if (enc && json_object_object_get_ex(enc, "width", &v) && v)
        p.width = json_object_get_int(v);
    if (enc && json_object_object_get_ex(enc, "height", &v) && v)
        p.height = json_object_get_int(v);
    if (enc && json_object_object_get_ex(enc, "fps", &v) && v)
        p.fps = json_object_get_int(v);
    if (enc && json_object_object_get_ex(enc, "bitrateKbps", &v) && v)
        p.bitrate_kbps = json_object_get_int(v);
    if (enc && json_object_object_get_ex(enc, "gopMs", &v) && v)
        p.gop_ms = json_object_get_int(v);
    const char* sip = tgt ? json_str(tgt, "sip") : NULL;
    if (sip) snprintf(p.target_sip, sizeof(p.target_sip), "%s", sip);
    if (tgt && json_object_object_get_ex(tgt, "udpPort", &v) && v)
        p.target_port = json_object_get_int(v);
    if (tgt && json_object_object_get_ex(tgt, "audioUdpPort", &v) && v)
        p.audio_port = json_object_get_int(v);
    if (!p.port[0] || !p.codec[0] || !p.width || !p.height || !p.fps ||
        !p.bitrate_kbps || !p.target_sip[0] || !p.target_port) {
        if (err && errlen > 0)
            snprintf(err, errlen, "incomplete tx params (port+encode+target)");
        return -1;
    }
    return dvs_engine_tx_start(s_eng, &p, err, errlen);
}

static void hook_tx_stop(void) {
    if (s_eng) dvs_engine_tx_stop(s_eng);
}

static json_object* hook_tx_status(void) {
    if (!s_eng) return NULL;
    return dvs_engine_tx_status(s_eng);
}

static const char* hook_mtl_version(void) {
    if (!s_eng || !s_eng->st) return NULL;
    return mtl_version();
}

static const char* hook_backend(void) {
    if (!s_eng) return "unknown";
    const char* tx = s_eng->cfg.tx_port[0] ? s_eng->cfg.tx_port : NULL;
    const char* rx = s_eng->cfg.rx_port[0] ? s_eng->cfg.rx_port : NULL;
    if (tx && rx && strcmp(tx, rx) != 0) {
        /* Dual ports with different prefixes cannot be one backend string. */
        return "unknown";
    }
    const char* pick = tx ? tx : rx;
    if (!pick) return "unknown";
    return dvs_capability_backend_name(pick);
}

static json_object* hook_audio_status(void) {
    if (!s_eng) return NULL;
    return dvs_engine_audio_status(s_eng);
}

static json_object* hook_commit_info(void) {
    if (!s_eng) return NULL;
    return dvs_engine_commit_report(s_eng);
}

static const char* hook_commit_error(void) {
    if (!s_eng) return "";
    return dvs_engine_last_fail(s_eng);
}

static json_object* hook_display_status(void) {
    if (!s_eng) return NULL;
    return dvs_engine_display_status(s_eng);
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
        .mtl_version = hook_mtl_version,
        .backend = hook_backend,
        .lane_stats = hook_lane_stats,
        .audio_status = hook_audio_status,
        .commit_info = hook_commit_info,
        .commit_error = hook_commit_error,
        .display_status = hook_display_status,
        .tx_start = hook_tx_start,
        .tx_stop = hook_tx_stop,
        .tx_status = hook_tx_status,
    };
    dvs_ipc_install_hooks(&h);
}
