#include "live_ingest_config.h"
#include "slab_pool.h"
#include "mtl_tx.h"
#include "mtl_tx_audio.h"
#include "file/file_source.h"
#include "pipeline/tx_pipeline.h"
#include "ring_buffer.h"
#include <errno.h>
#include <mtl_api.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static atomic_bool g_run = true;
static void sig_handler(int s) { (void)s; atomic_store(&g_run, false); }

static void usage(const char* prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s [sec] [--config FILE] [--port P] [--lip IP] [--dip IP] [--udp PORT] [--audio-udp PORT] [--log-level L]\n", prog);
    fprintf(stderr, "  %s [sec] --source file --mp4 FILE [--loop/--no-loop] [--audio-udp PORT] [--use-tai]\n", prog);
}

static int parse_args(int argc, char** argv, struct live_ingest_cfg* cfg) {
    int i = 1;
    if (i < argc && argv[i][0] != '-') {
        cfg->test_time = atoi(argv[i]);
        i++;
    }
    while (i < argc) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            if (live_ingest_config_load(argv[++i], cfg) != 0) return -1;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            cfg->tx_port = argv[++i];
        } else if (strcmp(argv[i], "--lip") == 0 && i + 1 < argc) {
            cfg->tx_lip = argv[++i];
        } else if (strcmp(argv[i], "--dip") == 0 && i + 1 < argc) {
            cfg->tx_dip = argv[++i];
        } else if (strcmp(argv[i], "--udp") == 0 && i + 1 < argc) {
            cfg->udp_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--audio-udp") == 0 && i + 1 < argc) {
            cfg->audio_udp_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--source") == 0 && i + 1 < argc) {
            const char* s = argv[++i];
            cfg->file_source = (strcmp(s, "file") == 0);
        } else if (strcmp(argv[i], "--mp4") == 0 && i + 1 < argc) {
            const char* mp = argv[++i];
            if (cfg->mp4_count < LIVE_INGEST_MAX_FILES) {
                snprintf(cfg->mp4_files[cfg->mp4_count++], LIVE_INGEST_MAX_PATH,
                         "%s", mp);
                cfg->file_source = true;
            }
        } else if (strcmp(argv[i], "--loop") == 0) {
            cfg->loop = true;
        } else if (strcmp(argv[i], "--no-loop") == 0) {
            cfg->loop = false;
        } else if (strcmp(argv[i], "--use-tai") == 0) {
            cfg->use_tai = true;
        } else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            cfg->log_level = argv[++i];
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            return -1;
        }
        i++;
    }
    return 0;
}

static void mtl_set_common_flags(struct mtl_init_params* mtp) {
    mtp->flags |= MTL_FLAG_BIND_NUMA;
    mtp->flags |= MTL_FLAG_TX_VIDEO_MIGRATE;
    mtp->flags |= MTL_FLAG_RX_VIDEO_MIGRATE;
    mtp->flags |= MTL_FLAG_RX_SEPARATE_VIDEO_LCORE;
}

static void apply_log_level(mtl_handle st, const char* level) {
    if (!level || !st) return;
    if (strcmp(level, "debug") == 0)
        mtl_set_log_level(st, MTL_LOG_LEVEL_DEBUG);
    else if (strcmp(level, "notice") == 0)
        mtl_set_log_level(st, MTL_LOG_LEVEL_NOTICE);
    else if (strcmp(level, "warning") == 0)
        mtl_set_log_level(st, MTL_LOG_LEVEL_WARNING);
    else if (strcmp(level, "error") == 0)
        mtl_set_log_level(st, MTL_LOG_LEVEL_ERR);
    else
        mtl_set_log_level(st, MTL_LOG_LEVEL_INFO); /* default */
}

static enum mtl_pmd_type mtl_pmd_from_port(const char* port_name) {
    if (!port_name || !port_name[0]) return MTL_PMD_NATIVE_AF_XDP;
    if (strncmp(port_name, "kernel:", 7) == 0) return MTL_PMD_KERNEL_SOCKET;
    if (strncmp(port_name, "native_af_xdp:", 14) == 0)
        return MTL_PMD_NATIVE_AF_XDP;
    if (strncmp(port_name, "dpdk_af_xdp:", 12) == 0)
        return MTL_PMD_DPDK_AF_XDP;
    if (strncmp(port_name, "dpdk_af_packet:", 15) == 0)
        return MTL_PMD_DPDK_AF_PACKET;
    return MTL_PMD_DPDK_USER;
}

