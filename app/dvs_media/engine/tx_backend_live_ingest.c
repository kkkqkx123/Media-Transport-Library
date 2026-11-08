/* TX backend: dvs_tx_params_t -> live_ingest capture/encode/MTL-TX chain.
 *
 * Engine-owned (engine_ctx installs the entry and calls stop on every
 * teardown path). Holds the running MTL TX session + TX pipeline in this
 * file's statics; the tx_session module stays a pure state machine and never
 * sees these handles. All entry/stop calls happen under the engine mutex.
 *
 * Backend-only translation file: compiled solely when MPP+RGA exist
 * (HAVE_TX_BACKEND). Without it the engine links backend-less and TX start
 * honestly reports configured+pending. */

#include "tx_backend.h"
#include "tx_port_map.h"

#include "mtl_tx.h"
#include "pipeline/tx_pipeline.h"

#include <mtl_api.h>
#include <st30_pipeline_api.h>

#include <alsa/asoundlib.h>

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

static mtl_handle g_st = NULL;
static dvs_tx_backend_cfg g_be_cfg;
static int g_has_ctx = 0;

static struct mtl_tx* g_mtx = NULL;
static struct tx_pipeline* g_tp = NULL;
static struct tx_mtl_sink_ops g_sink;
static char g_vdev[DVS_TX_DEV_LEN];

/* Audio TX (ST2110-30): ALSA capture thread feeding one st30p session.
 * 48kHz stereo PCM16, 1ms packets, 10ms frames. Started only when the TX
 * params carry a non-zero audio port; a failed audio bring-up fails the
 * whole TX start loudly instead of sending silent video. */
#define DVS_TX_AUDIO_RATE 48000
#define DVS_TX_AUDIO_CH 2
#define DVS_TX_AUDIO_FRAME_MS 10
#define DVS_TX_AUDIO_FB_CNT 4

static st30p_tx_handle g_atx = NULL;
static pthread_t g_atx_tid;
static atomic_bool g_atx_running;
static atomic_bool g_atx_ready;
static char g_adev[128];

static void audio_tx_stop(void);

static void* audio_tx_thread(void* arg) {
    (void)arg;
#ifdef __linux__
    pthread_setname_np(pthread_self(), "dvs_txau");
#endif
    snd_pcm_t* pcm = NULL;
    const char* dev = g_adev[0] ? g_adev : "default";
    if (snd_pcm_open(&pcm, dev, SND_PCM_STREAM_CAPTURE, 0) != 0) {
        fprintf(stderr, "dvs_tx: audio capture open failed (%s)\n", dev);
        atomic_store(&g_atx_running, false);
        return NULL;
    }
    snd_pcm_hw_params_t* hw = NULL;
    snd_pcm_hw_params_alloca(&hw);
    int prc = 0;
    prc |= snd_pcm_hw_params_any(pcm, hw);
    prc |= snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    prc |= snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    prc |= snd_pcm_hw_params_set_channels(pcm, hw, DVS_TX_AUDIO_CH);
    {
        unsigned int rate = DVS_TX_AUDIO_RATE;
        prc |= snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL);
    }
    {
        snd_pcm_uframes_t period = DVS_TX_AUDIO_RATE / 100;
        prc |= snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, NULL);
    }
    prc |= snd_pcm_hw_params(pcm, hw);
    if (prc != 0) {
        fprintf(stderr, "dvs_tx: audio hw params failed (%s)\n", dev);
        snd_pcm_close(pcm);
        atomic_store(&g_atx_running, false);
        return NULL;
    }
    atomic_store(&g_atx_ready, true);
    while (atomic_load(&g_atx_running)) {
        struct st30_frame* f = st30p_tx_get_frame(g_atx);
        if (!f) continue;
        size_t want = f->data_size ? f->data_size : f->buffer_size;
        size_t got = 0;
        while (got < want && atomic_load(&g_atx_running)) {
            size_t left = want - got;
            /* 10ms frames at 48k stereo S16: bounded stack scratch. */
            unsigned char scratch[8192];
            size_t chunk = left > sizeof(scratch) ? sizeof(scratch) : left;
            snd_pcm_sframes_t n =
                snd_pcm_readi(pcm, scratch, chunk / (DVS_TX_AUDIO_CH * 2));
            if (n < 0) {
                snd_pcm_recover(pcm, (int)n, 0);
                continue;
            }
            if (n == 0) continue;
            memcpy((unsigned char*)f->addr + got, scratch,
                   (size_t)n * DVS_TX_AUDIO_CH * 2);
            got += (size_t)n * DVS_TX_AUDIO_CH * 2;
        }
        if (got < want) {
            st30p_tx_put_frame_abort(g_atx, f);
        } else {
            f->data_size = got;
            st30p_tx_put_frame(g_atx, f);
        }
    }
    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    return NULL;
}

