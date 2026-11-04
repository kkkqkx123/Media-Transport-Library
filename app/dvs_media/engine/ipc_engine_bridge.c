/* Engine <-> IPC bridge.
 *
 * Compiled into the real dvs_media binary only (NOT the no-MTL unit test).
 * Wires the IPC server's engine hooks to the actual media engine so the test
 * target links without MTL/MPP. */

#include "ipc/ipc_server.h"
#include "engine_ctx.h"
#include "engine/layout_executor.h"
#include <mtl_api.h>
#include <stddef.h>
#include <stdio.h>

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