/* ---- TX path: tx_pipeline → mtl_tx (direct-write, no fill thread) ---- */
struct tx_stack {
    mtl_handle          st;
    struct tx_pipeline* tp;
    struct mtl_tx*      mtx;
    struct tx_mtl_sink_ops mtl_sink;
};

static int tx_mtl_acquire(void* priv, uint16_t* idx, void** addr) {
    return mtl_tx_acquire_fb((struct mtl_tx*)priv, idx, addr);
}

static int tx_mtl_publish(void* priv, uint16_t idx, size_t size) {
    return mtl_tx_publish_fb((struct mtl_tx*)priv, idx, size);
}

static void tx_mtl_abort(void* priv, uint16_t idx) {
    mtl_tx_abort_fb((struct mtl_tx*)priv, idx);
}

static int tx_stack_create(struct tx_stack* ts, struct live_ingest_cfg* cfg,
                           mtl_handle st) {
    memset(ts, 0, sizeof(*ts));
    ts->st = st;
    if (!ts->st) return -1;

    size_t fb_max = cfg->codestream_size ? cfg->codestream_size : 512 * 1024;
    struct mtl_tx_params mtxp = {
        .st = ts->st,
        .framebuff_cnt = 8,
        .framebuff_max = fb_max,
        .width = cfg->width,
        .height = cfg->height,
        .fps = cfg->fps,
        .udp_port = cfg->udp_port,
        .dip_addr = cfg->tx_dip,
        .port_name = cfg->tx_port,
        .use_tai_timestamp = cfg->use_tai,
    };
    ts->mtx = mtl_tx_create(&mtxp);
    if (!ts->mtx) return -1;

    ts->mtl_sink.priv       = ts->mtx;
    ts->mtl_sink.acquire_fb = tx_mtl_acquire;
    ts->mtl_sink.publish_fb = tx_mtl_publish;
    ts->mtl_sink.abort_fb   = tx_mtl_abort;

    struct tx_pipeline_params txp = {
        .v4l2_device = cfg->v4l2_device,
        .width = cfg->width,
        .height = cfg->height,
        .fps = cfg->fps,
        .v4l2_buf_count = 6,
        .drm_device = cfg->drm_device,
        .rga_pool_size = 12,
        .bitrate_bps = cfg->bitrate_bps,
        .gop_size = cfg->gop_size,
        .num_slices = cfg->num_slices,
        .intra_refresh_mode = cfg->intra_refresh_mode,
        .intra_refresh_arg  = cfg->intra_refresh_arg,
        .coded_blk_size = 512 * 1024,
        .ring_size = 2,
        .cpu_capture = cfg->cpu_capture,
        .cpu_encode = cfg->cpu_encode,
        .time_now = (uint64_t(*)(void*))mtl_ptp_read_time,
        .time_priv = ts->st,
        .mtl_sink = &ts->mtl_sink,
    };
    ts->tp = tx_pipeline_create(&txp);
    if (!ts->tp) return -errno;

    printf("TX: port=%s lip=%s dip=%s udp=%u (direct-write)\n",
           cfg->tx_port, cfg->tx_lip, cfg->tx_dip, cfg->udp_port);
    return 0;
}

static int tx_stack_start(struct tx_stack* ts) {
    if (mtl_tx_start(ts->mtx) != 0) return -1;
    if (tx_pipeline_start(ts->tp) != 0) {
        mtl_tx_stop(ts->mtx);
        return -1;
    }
    return 0;
}

static void tx_stack_free(struct tx_stack* ts) {
    if (!ts) return;
    if (ts->tp) { tx_pipeline_stop(ts->tp); tx_pipeline_free(ts->tp); }
    if (ts->mtx) { mtl_tx_stop(ts->mtx); mtl_tx_free(ts->mtx); }
    if (ts->st) { mtl_stop(ts->st); mtl_uninit(ts->st); }
}

/* ---- File path: file_source → mtl_tx (video) + mtl_tx_audio ---- */
struct file_stack {
    mtl_handle st;
    struct mtl_tx* vtx;
    struct mtl_tx_audio* atx;
    struct file_source* fs;
    struct tx_mtl_sink_ops vsink;
};

static int file_video_acquire(void* priv, uint16_t* idx, void** addr) {
    return mtl_tx_acquire_fb((struct mtl_tx*)priv, idx, addr);
}

