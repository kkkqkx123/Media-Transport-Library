#ifndef _LIVE_INGEST_MTL_TX_AUDIO_H_
#define _LIVE_INGEST_MTL_TX_AUDIO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <mtl_api.h>
#include <st30_api.h>

struct mtl_tx_audio;

struct mtl_tx_audio_params {
    mtl_handle   st;
    int          framebuff_cnt;
    uint16_t     udp_port;
    const char*  dip_addr;
    const char*  port_name;
    uint8_t      payload_type;
    enum st30_fmt      fmt;
    enum st30_sampling sampling;
    enum st30_ptime    ptime;
    uint16_t           channel;
    bool               use_tai_timestamp;
};

struct mtl_tx_audio* mtl_tx_audio_create(const struct mtl_tx_audio_params* p);
void mtl_tx_audio_free(struct mtl_tx_audio* tx);

/** Re-anchor TAI timeline (playlist file switch / seek). */
void mtl_tx_audio_tai_reset(struct mtl_tx_audio* tx);

int  mtl_tx_audio_start(struct mtl_tx_audio* tx);
void mtl_tx_audio_stop(struct mtl_tx_audio* tx);

int  mtl_tx_audio_acquire_fb(struct mtl_tx_audio* tx, uint16_t* idx, void** addr);
int  mtl_tx_audio_publish_fb(struct mtl_tx_audio* tx, uint16_t idx, size_t size);
void mtl_tx_audio_abort_fb(struct mtl_tx_audio* tx, uint16_t idx);

size_t mtl_tx_audio_frame_bytes(const struct mtl_tx_audio* tx);
st30_tx_handle mtl_tx_audio_handle(struct mtl_tx_audio* tx);

#endif
