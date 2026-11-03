#ifndef _DVS_LAYOUT_EXECUTOR_H_
#define _DVS_LAYOUT_EXECUTOR_H_

#include "engine_ctx.h"
#include <json-c/json.h>

/* layout_executor — two-phase prepare/commit over a shadow layout (see
 * layout_executor.c). */

int dvs_layout_from_json(struct json_object* req, file_playout_pip_stream* out,
                         int* count);
int dvs_layout_prepare(const file_playout_pip_stream* streams, int count);
int dvs_layout_prepare_from_json(struct json_object* req);
uint64_t dvs_layout_pending_apply_at_ns(void);
int dvs_layout_commit(dvs_engine* e);
void dvs_layout_clear(dvs_engine* e);
void dvs_layout_abort(void);

#endif /* _DVS_LAYOUT_EXECUTOR_H_ */