static int file_video_publish(void* priv, uint16_t idx, size_t size) {
    return mtl_tx_publish_fb((struct mtl_tx*)priv, idx, size);
}

static void file_video_abort(void* priv, uint16_t idx) {
    mtl_tx_abort_fb((struct mtl_tx*)priv, idx);
}

static int file_stack_create(struct file_stack* fss, struct live_ingest_cfg* cfg,
                             mtl_handle st) {
    memset(fss, 0, sizeof(*fss));
    fss->st = st;
    if (!fss->st) return -1;
    if (cfg->mp4_count <= 0) {
        fprintf(stderr, "file_source: no MP4 file (--mp4 or mp4_file)\n");
        return -EINVAL;
    }
    int pw = 0, ph = 0;
    float pfps = 0.0f;
    if (file_source_probe(cfg->mp4_files[0], &pw, &ph, &pfps) == 0) {
        if (pw > 0 && ph > 0) {
            if (cfg->width != pw || cfg->height != ph)
                printf("file_source: probe %s → %dx%d (cfg %dx%d), align to file\n",
                       cfg->mp4_files[0], pw, ph, cfg->width, cfg->height);
            cfg->width = pw;
            cfg->height = ph;
        }
        if (pfps > 0.0f) {
            if (cfg->fps <= 0.0f || (cfg->fps > 60.0f - 0.01f && cfg->fps < 60.0f + 0.01f))
                cfg->fps = pfps;
            else if (cfg->fps != pfps)
                fprintf(stderr, "file_source: fps mismatch cfg=%.3f file=%.3f, use cfg\n",
                        (double)cfg->fps, (double)pfps);
            else
                cfg->fps = pfps;
        }
        printf("file_source: probe %s → %dx%d@%.3ffps\n",
               cfg->mp4_files[0], cfg->width, cfg->height, (double)cfg->fps);
    }
    size_t fb_max = cfg->codestream_size ? cfg->codestream_size : 512 * 1024;
    struct mtl_tx_params mtxp = {
        .st = fss->st,
        .framebuff_cnt = 8,
        .framebuff_max = fb_max,
        .width = cfg->width,
        .height = cfg->height,
        .fps = cfg->fps,
        .udp_port = cfg->udp_port,
        .dip_addr = cfg->tx_dip,
        .port_name = cfg->tx_port,
        .use_tai_timestamp = cfg->use_tai,
    };
    fss->vtx = mtl_tx_create(&mtxp);
    if (!fss->vtx) return -1;
    fss->vsink.priv = fss->vtx;
    fss->vsink.acquire_fb = file_video_acquire;
    fss->vsink.publish_fb = file_video_publish;
    fss->vsink.abort_fb = file_video_abort;

    struct mtl_tx_audio_params atxp = {
        .st = fss->st,
        .framebuff_cnt = 32,
        .udp_port = cfg->audio_udp_port,
        .dip_addr = cfg->tx_dip,
        .port_name = cfg->tx_port,
        .fmt = ST30_FMT_PCM16,
        .sampling = ST30_SAMPLING_48K,
        .ptime = ST30_PTIME_1MS,
        .channel = 2,
        .use_tai_timestamp = cfg->use_tai,
    };
    fss->atx = mtl_tx_audio_create(&atxp);
    if (!fss->atx) {
        fprintf(stderr, "file_source: audio TX unavailable, video-only\n");
    }

    struct file_source_params fsp = {0};
    fsp.st = fss->st;
    fsp.video_sink = &fss->vsink;
    fsp.audio_tx = fss->atx;
    for (int i = 0; i < cfg->mp4_count && i < FILE_SOURCE_MAX_FILES; i++)
        snprintf(fsp.files[i], sizeof(fsp.files[i]), "%s", cfg->mp4_files[i]);
    fsp.file_count = cfg->mp4_count;
    fsp.loop = cfg->loop;
    fsp.seek_ms = cfg->seek_ms;
    fsp.codestream_max = fb_max;
    fsp.width = cfg->width;
    fsp.height = cfg->height;
    fsp.fps = cfg->fps;
    fss->fs = file_source_create(&fsp);
    if (!fss->fs) {
        if (fss->atx) { mtl_tx_audio_free(fss->atx); fss->atx = NULL; }
        mtl_tx_free(fss->vtx);
        fss->vtx = NULL;
        return -1;
    }
    printf("FILE: %d file(s) %dx%d@%.3f udp=%u audio_udp=%u%s loop=%s\n",
           cfg->mp4_count, cfg->width, cfg->height, (double)cfg->fps,
           cfg->udp_port, cfg->audio_udp_port,
           cfg->use_tai ? " tai-ts" : "", cfg->loop ? "on" : "off");
    return 0;
}

