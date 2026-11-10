/* Unit test for engine/tx_session.c (no MTL/MPP dependency). */
#include "../engine/tx_session.h"

#include <json-c/json.h>

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (cond) {                                                           \
            printf("PASS: %s\n", name);                                       \
        } else {                                                              \
            printf("FAIL: %s\n", name);                                       \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static void good_params(dvs_tx_params_t* p) {
    memset(p, 0, sizeof(*p));
    snprintf(p->port, sizeof(p->port), "HDMI1");
    snprintf(p->codec, sizeof(p->codec), "h264");
    p->width = 1920;
    p->height = 1080;
    p->fps = 30;
    p->bitrate_kbps = 4000;
    snprintf(p->target_sip, sizeof(p->target_sip), "239.0.0.1");
    p->target_port = 5000;
}

static int fake_backend_ok(const dvs_tx_params_t* p, char* err, size_t n) {
    (void)p;
    (void)err;
    (void)n;
    return 0;
}

static int fake_backend_fail(const dvs_tx_params_t* p, char* err, size_t n) {
    (void)p;
    snprintf(err, n, "mpp busy");
    return -1;
}

static int fake_backend_positive(const dvs_tx_params_t* p, char* err, size_t n) {
    (void)p;
    (void)err;
    (void)n;
    return 1;
}

static const char* jstr(struct json_object* o, const char* k) {
    struct json_object* v = NULL;
    if (o && json_object_object_get_ex(o, k, &v) && v)
        return json_object_get_string(v);
    return NULL;
}

int main(void) {
    char err[256];
    dvs_tx_session_t s;
    dvs_tx_params_t p;

    /* validate: good params pass */
    good_params(&p);
    CHECK(dvs_tx_validate(&p, err, sizeof(err)) == 0, "validate good params");

    /* validate: bad codec / range / target rejected */
    good_params(&p);
    snprintf(p.codec, sizeof(p.codec), "vp9");
    CHECK(dvs_tx_validate(&p, err, sizeof(err)) != 0, "validate rejects codec");
    good_params(&p);
    p.width = 8192;
    CHECK(dvs_tx_validate(&p, err, sizeof(err)) != 0, "validate rejects width");
    /* GOP interval: 0 = default, 250..4000 explicit; audio port optional. */
    good_params(&p);
    p.gop_ms = 500;
    p.audio_port = 6000;
    CHECK(dvs_tx_validate(&p, err, sizeof(err)) == 0,
          "validate accepts gopMs + audio port");
    good_params(&p);
    p.gop_ms = 100;
    CHECK(dvs_tx_validate(&p, err, sizeof(err)) != 0,
          "validate rejects short gop");
    good_params(&p);
    p.gop_ms = 99999;
    CHECK(dvs_tx_validate(&p, err, sizeof(err)) != 0,
          "validate rejects long gop");
    good_params(&p);
    p.audio_port = 70000;
    CHECK(dvs_tx_validate(&p, err, sizeof(err)) != 0,
          "validate rejects audio port");
    good_params(&p);
    p.fps = 120;
    CHECK(dvs_tx_validate(&p, err, sizeof(err)) != 0, "validate rejects fps");
    good_params(&p);
    p.bitrate_kbps = 100;
    CHECK(dvs_tx_validate(&p, err, sizeof(err)) != 0, "validate rejects bitrate");
    good_params(&p);
    snprintf(p.target_sip, sizeof(p.target_sip), "not-an-ip");
    CHECK(dvs_tx_validate(&p, err, sizeof(err)) != 0, "validate rejects target ip");
    good_params(&p);
    p.target_port = 70000;
    CHECK(dvs_tx_validate(&p, err, sizeof(err)) != 0, "validate rejects target port");
    good_params(&p);
    p.port[0] = '\0';
    CHECK(dvs_tx_validate(&p, err, sizeof(err)) != 0, "validate rejects empty port");

    /* role gate: output role refused with explicit mismatch */
    dvs_tx_init(&s);
    good_params(&p);
    CHECK(dvs_tx_start(&s, &p, "output", err, sizeof(err)) != 0,
          "tx_start refused on output role");
    CHECK(s.state == DVS_TX_FAILED, "role mismatch leaves failed state");
    CHECK(strstr(err, "TX_ROLE_MISMATCH") != NULL, "role mismatch is explicit");

    /* no backend: configured + pending, never running */
    dvs_tx_init(&s);
    good_params(&p);
    CHECK(dvs_tx_start(&s, &p, "input", err, sizeof(err)) == 0,
          "tx_start without backend stores params");
    CHECK(s.state == DVS_TX_CONFIGURED, "no backend means configured");
    CHECK(s.backend_pending == 1, "backend pending flagged");
    CHECK(strcmp(dvs_tx_state_name(s.state), "configured") == 0,
          "state name configured");

    /* backend ok: running, error cleared */
    dvs_tx_init(&s);
    dvs_tx_set_backend(&s, fake_backend_ok);
    good_params(&p);
    CHECK(dvs_tx_start(&s, &p, "input", err, sizeof(err)) == 0,
          "tx_start with backend ok");
    CHECK(s.state == DVS_TX_RUNNING, "backend ok means running");
    CHECK(s.error[0] == '\0', "running clears error");

    /* backend fail: failed with reason kept */
    dvs_tx_init(&s);
    dvs_tx_set_backend(&s, fake_backend_fail);
    good_params(&p);
    CHECK(dvs_tx_start(&s, &p, "input", err, sizeof(err)) != 0,
          "tx_start with backend fail");
    CHECK(s.state == DVS_TX_FAILED, "backend fail means failed");
    CHECK(strstr(s.error, "mpp busy") != NULL, "backend reason kept");

    /* backend positive return: contract violation, loud failure, never pending */
    dvs_tx_init(&s);
    dvs_tx_set_backend(&s, fake_backend_positive);
    good_params(&p);
    CHECK(dvs_tx_start(&s, &p, "input", err, sizeof(err)) != 0,
          "tx_start with backend positive return fails");
    CHECK(s.state == DVS_TX_FAILED, "backend positive return means failed");
    CHECK(s.backend_pending == 0, "backend positive return never pending");
    CHECK(strstr(s.error, "TX_BACKEND_CONTRACT") != NULL,
          "backend contract violation is explicit");

    /* stop is idempotent and clears params */
    dvs_tx_stop(&s);
    CHECK(s.state == DVS_TX_IDLE, "stop returns to idle");
    CHECK(s.params.width == 0 && s.backend_pending == 0, "stop clears params");
    dvs_tx_stop(&s);
    CHECK(s.state == DVS_TX_IDLE, "double stop stays idle");

    /* status json carries the minimal field set */
    dvs_tx_init(&s);
    good_params(&p);
    dvs_tx_start(&s, &p, "input", err, sizeof(err));
    struct json_object* st = dvs_tx_status_json(&s);
    CHECK(st && jstr(st, "state") && strcmp(jstr(st, "state"), "configured") == 0,
          "status json state configured");
    struct json_object* t = NULL;
    json_object_object_get_ex(st, "target", &t);
    struct json_object* tv = NULL;
    const char* sip = NULL;
    if (t && json_object_object_get_ex(t, "sip", &tv) && tv)
        sip = json_object_get_string(tv);
    CHECK(sip && strcmp(sip, "239.0.0.1") == 0, "status json target sip");
    json_object_put(st);
    st = dvs_tx_status_json(NULL);
    CHECK(st && jstr(st, "state") && strcmp(jstr(st, "state"), "idle") == 0,
          "status json null session is idle");
    json_object_put(st);

    /* capability gate: NULL ceiling disables the gate */
    good_params(&p);
    CHECK(dvs_tx_validate_against_capability(&p, NULL, err, sizeof(err)) == 0,
          "capability gate passes with NULL ceiling");
    dvs_tx_cap_t cap;
    memset(&cap, 0, sizeof(cap));
    snprintf(cap.codecs, sizeof(cap.codecs), "h264,h265");
    cap.max_w = 1920;
    cap.max_h = 1080;
    cap.max_fps = 30;
    cap.max_br_kbps = 8000;
    good_params(&p);
    CHECK(dvs_tx_validate_against_capability(&p, &cap, err, sizeof(err)) == 0,
          "capability gate passes fitting params");
    good_params(&p);
    p.width = 3840;
    CHECK(dvs_tx_validate_against_capability(&p, &cap, err, sizeof(err)) != 0,
          "capability gate rejects over-width params");
    good_params(&p);
    p.fps = 60;
    CHECK(dvs_tx_validate_against_capability(&p, &cap, err, sizeof(err)) != 0,
          "capability gate rejects over-fps params");
    good_params(&p);
    p.bitrate_kbps = 12000;
    CHECK(dvs_tx_validate_against_capability(&p, &cap, err, sizeof(err)) != 0,
          "capability gate rejects over-bitrate params");
    good_params(&p);
    snprintf(p.codec, sizeof(p.codec), "h265");
    snprintf(cap.codecs, sizeof(cap.codecs), "h264");
    CHECK(dvs_tx_validate_against_capability(&p, &cap, err, sizeof(err)) != 0,
          "capability gate rejects unlisted codec");

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
