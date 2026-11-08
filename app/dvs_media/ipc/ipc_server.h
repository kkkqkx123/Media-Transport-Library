#ifndef DVS_IPC_SERVER_H
#define DVS_IPC_SERVER_H

#include <stddef.h>

#include <json-c/json.h>

struct dvs_engine; /* forward declaration; full type in engine/engine_ctx.h */

/* Tuneable simulated prepare duration (ms). Default 50; tests may set 0. */
void dvs_ipc_set_prepare_delay_ms(int ms);

/* Engine hook table. main() installs the real implementation (engine/
 * ipc_engine_bridge.c); when unset the IPC server runs in pure state-machine
 * mode (used by the no-MTL unit test). This keeps ipc_server.c free of direct
 * engine_ctx/layout_executor symbol references so the test target links
 * without MTL/MPP. */
typedef struct {
    int  (*prepare)(struct json_object* req, char* err, size_t errlen); /* stash layout or apply config */
    int  (*commit)(void);                     /* apply pending layout */
    void (*clear)(void);                      /* remove all media */
    void (*abort)(void);                      /* discard pending layout */
    /* TX orchestration (input role only). tx_start requires complete
     * params; both may be NULL (tx verbs then report NOT_IMPLEMENTED / idle). */
    int  (*tx_start)(struct json_object* req, char* err, size_t errlen);
    void (*tx_stop)(void);
    void (*counts)(int* rx_sessions, int* decoding, int* layers); /* query_state */
    void (*ptp_state)(char* state, size_t n, int* offset_us); /* live PTP */
    /* MTL version string (e.g. from mtl_version()). May be NULL; the
     * capability reply then keeps the last probed value. */
    const char* (*mtl_version)(void);
    /* Backend string derived from the registered MTL ports. May be NULL. */
    const char* (*backend)(void);
    /* Applied audio snapshot for query_state. May be NULL. */
    struct json_object* (*audio_status)(void);
    /* Last commit report {frameIndex,appliedAtTaiNs,late} merged into the
     * commit response data. May be NULL; the response then carries only
     * the revision. */
    struct json_object* (*commit_info)(void);
    /* Short failure code for the last failed commit ("" when none). May
     * be NULL; the failure envelope then keeps the generic INTERNAL. */
    const char* (*commit_error)(void);
    /* Still + adjustment status fragment merged into query_state. May be
     * NULL; query_state then omits the display section. */
    struct json_object* (*display_status)(void);
    /* Per-lane readiness stats for the barrier. May be NULL; query_state
     * then reports an empty lanes array. */
    struct json_object* (*lane_stats)(void);
    /* TX session snapshot for query_state. May be NULL; query_state then
     * reports {state:"idle"}. The callee returns a new reference. */
    struct json_object* (*tx_status)(void);
} dvs_ipc_engine_hooks_t;

void dvs_ipc_install_hooks(const dvs_ipc_engine_hooks_t* h);

/* Attach the media engine created by main; layout/query_state then drive it.
 * Defined in engine/ipc_engine_bridge.c (compiled into the real binary only). */
void dvs_ipc_attach_engine(struct dvs_engine* e);

/* Run the IPC server (blocking). JSON line protocol over a Unix domain
 * socket: long-lived connections, messageId echo, a hello handshake carrying
 * the bundle contract versions, async prepare with media.prepare.result
 * events, and broadcast events on commit. Configuration flows through the
 * single configure verb; query_state drives status. */
int dvs_ipc_server_run(const char* sock_path);

#endif
