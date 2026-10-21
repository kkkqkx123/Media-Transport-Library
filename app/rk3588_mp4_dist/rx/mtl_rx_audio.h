/* SPDX-License-Identifier: BSD-3-Clause
 *
 * ST30 RX session — lcore callback hands frames to ALSA thread via lock-free SPSC.
 */

#ifndef _MTL_RX_AUDIO_H_
#define _MTL_RX_AUDIO_H_

#include "../mp4_dist_app.h"
#include "av_sync.h"
#include <stdatomic.h>
#include <stdint.h>
#include <st30_api.h>

struct alsa_audio;

typedef struct mtl_rx_audio mtl_rx_audio;

typedef struct mtl_rx_audio_params {
    mtl_handle      st;
    const char*     rx_port;
    const char*     rx_sip;
    uint16_t        audio_udp_port;
    uint8_t         payload_type;
    struct alsa_audio* alsa;
    av_sync*        sync;
    int             cpu;
} mtl_rx_audio_params;

mtl_rx_audio* mtl_rx_audio_create(const mtl_rx_audio_params* p);
int  mtl_rx_audio_start(mtl_rx_audio* rx);
void mtl_rx_audio_stop(mtl_rx_audio* rx);
void mtl_rx_audio_free(mtl_rx_audio* rx);

#endif /* _MTL_RX_AUDIO_H_ */
