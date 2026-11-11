#include "engine_ctx.h"
#include "engine/session_admit.h"
#include "display_layout.h" /* MP4_PIP_MAX_LANES */

#include <json-c/json.h>

#include <pthread.h>
#include <string.h>
#include <time.h>

/* layout_executor — two-phase prepare/commit with a shadow (pending) layout.
 *
 * prepare(): validate + stash the desired layout (does not touch the running
 *            compositor — current picture stays).
 * commit() : swap the running layout to the pending one (engine recreates the
 *            compositor at the staged frame anchor).
 * abort()  : discard the pending layout (rollback to current). */

static file_playout_pip_stream g_pending[MP4_PIP_MAX_LANES];
static uint64_t g_pending_apply_at_ns = 0;
static uint64_t g_pending_expires_ns = 0;
static int g_pending_count = 0;
static dvs_frame_anchor_t g_pending_frame;
static dvs_base_image_desc_t g_pending_base;
static dvs_image_adjust_t g_pending_adjust;
/* Set only when a prepare actually staged a layout. A refused prepare (e.g.
 * the pre-translation windows shape, rejected by hook_prepare since the agent
 * translates to streams) stages nothing and leaves this clear, so commit
 * keeps the running picture instead of clearing media. */
static int g_pending_valid = 0;
static char g_pending_audio_owner[64];
static int g_pending_audio_volume = 0;
static pthread_mutex_t g_pending_mu = PTHREAD_MUTEX_INITIALIZER;

/* Wall-clock expiry for the control-plane barrier. Monotonic media time
 * cannot judge it, so realtime is used here while applyAt keeps its media
 * clock. Expired work is refused at prepare and rechecked at commit. */
static uint64_t realtime_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Parse a "streams" JSON array into pip_stream structs. Keys are the exact
 * camelCase spellings the agent emits; anything else is rejected rather
 * than guessed. Returns 0 on success. */
int dvs_layout_from_json(struct json_object* req, file_playout_pip_stream* out,
                         int* count) {
    if (!req || !out || !count) return -1;
    struct json_object* arr = NULL;
    if (!json_object_object_get_ex(req, "streams", &arr))
        return -1;
    if (!json_object_is_type(arr, json_type_array)) return -1;

    int n = json_object_array_length(arr);
    /* Over-budget layouts fail loudly instead of being silently truncated:
     * the platform barrier already caps windows per node, so reaching here
     * means a conversion bug that must surface as a 400, never as a
     * partial wall. */
    if (n > MP4_PIP_MAX_LANES) return -1;
    int any_lane = 0;
    for (int i = 0; i < n; i++) {
        struct json_object* s = json_object_array_get_idx(arr, i);
        file_playout_pip_stream* p = &out[i];
        memset(p, 0, sizeof(*p));
        struct json_object* v;
        if (json_object_object_get_ex(s, "lane", &v)) {
            p->lane = json_object_get_int(v);
            any_lane = 1;
        }
        if (json_object_object_get_ex(s, "main", &v)) p->main = json_object_get_boolean(v);
        if (json_object_object_get_ex(s, "sip", &v))
            strncpy(p->sip, json_object_get_string(v), MP4_PIP_SIP_LEN - 1);
        if (json_object_object_get_ex(s, "srcIp", &v) && v &&
            json_object_is_type(v, json_type_string))
            strncpy(p->src_ip, json_object_get_string(v), MP4_PIP_SIP_LEN - 1);
        if (json_object_object_get_ex(s, "udpPort", &v))
            p->udp_port = (uint16_t)json_object_get_int(v);
        if (json_object_object_get_ex(s, "audioUdpPort", &v))
            p->audio_udp_port = (uint16_t)json_object_get_int(v);
        if (json_object_object_get_ex(s, "width", &v)) p->width = json_object_get_int(v);
        if (json_object_object_get_ex(s, "height", &v)) p->height = json_object_get_int(v);
        if (json_object_object_get_ex(s, "fps", &v)) p->fps = (float)json_object_get_double(v);
        if (json_object_object_get_ex(s, "x", &v)) p->x = json_object_get_int(v);
        if (json_object_object_get_ex(s, "y", &v)) p->y = json_object_get_int(v);
        if (json_object_object_get_ex(s, "w", &v)) p->w = json_object_get_int(v);
        if (json_object_object_get_ex(s, "h", &v)) p->h = json_object_get_int(v);
        /* Source crop (forward-compatible; see display_layout.h). Zero w/h
         * stays "full frame". */
        if (json_object_object_get_ex(s, "srcX", &v)) p->src_x = json_object_get_int(v);
        if (json_object_object_get_ex(s, "srcY", &v)) p->src_y = json_object_get_int(v);
        if (json_object_object_get_ex(s, "srcW", &v)) p->src_w = json_object_get_int(v);
        if (json_object_object_get_ex(s, "srcH", &v)) p->src_h = json_object_get_int(v);
        if (json_object_object_get_ex(s, "codestreamSize", &v))
            p->codestream_size = (size_t)json_object_get_int64(v);
    }
    if (!any_lane) {
        for (int i = 0; i < n; i++) out[i].lane = i;
    }
    *count = n;
    return 0;
}