static int file_stack_start(struct file_stack* fss) {
    if (mtl_tx_start(fss->vtx) != 0) return -1;
    if (fss->atx && mtl_tx_audio_start(fss->atx) != 0) return -1;
    if (file_source_start(fss->fs, &g_run) != 0) {
        if (fss->atx) mtl_tx_audio_stop(fss->atx);
        mtl_tx_stop(fss->vtx);
        return -1;
    }
    return 0;
}

static void file_stack_free(struct file_stack* fss) {
    if (!fss) return;
    if (fss->fs) { file_source_stop(fss->fs); file_source_free(fss->fs); fss->fs = NULL; }
    if (fss->atx) { mtl_tx_audio_stop(fss->atx); mtl_tx_audio_free(fss->atx); fss->atx = NULL; }
    if (fss->vtx) { mtl_tx_stop(fss->vtx); mtl_tx_free(fss->vtx); fss->vtx = NULL; }
    if (fss->st) { mtl_stop(fss->st); mtl_uninit(fss->st); fss->st = 0; }
}

int main(int argc, char** argv) {
    signal(SIGINT, sig_handler);

    struct live_ingest_cfg cfg = live_ingest_cfg_defaults();
    if (parse_args(argc, argv, &cfg) != 0) { usage(argv[0]); return 1; }

    struct mtl_init_params mtp = {0};
    mtp.num_ports = 1;
    strncpy(mtp.port[MTL_PORT_P], cfg.tx_port, MTL_PORT_MAX_LEN - 1);
    mtp.pmd[MTL_PORT_P] = mtl_pmd_from_port(cfg.tx_port);
    mtp.net_proto[MTL_PORT_P] = MTL_PROTO_STATIC;
    strncpy((char*)mtp.sip_addr[MTL_PORT_P], cfg.tx_lip, MTL_IP_ADDR_LEN - 1);
    /* r8168 single-queue: native_af_xdp needs rx_queues_cnt==1 to select 1q path. */
    mtp.tx_queues_cnt[MTL_PORT_P] = 1;
    mtp.rx_queues_cnt[MTL_PORT_P] = 1;
    mtl_set_common_flags(&mtp);
    mtp.flags |= MTL_FLAG_SHARED_RX_QUEUE;
    mtp.flags |= MTL_FLAG_SHARED_TX_QUEUE;

    mtl_handle st = mtl_init(&mtp);
    if (!st) return 1;
    if (mtl_start(st) != 0) { mtl_uninit(st); return 1; }
    printf("MTL: %s (lip=%s) started\n", cfg.tx_port, cfg.tx_lip);
    apply_log_level(st, cfg.log_level);

    if (cfg.file_source) {
        struct file_stack fss;
        if (file_stack_create(&fss, &cfg, st) != 0) {
            mtl_stop(st);
            mtl_uninit(st);
            return 1;
        }
        if (file_stack_start(&fss) != 0) {
            file_stack_free(&fss);
            return 1;
        }
        int t = cfg.test_time;
        while (atomic_load(&g_run) && t > 0) {
            sleep(1);
            t--;
        }
        file_stack_free(&fss);
        return 0;
    }

    struct tx_stack ts;
    int rc = tx_stack_create(&ts, &cfg, st);
    while (rc == -ENODATA && atomic_load(&g_run)) {
        if (ts.mtx) { mtl_tx_free(ts.mtx); ts.mtx = NULL; }
        fprintf(stderr, "live_ingest: no HDMI signal, retry in 1s\n");
        sleep(1);
        memset(&ts, 0, sizeof(ts));
        rc = tx_stack_create(&ts, &cfg, st);
    }
    if (rc != 0) { tx_stack_free(&ts); return 1; }
    if (tx_stack_start(&ts) != 0) { tx_stack_free(&ts); return 1; }
    int t = cfg.test_time;
    while (atomic_load(&g_run) && t > 0) {
        sleep(1);
        t--;
    }
    tx_stack_free(&ts);
    return 0;
}
