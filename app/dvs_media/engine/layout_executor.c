#include "engine_ctx.h"
#include "shared/display_layout.h" /* MP4_PIP_MAX_LANES */

#include <json-c/json.h>

#include <pthread.h>
#include <string.h>

/* layout_executor — two-phase prepare/commit with a shadow (pending) layout.
 *
 * prepare(): validate + stash the desired layout (does not touch the running
 *            compositor — current picture stays).
 * commit() : swap the running layout to the pending one (engine recreates the
 *            compositor).
 * abort()  : discard the pending layout (rollback to current).
 *
 * NOTE: this is the pragmatic first implementation. A future refinement adds
 * a frame-N vblank atomic plane switch and rollback from a saved shadow copy on
 * commit failure, instead of full compositor recreation. */

static file_playout_pip_stream g_pending[MP4_PIP_MAX_LANES];
static uint64_t g_pending_apply_at_ns = 0;
static int g_pending_count = 0;
static char g_pending_audio_owner[64];
static int g_pending_audio_volume = 0;
static pthread_mutex_t g_pending_mu = PTHREAD_MUTEX_INITIALIZER;

/* Parse a "streams" (or "layout") JSON array into pip_stream structs.
 * Accepts camelCase or snake_case keys. Returns 0 on success. */
int dvs_layout_from_json(struct json_object* req, file_playout_pip_stream* out,
                         int* count) {
    if (!req || !out || !count) return -1;
    struct json_object* arr = NULL;
    if (!json_object_object_get_ex(req, "streams", &arr) &&
        !json_object_object_get_ex(req, "layout", &arr))
        return -1;
    if (!json_object_is_type(arr, json_type_array)) return -1;

    int n = json_object_array_length(arr);
    if (n > MP4_PIP_MAX_LANES) n = MP4_PIP_MAX_LANES;
    for (int i = 0; i < n; i++) {
        struct json_object* s = json_object_array_get_idx(arr, i);
        file_playout_pip_stream* p = &out[i];
        memset(p, 0, sizeof(*p));
        struct json_object* v;
        if (json_object_object_get_ex(s, "lane", &v)) p->lane = json_object_get_int(v);
        if (json_object_object_get_ex(s, "main", &v)) p->main = json_object_get_boolean(v);
        if (json_object_object_get_ex(s, "sip", &v))
            strncpy(p->sip, json_object_get_string(v), MP4_PIP_SIP_LEN - 1);
        if (json_object_object_get_ex(s, "udpPort", &v))
            p->udp_port = (uint16_t)json_object_get_int(v);
        else if (json_object_object_get_ex(s, "udp_port", &v))
            p->udp_port = (uint16_t)json_object_get_int(v);
        if (json_object_object_get_ex(s, "audioUdpPort", &v))
            p->audio_udp_port = (uint16_t)json_object_get_int(v);
        else if (json_object_object_get_ex(s, "audio_udp_port", &v))
            p->audio_udp_port = (uint16_t)json_object_get_int(v);
        if (json_object_object_get_ex(s, "width", &v)) p->width = json_object_get_int(v);
        if (json_object_object_get_ex(s, "height", &v)) p->height = json_object_get_int(v);
        if (json_object_object_get_ex(s, "fps", &v)) p->fps = (float)json_object_get_double(v);
        if (json_object_object_get_ex(s, "x", &v)) p->x = json_object_get_int(v);
        if (json_object_object_get_ex(s, "y", &v)) p->y = json_object_get_int(v);
        if (json_object_object_get_ex(s, "w", &v)) p->w = json_object_get_int(v);
        if (json_object_object_get_ex(s, "h", &v)) p->h = json_object_get_int(v);
        if (json_object_object_get_ex(s, "codestreamSize", &v))
            p->codestream_size = (size_t)json_object_get_int64(v);
    }
    *count = n;
    return 0;
}

static uint64_t json_u64(struct json_object* o, const char* a, const char* b) {
    struct json_object* v = NULL;
    if (o && json_object_object_get_ex(o, a, &v) && v)
        return (uint64_t)json_object_get_int64(v);
    if (o && b && json_object_object_get_ex(o, b, &v) && v)
        return (uint64_t)json_object_get_int64(v);
    return 0;
}

int dvs_layout_prepare(const file_playout_pip_stream* streams, int count) {
    if (count < 0 || count > MP4_PIP_MAX_LANES) return -1;
    pthread_mutex_lock(&g_pending_mu);
    memcpy(g_pending, streams, (size_t)count * sizeof(*streams));
    g_pending_count = count;
    g_pending_apply_at_ns = 0;
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

    /* optional audio routing carried on the same request */
    char owner[64] = "";
    int volume = 0;
    struct json_object* v;
    if (req && json_object_object_get_ex(req, "audioOwner", &v))
        strncpy(owner, json_object_get_string(v), sizeof(owner) - 1);
    if (req && json_object_object_get_ex(req, "audioVolume", &v))
        volume = json_object_get_int(v);

    pthread_mutex_lock(&g_pending_mu);
    memcpy(g_pending, buf, (size_t)n * sizeof(*buf));
    g_pending_count = n;
    g_pending_apply_at_ns = json_u64(req, "applyAt", "apply_at");
    strncpy(g_pending_audio_owner, owner, sizeof(g_pending_audio_owner) - 1);
    g_pending_audio_owner[sizeof(g_pending_audio_owner) - 1] = '\0';
    g_pending_audio_volume = volume;
    pthread_mutex_unlock(&g_pending_mu);
    return 0;
}

int dvs_layout_commit(dvs_engine* e) {
    file_playout_pip_stream buf[MP4_PIP_MAX_LANES];
    int n = 0;
    char owner[64];
    int volume;
    uint64_t apply_at;
    pthread_mutex_lock(&g_pending_mu);
    n = g_pending_count;
    memcpy(buf, g_pending, (size_t)n * sizeof(*buf));
    memcpy(owner, g_pending_audio_owner, sizeof(owner));
    volume = g_pending_audio_volume;
    apply_at = g_pending_apply_at_ns;
    pthread_mutex_unlock(&g_pending_mu);

    /* push audio routing to the engine before (re)building the compositor */
    dvs_engine_set_audio(e, owner, volume);
    int rc = dvs_engine_apply_layout_at(e, buf, n, apply_at);
    if (rc == 0) {
        pthread_mutex_lock(&g_pending_mu);
        g_pending_count = 0;
        g_pending_apply_at_ns = 0;
        pthread_mutex_unlock(&g_pending_mu);
    }
    return rc;
}

void dvs_layout_clear(dvs_engine* e) {
    dvs_engine_apply_layout(e, NULL, 0);
    pthread_mutex_lock(&g_pending_mu);
    g_pending_count = 0;
    g_pending_apply_at_ns = 0;
    g_pending_audio_owner[0] = '\0';
    g_pending_audio_volume = 0;
    pthread_mutex_unlock(&g_pending_mu);
}

void dvs_layout_abort(void) {
    pthread_mutex_lock(&g_pending_mu);
    g_pending_count = 0;
    g_pending_apply_at_ns = 0;
    g_pending_audio_owner[0] = '\0';
    g_pending_audio_volume = 0;
    pthread_mutex_unlock(&g_pending_mu);
}