int dvs_layout_prepare(const file_playout_pip_stream* streams, int count) {
    if (count < 0 || count > MP4_PIP_MAX_LANES) return -1;
    pthread_mutex_lock(&g_pending_mu);
    memcpy(g_pending, streams, (size_t)count * sizeof(*streams));
    g_pending_count = count;
    g_pending_apply_at_ns = 0;
    g_pending_expires_ns = 0;
    memset(&g_pending_frame, 0, sizeof(g_pending_frame));
    g_pending_frame.frame_index = -1;
    memset(&g_pending_base, 0, sizeof(g_pending_base));
    memset(&g_pending_adjust, 0, sizeof(g_pending_adjust));
    g_pending_valid = 1;
    pthread_mutex_unlock(&g_pending_mu);
    return 0;
}

uint64_t dvs_layout_pending_apply_at_ns(void) {
    uint64_t v;
    pthread_mutex_lock(&g_pending_mu);
    v = g_pending_apply_at_ns;
    pthread_mutex_unlock(&g_pending_mu);
    return v;
}

int dvs_layout_prepare_from_json(struct json_object* req) {
    file_playout_pip_stream buf[MP4_PIP_MAX_LANES];
    int n = 0;
    if (dvs_layout_from_json(req, buf, &n) != 0) return -1;
    /* An explicit empty array is not a clear-screen: clear has its own verb.
     * The barrier close without a streams key never reaches here. */
    if (n <= 0) return -1;

    /* optional audio routing carried on the same request */
    char owner[64] = "";
    int volume = 0;
    struct json_object* v;
    if (req && json_object_object_get_ex(req, "audioOwner", &v))
        strncpy(owner, json_object_get_string(v), sizeof(owner) - 1);
    if (req && json_object_object_get_ex(req, "audioVolume", &v))
        volume = json_object_get_int(v);

    /* Frame anchor (optional): epoch/fps/frameIndex travel the same path as
     * applyAtNs. Absent means a legacy payload: commit at the current frame. */
    dvs_frame_anchor_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.frame_index = -1;
    frame.fps = 60.0; /* legacy wall rate when the payload names no fps */
    {
        struct json_object* e = NULL, *f = NULL, *i = NULL;
        if (req && json_object_object_get_ex(req, "epochTaiNs", &e) && e)
            frame.epoch_ns = (uint64_t)json_object_get_int64(e);
        if (req && json_object_object_get_ex(req, "fps", &f) && f) {
            if (json_object_is_type(f, json_type_double))
                frame.fps = json_object_get_double(f);
            else
                frame.fps = (double)json_object_get_int(f);
        }
        if (req && json_object_object_get_ex(req, "frameIndex", &i) && i)
            frame.frame_index = json_object_get_int64(i);
    }
    /* Wall still (optional): the node fetches the bytes from url itself. */
    dvs_base_image_desc_t base;
    memset(&base, 0, sizeof(base));
    {
        struct json_object* b = NULL;
        if (req && json_object_object_get_ex(req, "baseImage", &b) && b &&
            json_object_is_type(b, json_type_object)) {
            struct json_object* q = NULL;
            if (json_object_object_get_ex(b, "revision", &q) && q)
                base.revision = json_object_get_int(q);
            const char* u = NULL;
            if (json_object_object_get_ex(b, "url", &q) && q &&
                json_object_is_type(q, json_type_string))
                u = json_object_get_string(q);
            if (u) strncpy(base.url, u, sizeof(base.url) - 1);
            const char* pth = NULL;
            if (json_object_object_get_ex(b, "path", &q) && q &&
                json_object_is_type(q, json_type_string))
                pth = json_object_get_string(q);
            if (pth) strncpy(base.path, pth, sizeof(base.path) - 1);
            if (json_object_object_get_ex(b, "width", &q) && q)
                base.img_w = json_object_get_int(q);
            if (json_object_object_get_ex(b, "height", &q) && q)
                base.img_h = json_object_get_int(q);
            struct json_object* s = NULL;
            if (json_object_object_get_ex(b, "sub", &s) && s &&
                json_object_is_type(s, json_type_object)) {
                if (json_object_object_get_ex(s, "x", &q) && q)
                    base.sub_x = json_object_get_int(q);
                if (json_object_object_get_ex(s, "y", &q) && q)
                    base.sub_y = json_object_get_int(q);
                if (json_object_object_get_ex(s, "w", &q) && q)
                    base.sub_w = json_object_get_int(q);
                if (json_object_object_get_ex(s, "h", &q) && q)
                    base.sub_h = json_object_get_int(q);
            }
        }
    }
    /* Image adjustment targets (optional): stored pending, applied by the
     * display backend when supported, else reported as unsupported. */
    dvs_image_adjust_t adjust;
    memset(&adjust, 0, sizeof(adjust));
    {
        struct json_object* a = NULL;
        if (req && json_object_object_get_ex(req, "imageAdjust", &a) && a &&
            json_object_is_type(a, json_type_object)) {
            static const char* keys[4] = {"brightness", "contrast",
                                          "saturation", "hue"};
            int* vals[4] = {&adjust.brightness, &adjust.contrast,
                            &adjust.saturation, &adjust.hue};
            for (int k = 0; k < 4; k++) {
                struct json_object* q = NULL;
                if (json_object_object_get_ex(a, keys[k], &q) && q) {
                    *vals[k] = json_object_get_int(q);
                    adjust.has_value = 1;
                }
            }
        }
    }
    pthread_mutex_lock(&g_pending_mu);
    memcpy(g_pending, buf, (size_t)n * sizeof(*buf));
    g_pending_count = n;
    /* The switch time travels as applyAtNs from platform through agent. */
    uint64_t apply_at = 0;
    struct json_object* atv = NULL;
    if (req && json_object_object_get_ex(req, "applyAtNs", &atv) && atv)
      apply_at = (uint64_t)json_object_get_int64(atv);
    uint64_t expires = 0;
    struct json_object* exv = NULL;
    if (req && json_object_object_get_ex(req, "expiresAtTaiNs", &exv) && exv)
      expires = (uint64_t)json_object_get_int64(exv);
    if (expires > 0 && realtime_now_ns() > expires) {
        pthread_mutex_unlock(&g_pending_mu);
        return -1;
    }
    g_pending_apply_at_ns = apply_at;
    g_pending_expires_ns = expires;
    g_pending_frame = frame;
    g_pending_base = base;
    g_pending_adjust = adjust;
    g_pending_valid = 1;
    strncpy(g_pending_audio_owner, owner, sizeof(g_pending_audio_owner) - 1);
    g_pending_audio_owner[sizeof(g_pending_audio_owner) - 1] = '\0';
    g_pending_audio_volume = volume;
    pthread_mutex_unlock(&g_pending_mu);
    return 0;
}

