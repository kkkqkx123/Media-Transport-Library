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

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static mtl_handle g_st = NULL;
static dvs_tx_backend_cfg g_be_cfg;
static int g_has_ctx = 0;

static struct mtl_tx* g_mtx = NULL;
static struct tx_pipeline* g_tp = NULL;
static struct tx_mtl_sink_ops g_sink;
static char g_vdev[DVS_TX_DEV_LEN];

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
    /* Audio TX (ST2110-30) is out of scope: dvs_tx_params_t carries video
     * only, so there is nothing to reject here yet. When an audio sub-object
     * lands, this entry must fail it explicitly without breaking video. */

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
    txp.gop_size = dvs_tx_default_gop(p->fps);
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
    return 0;
}
