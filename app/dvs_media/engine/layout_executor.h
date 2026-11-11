#ifndef _DVS_LAYOUT_EXECUTOR_H_
#define _DVS_LAYOUT_EXECUTOR_H_

#include "engine_ctx.h"
#include <json-c/json.h>
#include <stdint.h>

/* layout_executor — two-phase prepare/commit over a shadow layout (see
 * layout_executor.c). Frame/still/adjust types live in engine_ctx.h. */

int dvs_layout_from_json(struct json_object* req, file_playout_pip_stream* out,
                         int* count);
int dvs_layout_prepare(const file_playout_pip_stream* streams, int count);
int dvs_layout_prepare_from_json(struct json_object* req);
uint64_t dvs_layout_pending_apply_at_ns(void);
/* Pending frame anchor / still / adjust (copies under the pending lock). */
void dvs_layout_pending_frame(dvs_frame_anchor_t* out);
void dvs_layout_pending_base(dvs_base_image_desc_t* out);
void dvs_layout_pending_adjust(dvs_image_adjust_t* out);
int dvs_layout_commit(dvs_engine* e);
void dvs_layout_clear(dvs_engine* e);
void dvs_layout_abort(void);
/* Abort while keeping the given live sources admitted: staged entries also
 * present in the live picture are left untouched so a shared-signal abort
 * cannot drop the running compositor's admission. */
void dvs_layout_abort_except(const file_playout_pip_stream* live, int live_n);

#endif /* _DVS_LAYOUT_EXECUTOR_H_ */
