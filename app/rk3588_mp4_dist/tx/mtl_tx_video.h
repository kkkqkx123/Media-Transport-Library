/* SPDX-License-Identifier: BSD-3-Clause
 *
 * ST22 TX session wrapper — framebuff pool + lcore pull callbacks.
 * Producer thread: acquire_fb → fill → publish_fb (MTL get_next_frame paces wire).
 */

#ifndef _MTL_TX_VIDEO_H_
#define _MTL_TX_VIDEO_H_

#include "../mp4_dist_app.h"
#include <stddef.h>
#include <stdint.h>
#include <st20_api.h>

struct mtl_tx_video;

struct mtl_tx_video_params {
    mtl_handle   st;
    int          framebuff_cnt;
    size_t       framebuff_max;
    int          width;
    int          height;
    float        fps;
    uint16_t     udp_port;
    const char*  dip_addr;
    const char*  port_name;
    uint8_t      payload_type;
    /** P7: ST22 USER_TIMESTAMP + USER_PACING (false = MTL default pacing). */
    bool         use_tai_timestamp;
};

struct mtl_tx_video* mtl_tx_video_create(const struct mtl_tx_video_params* p);
void mtl_tx_video_free(struct mtl_tx_video* tx);

int  mtl_tx_video_start(struct mtl_tx_video* tx);
void mtl_tx_video_stop(struct mtl_tx_video* tx);

int  mtl_tx_video_acquire_fb(struct mtl_tx_video* tx, uint16_t* idx, void** addr);
int  mtl_tx_video_publish_fb(struct mtl_tx_video* tx, uint16_t idx, size_t size);
void mtl_tx_video_abort_fb(struct mtl_tx_video* tx, uint16_t idx);

st22_tx_handle mtl_tx_video_handle(struct mtl_tx_video* tx);

#endif /* _MTL_TX_VIDEO_H_ */