static int audio_tx_start(mtl_handle st, const char* tx_port,
                          const char* dip, uint16_t udp_port,
                          char* err, size_t errlen) {
    struct st30p_tx_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.port.num_port = 1;
    snprintf(ops.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             tx_port);
    if (inet_pton(AF_INET, dip, ops.port.dip_addr[MTL_SESSION_PORT_P]) != 1) {
        if (err && errlen > 0) snprintf(err, errlen, "bad audio target ip");
        return -1;
    }
    ops.port.udp_port[MTL_SESSION_PORT_P] = udp_port;
    ops.port.payload_type = 98;
    ops.fmt = ST30_FMT_PCM16;
    ops.channel = DVS_TX_AUDIO_CH;
    ops.sampling = ST30_SAMPLING_48K;
    ops.ptime = ST30_PTIME_1MS;
    ops.framebuff_cnt = DVS_TX_AUDIO_FB_CNT;
    {
        int fbsize = st30_calculate_framebuff_size(
            ops.fmt, ops.ptime, ops.sampling, ops.channel,
            (uint64_t)DVS_TX_AUDIO_FRAME_MS * 1000000ULL, NULL);
        if (fbsize <= 0) {
            if (err && errlen > 0) snprintf(err, errlen, "bad audio frame size");
            return -1;
        }
        ops.framebuff_size = (uint32_t)fbsize;
    }
    ops.flags = ST30P_TX_FLAG_BLOCK_GET;
    ops.name = "dvs_tx_audio";
    g_atx = st30p_tx_create(st, &ops);
    if (!g_atx) {
        if (err && errlen > 0)
            snprintf(err, errlen, "ST30 TX create failed (%s:%u)", dip,
                     (unsigned)udp_port);
        return -1;
    }
    atomic_store(&g_atx_running, true);
    atomic_store(&g_atx_ready, false);
    if (pthread_create(&g_atx_tid, NULL, audio_tx_thread, NULL) != 0) {
        atomic_store(&g_atx_running, false);
        st30p_tx_free(g_atx);
        g_atx = NULL;
        if (err && errlen > 0) snprintf(err, errlen, "audio thread failed");
        return -1;
    }
    /* Bring-up handshake: a dead capture device fails the start loudly
     * here instead of sending video with a dead audio chain. */
    for (int i = 0; i < 100; i++) {
        if (atomic_load(&g_atx_ready)) break;
        if (!atomic_load(&g_atx_running)) break;
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 10000000L};
        nanosleep(&ts, NULL);
    }
    if (!atomic_load(&g_atx_ready)) {
        audio_tx_stop();
        if (err && errlen > 0)
            snprintf(err, errlen, "audio capture not ready (%s)",
                     g_adev[0] ? g_adev : "default");
        return -1;
    }
    return 0;
}

static void audio_tx_stop(void) {
    if (g_atx) {
        atomic_store(&g_atx_running, false);
        st30p_tx_wake_block(g_atx);
        pthread_join(g_atx_tid, NULL);
        st30p_tx_free(g_atx);
        g_atx = NULL;
    }
}

static dvs_tx_port_entry g_table[DVS_TX_PORT_MAX];
static int g_table_n = 0;
static int g_table_valid = 0;

static int be_acquire(void* priv, uint16_t* idx, void** addr) {
    return mtl_tx_acquire_fb((struct mtl_tx*)priv, idx, addr);
}

static int be_publish(void* priv, uint16_t idx, size_t size) {
    return mtl_tx_publish_fb((struct mtl_tx*)priv, idx, size);
}

static void be_abort(void* priv, uint16_t idx) {
    mtl_tx_abort_fb((struct mtl_tx*)priv, idx);
}

void dvs_tx_backend_set_ctx(dvs_mtl_handle_t st,
                            const dvs_tx_backend_cfg* cfg) {
    g_st = (mtl_handle)st;
    if (cfg)
        g_be_cfg = *cfg;
    else
        memset(&g_be_cfg, 0, sizeof(g_be_cfg));
    g_has_ctx = 1;
}