void dvs_layout_pending_frame(dvs_frame_anchor_t* out) {
    if (!out) return;
    pthread_mutex_lock(&g_pending_mu);
    *out = g_pending_frame;
    pthread_mutex_unlock(&g_pending_mu);
}

void dvs_layout_pending_base(dvs_base_image_desc_t* out) {
    if (!out) return;
    pthread_mutex_lock(&g_pending_mu);
    *out = g_pending_base;
    pthread_mutex_unlock(&g_pending_mu);
}

void dvs_layout_pending_adjust(dvs_image_adjust_t* out) {
    if (!out) return;
    pthread_mutex_lock(&g_pending_mu);
    *out = g_pending_adjust;
    pthread_mutex_unlock(&g_pending_mu);
}

int dvs_layout_commit(dvs_engine* e) {
    file_playout_pip_stream buf[MP4_PIP_MAX_LANES];
    int n = 0;
    char owner[64];
    int volume;
    uint64_t apply_at;
    dvs_frame_anchor_t frame;
    dvs_base_image_desc_t base;
    dvs_image_adjust_t adjust;
    pthread_mutex_lock(&g_pending_mu);
    n = g_pending_count;
    int valid = g_pending_valid;
    /* Keep the staged layout on failure so a retry re-attempts the same
     * picture instead of no-op succeeding; only success consumes it. */
    uint64_t expires = g_pending_expires_ns;
    memcpy(buf, g_pending, (size_t)n * sizeof(*buf));
    memcpy(owner, g_pending_audio_owner, sizeof(owner));
    volume = g_pending_audio_volume;
    apply_at = g_pending_apply_at_ns;
    frame = g_pending_frame;
    base = g_pending_base;
    adjust = g_pending_adjust;
    pthread_mutex_unlock(&g_pending_mu);

    if (!valid) {
        /* no layout was staged: keep the running picture */
        return 0;
    }
    if (expires > 0 && realtime_now_ns() > expires) {
        if (e) dvs_engine_note_fail(e, "EXPIRED");
        return -1;
    }

    /* push audio routing to the engine before (re)building the compositor */
    dvs_engine_set_audio(e, owner, volume);
    /* still + adjustment ride the same commit; the engine resolves them
     * after the layout swap so a failed swap keeps the old picture. */
    dvs_engine_set_pending_base(e, &base);
    dvs_engine_set_pending_adjust(e, &adjust);
    int rc = dvs_engine_apply_layout_frame(e, buf, n, apply_at,
                                            frame.epoch_ns, frame.fps,
                                            frame.frame_index);
    if (rc == 0) {
        pthread_mutex_lock(&g_pending_mu);
        g_pending_valid = 0;
        g_pending_count = 0;
        g_pending_apply_at_ns = 0;
        g_pending_expires_ns = 0;
        memset(&g_pending_frame, 0, sizeof(g_pending_frame));
        g_pending_frame.frame_index = -1;
        memset(&g_pending_base, 0, sizeof(g_pending_base));
        memset(&g_pending_adjust, 0, sizeof(g_pending_adjust));
        pthread_mutex_unlock(&g_pending_mu);
    }
    return rc;
}

