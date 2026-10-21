/* SPDX-License-Identifier: BSD-3-Clause
 *
 * TX Scheduler — orchestrates MP4 demux → extradata → MTL TX with frame pacing.
 * Runs the main TX loop in a dedicated thread (or synchronously in TX mode).
 */

#ifndef _TX_SCHEDULER_H_
#define _TX_SCHEDULER_H_

#include "../mp4_dist_app.h"
#include <stdatomic.h>

/** Opaque scheduler context. */
typedef struct tx_scheduler tx_scheduler;

/**
 * Create the scheduler and all sub-components (demux, extradata, MTL TX).
 * @param mt     initialized MTL handle
 * @param cfg    application configuration (mp4_file must be set)
 * @param out    [out] receives scheduler
 * @return 0 on success
 */
int tx_scheduler_create(mtl_handle mt, const mp4_dist_cfg* cfg,
                        tx_scheduler** out);

/**
 * Run the TX main loop (blocking).
 * Video + audio producer threads; MTL session pull drives wire pacing.
 *
 * @param sched   scheduler
 * @param running atomic flag; loop stops when this becomes false
 */
void tx_scheduler_run(tx_scheduler* sched, atomic_bool* running);

/** Stop and free all resources. */
void tx_scheduler_close(tx_scheduler* sched);

#endif /* _TX_SCHEDULER_H_ */
