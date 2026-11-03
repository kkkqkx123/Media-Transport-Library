/* Engine <-> IPC bridge.
 *
 * Compiled into the real dvs_media binary only (NOT the no-MTL unit test).
 * Wires the IPC server's engine hooks to the actual media engine so the test
 * target links without MTL/MPP. */

#include "ipc/ipc_server.h"
#include "engine_ctx.h"
#include "engine/layout_executor.h"

static dvs_engine* s_eng = NULL;

static void hook_prepare(struct json_object* req) {
    dvs_layout_prepare_from_json(req);
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

void dvs_ipc_attach_engine(dvs_engine* e) {
    s_eng = e;
    dvs_ipc_engine_hooks_t h = {
        .prepare = hook_prepare,
        .commit = hook_commit,
        .clear = hook_clear,
        .abort = hook_abort,
        .counts = hook_counts,
    };
    dvs_ipc_install_hooks(&h);
}
