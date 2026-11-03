#ifndef _DVS_SESSION_MGR_H_
#define _DVS_SESSION_MGR_H_

#include "engine_ctx.h"

/* session_mgr — media engine lifecycle owner (see session_mgr.c). */
dvs_engine* dvs_session_mgr_create(const file_playout_cfg* base_cfg);
void        dvs_session_mgr_destroy(dvs_engine* e);
int         dvs_session_mgr_apply_layout(dvs_engine* e,
                                         const file_playout_pip_stream* streams,
                                         int count);
void        dvs_session_mgr_request_stop(dvs_engine* e);

/* Configure the allowed-source whitelist (S4 admission). */
void        dvs_session_mgr_set_whitelist(const char* csv);

#endif /* _DVS_SESSION_MGR_H_ */
