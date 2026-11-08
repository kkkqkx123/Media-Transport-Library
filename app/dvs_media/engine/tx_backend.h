/* TX data-plane backend hook — engine-owned, RK3588 only.
 *
 * This header is MTL-light (no live_ingest types) so engine_ctx.c compiles
 * with or without the backend. The live_ingest implementation lives in
 * tx_backend_live_ingest.c and is linked only when MPP+RGA are present
 * (HAVE_TX_BACKEND); otherwise the engine runs backend-less and TX start
 * honestly reports configured+pending via tx_session.c. */

#ifndef _DVS_TX_BACKEND_H_
#define _DVS_TX_BACKEND_H_

#include <stddef.h>

#include "tx_session.h"

typedef void* dvs_mtl_handle_t;

typedef struct {
    char tx_port[64];      /* MTL TX port name, e.g. "native_af_xdp:enP3p49s0" */
    char drm_device[256];  /* "/dev/dri/card0" for RGA DMA allocation */
    char alsa_device[128]; /* ALSA capture device for audio TX, "" = default */
    int  cpu_capture;
    int  cpu_encode;
} dvs_tx_backend_cfg;

/* Install the engine context the backend entry will use. Must be called
 * before dvs_tx_start() invokes the entry (engine holds its mutex). */
void dvs_tx_backend_set_ctx(dvs_mtl_handle_t st,
                            const dvs_tx_backend_cfg* cfg);

/* Backend entry matching dvs_tx_backend_fn. Returns 0 when packets flow,
 * <0 on failure with err set (no-signal targets name the available table). */
int dvs_tx_backend_entry(const dvs_tx_params_t* p, char* err, size_t errlen);

/* Symmetric teardown: stop + free the MTL TX session and TX pipeline.
 * Idempotent and reentrant; safe to call when idle or twice in a row. */
void dvs_tx_backend_stop(void);

/* Non-zero while the data plane is up. */
int dvs_tx_backend_running(void);

#endif /* _DVS_TX_BACKEND_H_ */
