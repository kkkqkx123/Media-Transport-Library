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
    void (*prepare)(struct json_object* req); /* stash pending layout */
    int  (*commit)(void);                     /* apply pending layout */
    void (*clear)(void);                      /* remove all media */
    void (*abort)(void);                      /* discard pending layout */
    void (*counts)(int* rx_sessions, int* decoding, int* layers); /* query_state */
} dvs_ipc_engine_hooks_t;

void dvs_ipc_install_hooks(const dvs_ipc_engine_hooks_t* h);

/* Attach the media engine created by main; layout/query_state then drive it.
 * Defined in engine/ipc_engine_bridge.c (compiled into the real binary only). */
void dvs_ipc_attach_engine(struct dvs_engine* e);

/* Run the IPC server (blocking). JSON line protocol over a Unix domain
 * socket: long-lived connections, messageId echo, async prepare with
 * media.prepare.result events, and broadcast events on commit. */
int dvs_ipc_server_run(const char* sock_path);

#endif
