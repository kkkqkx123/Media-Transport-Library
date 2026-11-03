#include "engine_ctx.h"

#include "mp4_dist_config.h"      /* mp4_dist_mtl_init, mode_dims, apply_drm_mode */
#include "shared/display_layout.h" /* mp4_dist_pip_validate, MP4_PIP_MAX_LANES */
#include "session_admit.h"         /* whitelist + LRU admission (S4) */
#include "audio_arbiter.h"         /* ALSA audio routing (S4) */

#include <mtl_api.h> /* mtl_ptp_read_time, mtl_stop, mtl_uninit */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void* engine_loop(void* arg) {
    dvs_engine* e = arg;
    int report_iv = e->cfg.stats_report_interval_s > 0 ? e->cfg.stats_report_interval_s
                     : e->cfg.e2e_report_interval_s > 0 ? e->cfg.e2e_report_interval_s
                                                        : 10;
    int t = e->cfg.test_time; /* <=0 => run until request_stop */
    int elapsed = 0;
    while (atomic_load(&e->running)) {
        sleep(1);
        elapsed++;
        if (report_iv > 0 && (elapsed % report_iv) == 0) {
            pthread_mutex_lock(&e->mu);
            if (e->comp)
                rx_compositor_report_stats(e->comp, report_iv, true);
            pthread_mutex_unlock(&e->mu);
        }
        if (t > 0) {
            t--;
            if (t == 0) break; /* bounded run finished */
        }
    }
    atomic_store(&e->running, false);
    return NULL;
}

dvs_engine* dvs_engine_create(const mp4_dist_cfg* base_cfg) {
    if (!base_cfg) return NULL;
    dvs_engine* e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->cfg = *base_cfg;
    e->st = NULL;
    e->comp = NULL;
    atomic_store(&e->running, false);
    atomic_store(&e->rx_sessions, 0);
    atomic_store(&e->decoding, 0);
    atomic_store(&e->layers, 0);
    pthread_mutex_init(&e->mu, NULL);
    return e;
}

void dvs_engine_destroy(dvs_engine* e) {
    if (!e) return;
    dvs_engine_stop(e);
    pthread_mutex_destroy(&e->mu);
    free(e);
}

int dvs_engine_init_mtl(dvs_engine* e) {
    if (!e) return -1;
    pthread_mutex_lock(&e->mu);
    if (e->st == NULL) {
        e->st = mp4_dist_mtl_init(&e->cfg, e->cfg.rx_port, e->cfg.rx_lip, 0, 1,
                                  e->cfg.mtl_log_level, e->cfg.mtl_lcores_rx);
        if (!e->st) {
            pthread_mutex_unlock(&e->mu);
            fprintf(stderr, "dvs_engine: MTL init failed\n");
            return -1;
        }
    }
    pthread_mutex_unlock(&e->mu);
    return 0;
}

static rx_compositor* build_compositor(dvs_engine* e,
                                       const mp4_dist_pip_stream* streams,
                                       int count) {
    mp4_dist_cfg* cfg = &e->cfg;
    memcpy(cfg->pip_streams, streams, (size_t)count * sizeof(*streams));
    cfg->pip_stream_count = count;

    /* S4 admission: enforce whitelist and refresh LRU for each source. Reject
     * the whole layout if any source is not whitelisted. */
    dvs_session_tick();
    for (int i = 0; i < count; i++) {
        int lane = 0;
        if (dvs_session_admit(cfg->pip_streams[i].sip,
                              cfg->pip_streams[i].udp_port, &lane) != 0) {
            fprintf(stderr, "dvs_engine: source %s:%u rejected by whitelist\n",
                    cfg->pip_streams[i].sip, cfg->pip_streams[i].udp_port);
            return NULL;
        }
    }

    if (cfg->width <= 0) cfg->width = 1920;
    if (cfg->height <= 0) cfg->height = 1080;
    if (cfg->fps <= 0.0f)
        cfg->fps = streams[0].fps > 0 ? streams[0].fps : 24.0f;
    mp4_dist_apply_drm_mode(cfg);

    int mode_w = 0, mode_h = 0;
    mp4_dist_mode_dims(cfg, &mode_w, &mode_h);
    if (mp4_dist_pip_validate(cfg->pip_streams, count, mode_w, mode_h) < 0) {
        fprintf(stderr, "dvs_engine: layout validation failed\n");
        return NULL;
    }

    rx_compositor_params cp = {
        .mt = e->st,
        .drm_device = cfg->drm_device,
        .drm_connector = cfg->drm_connector,
        .drm_mode = cfg->drm_mode,
        .drm_buf_count = cfg->drm_buf_count,
        .streams = cfg->pip_streams,
        .stream_count = count,
        .rx_port = cfg->rx_port,
        .alsa_device = cfg->alsa_device,
        .playout_delay_ms = cfg->playout_delay_ms,
        .test_time = cfg->test_time,
        .default_codestream_size = cfg->codestream_size,
        .default_audio_udp_port = cfg->audio_udp_port,
        .cpu_bridge = cfg->cpu_bridge,
        .cpu_decode = cfg->cpu_decode,
        .cpu_render = cfg->cpu_render,
        .cpu_audio = cfg->cpu_audio,
    };
    if (cfg->ptp_mode != MP4_DIST_PTP_MONOTONIC) {
        cp.time_now = (uint64_t(*)(void*))mtl_ptp_read_time;
        cp.time_priv = e->st;
    }
    return rx_compositor_create(&cp);
}