void dvs_layout_clear(dvs_engine* e) {
    /* Clear-screen keeps the platform backdrop: media planes go to zero
     * windows while the decoded base image (or black when no still is set)
     * stays on screen. The compositor is rebuilt with zero lanes instead of
     * being freed so the backdrop plane keeps scanning out; MTL stays up
     * for fast future commits. The empty layout carries no frame anchor:
     * frameIndex -1 commits at the current frame (see engine_ctx). */
    dvs_engine_apply_layout_frame(e, NULL, 0, 0, 0, 60.0, -1);
    pthread_mutex_lock(&g_pending_mu);
    g_pending_count = 0;
    g_pending_apply_at_ns = 0;
    g_pending_expires_ns = 0;
    g_pending_valid = 0;
    g_pending_audio_owner[0] = '\0';
    g_pending_audio_volume = 0;
    pthread_mutex_unlock(&g_pending_mu);
}

void dvs_layout_abort_except(const file_playout_pip_stream* live, int live_n) {
    /* Release what this prepare reserved, minus whatever is still live. */
    file_playout_pip_stream staged[MP4_PIP_MAX_LANES];
    int n = 0;
    pthread_mutex_lock(&g_pending_mu);
    n = g_pending_count;
    if (n < 0) n = 0;
    if (n > MP4_PIP_MAX_LANES) n = MP4_PIP_MAX_LANES;
    if (n > 0) memcpy(staged, g_pending, (size_t)n * sizeof(*staged));
    g_pending_count = 0;
    g_pending_apply_at_ns = 0;
    g_pending_expires_ns = 0;
    g_pending_valid = 0;
    memset(&g_pending_frame, 0, sizeof(g_pending_frame));
    g_pending_frame.frame_index = -1;
    memset(&g_pending_base, 0, sizeof(g_pending_base));
    memset(&g_pending_adjust, 0, sizeof(g_pending_adjust));
    g_pending_audio_owner[0] = '\0';
    g_pending_audio_volume = 0;
    pthread_mutex_unlock(&g_pending_mu);
    for (int i = 0; i < n; i++) {
        if (!staged[i].sip[0]) continue;
        int keep = 0;
        if (live && live_n > 0) {
            for (int j = 0; j < live_n; j++) {
                if (live[j].sip[0] &&
                    strcmp(staged[i].sip, live[j].sip) == 0 &&
                    staged[i].udp_port == live[j].udp_port) {
                    keep = 1;
                    break;
                }
            }
        }
        if (!keep) dvs_session_release(staged[i].sip, staged[i].udp_port);
    }
}

void dvs_layout_abort(void) {
    dvs_layout_abort_except(NULL, 0);
}
