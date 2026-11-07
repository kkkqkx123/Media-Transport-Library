#ifndef _DVS_ENGINE_CTX_H_
#define _DVS_ENGINE_CTX_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <json-c/json.h>
#include <stdatomic.h>
#include <pthread.h>

#include "file_playout_app.h"
#include "rx/rx_compositor.h"
#include "engine/tx_session.h"

/* Central media engine context for dvs_media.
 *
 * Owns the MTL instance and (at most one) rx_compositor covering the current
 * layout. Layout changes are applied by recreating the compositor
 * (stop + free + create + start) — a pragmatic "commit" that is correct but not
 * yet frame-boundary atomic. */
typedef struct dvs_engine {
    mtl_handle      st;        /* MTL instance (NULL until inited) */
    rx_compositor*  comp;      /* current compositor (NULL when media off) */
    file_playout_cfg    cfg;       /* working copy: base params + current layout */
    atomic_bool     running;   /* loop thread alive / media active */
    pthread_t       loop_tid;  /* run-loop thread (when media is on) */
    atomic_int      rx_sessions;
    atomic_int      decoding;
    atomic_int      layers;
    pthread_mutex_t mu;        /* guards st/comp swap during reconfiguration */

    /* pending audio routing (window identifier + volume; applied on commit,
     * or immediately when only the owner changes) */
    char            audio_owner[64];
    int             audio_volume;
    /* last applied audio state (honest query, never pending echo) */
    char            audio_applied_owner[64];
    char            audio_applied_sip[32];
    uint16_t        audio_applied_port;
    int             audio_applied_volume;
    int             audio_applied_ok;

    /* node role driving mutual exclusion ("input" | "output"); TX may
     * only start in the input role, RX only composes in the output role. */
    char            role[16];
    /* input-box capture/send session (params + state machine; the engine
     * installs the live_ingest data-plane backend when linked with it). */
    dvs_tx_session_t tx;
} dvs_engine;

/* Create an engine from a base config (network/DRM/MTL params). Media is not
 * started until dvs_engine_apply_layout() is called with >=1 stream. */
dvs_engine* dvs_engine_create(const file_playout_cfg* base_cfg);
void        dvs_engine_destroy(dvs_engine* e);

/* Init MTL up-front (so the first commit is fast). No compositor yet. */
int  dvs_engine_init_mtl(dvs_engine* e);

/* Apply the base config's current pip_streams layout and start the run loop. */
int  dvs_engine_start(dvs_engine* e);

/* Recreate the compositor with a new layout (count==0 clears media). */
int  dvs_engine_apply_layout(dvs_engine* e,
                             const file_playout_pip_stream* streams, int count);

/* Apply with a target TAI time in nanoseconds (0 = immediate). Geometry-only
 * changes reuse the running compositor without decoder rebuild; other
 * changes fall back to recreation. A future target is honored on the fast
 * path by waiting for the TAI clock before swapping the layout. */
int  dvs_engine_apply_layout_at(dvs_engine* e,
                                const file_playout_pip_stream* streams, int count,
                                uint64_t apply_at_ns);

/* Set the desired audio owner/volume; applied on the next layout commit. */
void dvs_engine_set_audio(dvs_engine* e, const char* owner, int volume);

/* Applied audio snapshot for query_state (new ref, never NULL). */
struct json_object* dvs_engine_audio_status(dvs_engine* e);

/* Store an output mode from the node agent. Validates ranges and updates the
 * cached config; when a compositor is running it is rebuilt so the new mode
 * takes effect immediately, otherwise the mode waits for the next layout. */
int dvs_engine_set_output(dvs_engine* e, const char* connector, int width,
                          int height, int refresh_hz, const char* mode);

/* Store a role switch. Input boxes stop display output; output boxes stay
 * idle until a layout arrives. Switching to output also stops any TX
 * session; switching to input stops the RX compositor. */
int dvs_engine_set_role(dvs_engine* e, const char* role);

/* Current node role ("input" | "output"). Never NULL. */
const char* dvs_engine_role(const dvs_engine* e);

/* Non-zero when nothing is running: no compositor, no TX session and no
 * residual layout. Round-trip and maintenance checks use this as the
 * no-leak assertion. */
int dvs_engine_media_idle(dvs_engine* e);

/* TX orchestration (input role only). Params must be complete; on builds
 * without the data-plane backend the session reports configured,
 * never running. */
int dvs_engine_tx_start(dvs_engine* e, const dvs_tx_params_t* params,
                        char* err, size_t errlen);
void dvs_engine_tx_stop(dvs_engine* e);

/* TX snapshot for query_state (new json ref, caller puts; never NULL). */
struct json_object* dvs_engine_tx_status(dvs_engine* e);

/* Stop the run loop, free compositor, uninit MTL. */
void dvs_engine_stop(dvs_engine* e);

/* Request a graceful stop (e.g. on SIGINT) without joining. */
void dvs_engine_request_stop(dvs_engine* e);

/* Live counters for query_state. */
void dvs_engine_counts(dvs_engine* e, int* rx_sessions, int* decoding,
                       int* layers);

/* Per-lane readiness snapshot for query_state lanes. Empty array means zero
 * admitted sessions. Ready reflects a running compositor; loss and timestamp
 * fields come from the compositor lane statistics. */
struct json_object* dvs_engine_lane_stats(dvs_engine* e);

#endif /* _DVS_ENGINE_CTX_H_ */
