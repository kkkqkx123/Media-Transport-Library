#include "engine_ctx.h"
#include "session_admit.h" /* whitelist + LRU admission */

/* session_mgr — media engine lifecycle owner.
 *
 * Each ST22 lane in the current layout maps 1:1 to a "session" (a compositor
 * lane). Per-session MTL session management with whitelist/LRU admission
 * is a future refinement; here the engine owns the compositor and
 * this module is the lifecycle entry point used by the IPC layer. */

dvs_engine* dvs_session_mgr_create(const file_playout_cfg* base_cfg) {
    return dvs_engine_create(base_cfg);
}

void dvs_session_mgr_destroy(dvs_engine* e) {
    dvs_engine_destroy(e);
}

int dvs_session_mgr_apply_layout(dvs_engine* e,
                                 const file_playout_pip_stream* streams, int count) {
    return dvs_engine_apply_layout(e, streams, count);
}

void dvs_session_mgr_request_stop(dvs_engine* e) {
    dvs_engine_request_stop(e);
}

/* Configure the allowed-source whitelist. csv = "ip:port,...".
 * NULL/"" disables the whitelist (allow all). */
void dvs_session_mgr_set_whitelist(const char* csv) {
    dvs_session_whitelist_set(csv);
}
