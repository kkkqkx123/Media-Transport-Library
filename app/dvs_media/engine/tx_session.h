/* TX session — input-box capture/send orchestration state.
 *
 * Responsibility split: this module owns TX parameter validation and the TX
 * lifecycle state machine only. It has no MTL/MPP dependency so it builds
 * and unit-tests on x86. The compositor engine (engine_ctx.c) owns role
 * mutual exclusion (stopping RX before TX starts) and owns data-plane
 * teardown; the data-plane backend (live_ingest capture/encode/MTL TX)
 * is installed by the engine when linked with it.
 *
 * Honesty rule: without a backend the session reports configured (params
 * stored and validated) with backend_pending set — it never claims packets
 * are flowing. Lanes keep their RX meaning; TX status is a separate object.
 */
#ifndef _DVS_TX_SESSION_H_
#define _DVS_TX_SESSION_H_

#include <stddef.h>

struct json_object;

typedef enum {
    DVS_TX_IDLE = 0,      /* no TX params, nothing running */
    DVS_TX_CONFIGURED,    /* params validated/stored, backend pending */
    DVS_TX_RUNNING,       /* backend accepted, packets flowing */
    DVS_TX_FAILED         /* last start failed, see error */
} dvs_tx_state_t;

const char* dvs_tx_state_name(dvs_tx_state_t s);

/* Frozen SoC ceilings (RK3588 public limits). Used when the caller has no
 * reported capability; a reported capability overrides per-field. */
#define DVS_TX_MAX_W      3840
#define DVS_TX_MAX_H      2160
#define DVS_TX_MAX_FPS    60
#define DVS_TX_MAX_BR_KBPS 20000
#define DVS_TX_MIN_W      640
#define DVS_TX_MIN_H      480
#define DVS_TX_MIN_FPS    24
#define DVS_TX_MIN_BR_KBPS 500

typedef struct {
    char port[32];        /* capture port id, e.g. "HDMI1" */
    char codec[16];       /* "h264" | "h265" */
    int  width;           /* 640..3840 */
    int  height;          /* 480..2160 */
    int  fps;            /* 24..60 */
    int  bitrate_kbps;   /* 500..20000 */
    char target_sip[64]; /* single IPv4 destination */
    int  target_port;    /* 1..65535 */
} dvs_tx_params_t;

/* Data-plane backend. Returns 0 when packets flow, 1 when the backend is
 * absent (session stays configured with backend_pending set), <0 on failure.
 * NULL backend means absent. The engine installs the live_ingest backend
 * entry when built with it; builds without it stay backend-less. */
typedef int (*dvs_tx_backend_fn)(const dvs_tx_params_t* p,
                                 char* err, size_t errlen);

typedef struct {
    dvs_tx_state_t    state;
    dvs_tx_params_t   params;
    char              error[256];
    int               backend_pending;
    dvs_tx_backend_fn backend;
} dvs_tx_session_t;

void dvs_tx_init(dvs_tx_session_t* s);
void dvs_tx_set_backend(dvs_tx_session_t* s, dvs_tx_backend_fn fn);

/* Validate ranges only (no role check, no state change). */
int dvs_tx_validate(const dvs_tx_params_t* p, char* err, size_t errlen);

/* Explicit capability ceiling for TX parameter gating. A NULL ceiling
 * disables the gate (frozen SoC ranges from dvs_tx_validate still apply).
 * An empty codecs list accepts any known codec. */
typedef struct {
    int  max_w;
    int  max_h;
    int  max_fps;
    int  max_br_kbps;
    char codecs[16]; /* comma list, e.g. "h264,h265" */
} dvs_tx_cap_t;

/* Gate already range-validated params against the reported capability.
 * Returns 0 when the params fit, -1 with err set otherwise. */
int dvs_tx_validate_against_capability(const dvs_tx_params_t* p,
                                       const dvs_tx_cap_t* cap,
                                       char* err, size_t errlen);

/* Start TX. Requires role "input" (else TX_ROLE_MISMATCH); validates params;
 * on backend absence reports configured+pending, on backend success running,
 * on backend failure failed. Never claims running without backend success. */
int dvs_tx_start(dvs_tx_session_t* s, const dvs_tx_params_t* p,
                 const char* role, char* err, size_t errlen);

/* Stop TX. Idempotent: always ends in idle with params cleared. */
void dvs_tx_stop(dvs_tx_session_t* s);

/* Snapshot for query_state. Returns a new json object (caller puts). */
struct json_object* dvs_tx_status_json(const dvs_tx_session_t* s);

#endif /* _DVS_TX_SESSION_H_ */