int dvs_tx_backend_running(void) {
    return (g_mtx && g_tp) ? 1 : 0;
}

void dvs_tx_backend_stop(void) {
    audio_tx_stop();
    if (g_tp) {
        tx_pipeline_stop(g_tp);
        tx_pipeline_free(g_tp);
        g_tp = NULL;
    }
    if (g_mtx) {
        mtl_tx_stop(g_mtx);
        mtl_tx_free(g_mtx);
        g_mtx = NULL;
    }
    memset(&g_sink, 0, sizeof(g_sink));
}

int dvs_tx_backend_entry(const dvs_tx_params_t* p, char* err, size_t errlen) {
    if (!p) {
        if (err && errlen > 0) snprintf(err, errlen, "missing tx params");
        return -1;
    }
    if (!g_has_ctx || !g_st) {
        if (err && errlen > 0)
            snprintf(err, errlen, "MTL not initialized for TX");
        return -1;
    }
    if (!g_be_cfg.tx_port[0]) {
        if (err && errlen > 0) snprintf(err, errlen, "TX port not configured");
        return -1;
    }
    int codec = dvs_tx_codec_from_name(p->codec);
    if (codec < 0) {
        if (err && errlen > 0)
            snprintf(err, errlen, "bad codec (h264|h265)");
        return -1;
    }
    int bitrate_bps = 0;
    if (dvs_tx_bitrate_to_bps(p->bitrate_kbps, &bitrate_bps) != 0) {
        if (err && errlen > 0) snprintf(err, errlen, "bad bitrate");
        return -1;
    }

    char cur_devs[DVS_TX_PORT_MAX][DVS_TX_DEV_LEN];
    char cur_stables[DVS_TX_PORT_MAX][DVS_TX_STABLE_LEN];
    int cur_sig[DVS_TX_PORT_MAX];
    memset(cur_devs, 0, sizeof(cur_devs));
    memset(cur_stables, 0, sizeof(cur_stables));
    memset(cur_sig, 0, sizeof(cur_sig));
    int cur_n = dvs_tx_scan_devices(cur_devs, cur_stables, cur_sig,
                                    DVS_TX_PORT_MAX);

    if (!g_table_valid || g_table_n <= 0) {
        g_table_n = dvs_tx_table_build(g_table, DVS_TX_PORT_MAX,
                                       cur_n > 0 ? cur_devs : NULL,
                                       cur_n > 0 ? cur_stables : NULL,
                                       cur_n > 0 ? cur_sig : NULL, cur_n);
        g_table_valid = 1;
    } else {
        /* Keep startup bindings stable, but admit newcomers so devices
         * appearing after the first start stay reachable. */
        for (int i = 0; i < cur_n && g_table_n < DVS_TX_PORT_MAX; i++) {
            int known = 0;
            for (int j = 0; j < g_table_n; j++) {
                if (cur_stables[i][0] && g_table[j].stable_id[0] &&
                    strcmp(cur_stables[i], g_table[j].stable_id) == 0) {
                    known = 1;
                    break;
                }
            }
            if (known) continue;
            dvs_tx_port_entry* e = &g_table[g_table_n];
            snprintf(e->port, sizeof(e->port), "HDMI%d", g_table_n + 1);
            snprintf(e->device, sizeof(e->device), "%s", cur_devs[i]);
            if (cur_stables[i][0])
                snprintf(e->stable_id, sizeof(e->stable_id), "%s",
                         cur_stables[i]);
            else
                snprintf(e->stable_id, sizeof(e->stable_id), "%s",
                         cur_devs[i]);
            e->has_signal = cur_sig[i] != 0;
            g_table_n++;
        }
    }

    char vdev[DVS_TX_DEV_LEN];
    char resolve_err[384];
    if (dvs_tx_resolve_device(g_table, g_table_n,
                              cur_n > 0 ? cur_devs : NULL,
                              cur_n > 0 ? cur_stables : NULL,
                              cur_n > 0 ? cur_sig : NULL, cur_n,
                              p->port, vdev, sizeof(vdev),
                              resolve_err, sizeof(resolve_err)) != 0) {
        if (err && errlen > 0) snprintf(err, errlen, "%s", resolve_err);
        return -1;
    }
    snprintf(g_vdev, sizeof(g_vdev), "%s", vdev);

    /* Restart is teardown + create; stop is idempotent. */
    dvs_tx_backend_stop();

    struct mtl_tx_params mtxp;
    memset(&mtxp, 0, sizeof(mtxp));
    mtxp.st = g_st;
    mtxp.framebuff_cnt = DVS_TX_DEF_FB_CNT;
    mtxp.framebuff_max = DVS_TX_DEF_FB_MAX;
    mtxp.width = p->width;
    mtxp.height = p->height;
    mtxp.fps = (float)p->fps;
    mtxp.udp_port = (uint16_t)p->target_port;
    mtxp.dip_addr = p->target_sip;
    mtxp.port_name = g_be_cfg.tx_port;
    g_mtx = mtl_tx_create(&mtxp);
    if (!g_mtx) {
        if (err && errlen > 0)
            snprintf(err, errlen, "ST22 TX create failed (port %s -> %s:%d)",
                     g_be_cfg.tx_port, p->target_sip, p->target_port);
        return -1;
    }
    mtl_tx_start(g_mtx);
    g_sink.priv = g_mtx;
    g_sink.acquire_fb = be_acquire;
    g_sink.publish_fb = be_publish;
    g_sink.abort_fb = be_abort;

    struct tx_pipeline_params txp;
    memset(&txp, 0, sizeof(txp));
    txp.v4l2_device = g_vdev;
    txp.width = p->width;
    txp.height = p->height;
    txp.fps = (float)p->fps;
    txp.v4l2_buf_count = DVS_TX_DEF_V4L2_BUFS;
    txp.drm_device =
        g_be_cfg.drm_device[0] ? g_be_cfg.drm_device : "/dev/dri/card0";
    txp.rga_pool_size = DVS_TX_DEF_RGA_POOL;
    txp.codec = (codec == 1) ? CODEC_FMT_H265 : CODEC_FMT_H264;
    txp.bitrate_bps = bitrate_bps;
    txp.gop_size = dvs_tx_gop_ms_to_frames(p->gop_ms, p->fps);
    txp.num_slices = DVS_TX_DEF_SLICES;
    txp.intra_refresh_mode = 0;
    txp.intra_refresh_arg = 0;
    txp.coded_blk_size = DVS_TX_DEF_CODED_BLK;
    txp.ring_size = DVS_TX_DEF_RING;
    txp.cpu_capture = g_be_cfg.cpu_capture > 0 ? g_be_cfg.cpu_capture
                                               : DVS_TX_DEF_CPU_CAP;
    txp.cpu_encode = g_be_cfg.cpu_encode > 0 ? g_be_cfg.cpu_encode
                                             : DVS_TX_DEF_CPU_ENC;
    txp.time_now = (uint64_t (*)(void*))mtl_ptp_read_time;
    txp.time_priv = g_st;
    txp.mtl_sink = &g_sink;

    g_tp = tx_pipeline_create(&txp);
    if (!g_tp) {
        int e = errno;
        char why[128];
        snprintf(why, sizeof(why), "%s",
                 e == ENODATA ? "no HDMI signal" : "capture/encode create failed");
        if (err && errlen > 0)
            snprintf(err, errlen, "TX pipeline create failed: %s (%s)", why,
                     g_vdev);
        mtl_tx_stop(g_mtx);
        mtl_tx_free(g_mtx);
        g_mtx = NULL;
        memset(&g_sink, 0, sizeof(g_sink));
        return -1;
    }
    if (tx_pipeline_start(g_tp) != 0) {
        if (err && errlen > 0)
            snprintf(err, errlen, "TX pipeline start failed (%s)", g_vdev);
        tx_pipeline_free(g_tp);
        g_tp = NULL;
        mtl_tx_stop(g_mtx);
        mtl_tx_free(g_mtx);
        g_mtx = NULL;
        memset(&g_sink, 0, sizeof(g_sink));
        return -1;
    }
    /* Audio TX shares the video destination; a requested but failed audio
     * chain fails the whole start instead of sending silent video. */
    if (p->audio_port > 0) {
        snprintf(g_adev, sizeof(g_adev), "%s",
                 g_be_cfg.alsa_device[0] ? g_be_cfg.alsa_device : "default");
        if (audio_tx_start(g_st, g_be_cfg.tx_port, p->target_sip,
                           (uint16_t)p->audio_port, err, errlen) != 0) {
            dvs_tx_backend_stop();
            return -1;
        }
    }
    return 0;
}