int dvs_engine_apply_layout(dvs_engine* e, const mp4_dist_pip_stream* streams,
                            int count) {
    if (!e) return -1;
    if (count < 0 || count > MP4_PIP_MAX_LANES) return -1;

    pthread_mutex_lock(&e->mu);

    if (e->comp) {
        rx_compositor_stop(e->comp);
        rx_compositor_free(e->comp);
        e->comp = NULL;
    }

    if (e->st == NULL) {
        e->st = mp4_dist_mtl_init(&e->cfg, e->cfg.rx_port, e->cfg.rx_lip, 0, 1,
                                  e->cfg.mtl_log_level, e->cfg.mtl_lcores_rx);
        if (!e->st) {
            pthread_mutex_unlock(&e->mu);
            fprintf(stderr, "dvs_engine: MTL init failed\n");
            return -1;
        }
    }

    if (count <= 0) {
        /* clear: media off, keep MTL up for fast future commits */
        atomic_store(&e->rx_sessions, 0);
        atomic_store(&e->decoding, 0);
        atomic_store(&e->layers, 0);
        pthread_mutex_unlock(&e->mu);
        return 0;
    }

    rx_compositor* comp = build_compositor(e, streams, count);
    if (!comp) {
        pthread_mutex_unlock(&e->mu);
        return -1;
    }
    if (rx_compositor_start(comp) != 0) {
        rx_compositor_free(comp);
        pthread_mutex_unlock(&e->mu);
        fprintf(stderr, "dvs_engine: compositor start failed\n");
        return -1;
    }
    e->comp = comp;
    atomic_store(&e->rx_sessions, count);
    atomic_store(&e->decoding, count);
    atomic_store(&e->layers, count);
    pthread_mutex_unlock(&e->mu);

    /* S4 audio: apply the pending audio owner/volume routing. */
    dvs_audio_set(e, e->audio_owner, e->audio_volume);

    if (!atomic_load(&e->running)) {
        atomic_store(&e->running, true);
        if (pthread_create(&e->loop_tid, NULL, engine_loop, e) != 0) {
            atomic_store(&e->running, false);
            fprintf(stderr, "dvs_engine: loop thread create failed\n");
            return -1;
        }
    }
    return 0;
}

int dvs_engine_start(dvs_engine* e) {
    if (!e) return -1;
    return dvs_engine_apply_layout(e, e->cfg.pip_streams, e->cfg.pip_stream_count);
}

void dvs_engine_request_stop(dvs_engine* e) {
    if (e) atomic_store(&e->running, false);
}

void dvs_engine_stop(dvs_engine* e) {
    if (!e) return;
    atomic_store(&e->running, false);
    if (e->loop_tid) {
        pthread_join(e->loop_tid, NULL);
        e->loop_tid = 0;
    }
    pthread_mutex_lock(&e->mu);
    if (e->comp) {
        rx_compositor_stop(e->comp);
        rx_compositor_free(e->comp);
        e->comp = NULL;
    }
    if (e->st) {
        mtl_stop(e->st);
        mtl_uninit(e->st);
        e->st = NULL;
    }
    pthread_mutex_unlock(&e->mu);
    atomic_store(&e->rx_sessions, 0);
    atomic_store(&e->decoding, 0);
    atomic_store(&e->layers, 0);
}

void dvs_engine_set_audio(dvs_engine* e, const char* owner, int volume) {
    if (!e) return;
    if (owner) {
        strncpy(e->audio_owner, owner, sizeof(e->audio_owner) - 1);
        e->audio_owner[sizeof(e->audio_owner) - 1] = '\0';
    } else {
        e->audio_owner[0] = '\0';
    }
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    e->audio_volume = volume;
}

void dvs_engine_counts(dvs_engine* e, int* rx_sessions, int* decoding,
                       int* layers) {
    if (!e) {
        if (rx_sessions) *rx_sessions = 0;
        if (decoding) *decoding = 0;
        if (layers) *layers = 0;
        return;
    }
    if (rx_sessions) *rx_sessions = atomic_load(&e->rx_sessions);
    if (decoding) *decoding = atomic_load(&e->decoding);
    if (layers) *layers = atomic_load(&e->layers);
}
