#include "engine_ctx.h"

#include <json-c/json.h>

#include "file_playout_config.h"      /* file_playout_mtl_init, mode_dims, apply_drm_mode */
#include "shared/display_layout.h" /* file_playout_pip_validate, MP4_PIP_MAX_LANES */
#include "session_admit.h"         /* whitelist + LRU admission */
#include "audio_arbiter.h"         /* ALSA audio routing */
#include "capability.h"

#include <mtl_api.h> /* mtl_ptp_read_time, mtl_stop, mtl_uninit */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

dvs_engine* dvs_engine_create(const file_playout_cfg* base_cfg) {
    if (!base_cfg) return NULL;
    dvs_engine* e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->cfg = *base_cfg;
    e->st = NULL;
    e->comp = NULL;
    snprintf(e->role, sizeof(e->role), "output");
    dvs_tx_init(&e->tx);
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

void dvs_capability_refresh_mtl(dvs_capability* cap, void* mtl_handle) {
    if (!cap) return;
    const char* v = mtl_version();
    if (v && v[0]) {
        snprintf(cap->mtl_version, sizeof(cap->mtl_version), "%s", v);
        snprintf(cap->mtl_backend, sizeof(cap->mtl_backend), "dpdk");
    }
    if (mtl_handle) {
        uint64_t t = mtl_ptp_read_time(mtl_handle);
        if (t != 0) {
            snprintf(cap->ptp_state, sizeof(cap->ptp_state), "synced");
            cap->ptp_offset_us = 0;
        }
    }
}

static uint64_t engine_now_ns(dvs_engine* e) {
    if (e->st && e->cfg.ptp_mode != FILE_PLAYOUT_PTP_MONOTONIC)
        return mtl_ptp_read_time(e->st);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void engine_wait_until(dvs_engine* e, uint64_t target_ns) {
    while (atomic_load(&e->running)) {
        uint64_t now = engine_now_ns(e);
        if (now >= target_ns) break;
        uint64_t left = target_ns - now;
        struct timespec ts;
        if (left > 2000000ULL) {
            ts.tv_sec = 0;
            ts.tv_nsec = 2000000L;
        } else {
            ts.tv_sec = 0;
            ts.tv_nsec = (long)left;
        }
        nanosleep(&ts, NULL);
    }
}

int dvs_engine_init_mtl(dvs_engine* e) {
    if (!e) return -1;
    pthread_mutex_lock(&e->mu);
    if (e->st == NULL) {
        e->st = file_playout_mtl_init(&e->cfg, e->cfg.rx_port, e->cfg.rx_lip, 0, 1,
                                  e->cfg.mtl_log_level, e->cfg.mtl_lcores_rx);
        if (!e->st) {
            pthread_mutex_unlock(&e->mu);
            fprintf(stderr, "dvs_engine: MTL init failed\n");
            return -1;
        }
        fprintf(stderr, "dvs_engine: MTL %s up\n", mtl_version());
    }
    pthread_mutex_unlock(&e->mu);
    return 0;
}

static rx_compositor* build_compositor(dvs_engine* e,
                                       const file_playout_pip_stream* streams,
                                       int count) {
    file_playout_cfg* cfg = &e->cfg;
    memcpy(cfg->pip_streams, streams, (size_t)count * sizeof(*streams));
    cfg->pip_stream_count = count;

    /* Admission: enforce whitelist and refresh LRU for each source. Reject
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
    file_playout_apply_drm_mode(cfg);

    int mode_w = 0, mode_h = 0;
    file_playout_mode_dims(cfg, &mode_w, &mode_h);
    if (file_playout_pip_validate(cfg->pip_streams, count, mode_w, mode_h) < 0) {
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
    if (cfg->ptp_mode != FILE_PLAYOUT_PTP_MONOTONIC) {
        cp.time_now = (uint64_t(*)(void*))mtl_ptp_read_time;
        cp.time_priv = e->st;
    }
    return rx_compositor_create(&cp);
}

int dvs_engine_apply_layout(dvs_engine* e, const file_playout_pip_stream* streams,
                            int count) {
    return dvs_engine_apply_layout_at(e, streams, count, 0);
}

int dvs_engine_apply_layout_at(dvs_engine* e,
                               const file_playout_pip_stream* streams, int count,
                               uint64_t apply_at_ns) {
    if (!e) return -1;
    if (count < 0 || count > MP4_PIP_MAX_LANES) return -1;

    pthread_mutex_lock(&e->mu);

    if (e->st == NULL) {
        e->st = file_playout_mtl_init(&e->cfg, e->cfg.rx_port, e->cfg.rx_lip, 0, 1,
                                  e->cfg.mtl_log_level, e->cfg.mtl_lcores_rx);
        if (!e->st) {
            pthread_mutex_unlock(&e->mu);
            fprintf(stderr, "dvs_engine: MTL init failed\n");
            return -1;
        }
        fprintf(stderr, "dvs_engine: MTL %s up\n", mtl_version());
    }

    if (count <= 0) {
        /* clear: media off, keep MTL up for fast future commits */
        dvs_tx_stop(&e->tx);
        file_playout_pip_stream old[MP4_PIP_MAX_LANES];
        int old_n = e->cfg.pip_stream_count;
        if (old_n > MP4_PIP_MAX_LANES) old_n = MP4_PIP_MAX_LANES;
        memcpy(old, e->cfg.pip_streams, (size_t)old_n * sizeof(*old));
        if (e->comp) {
            rx_compositor_stop(e->comp);
            rx_compositor_free(e->comp);
            e->comp = NULL;
        }
        e->cfg.pip_stream_count = 0;
        for (int i = 0; i < old_n; i++)
            dvs_session_release(old[i].sip, old[i].udp_port);
        atomic_store(&e->rx_sessions, 0);
        atomic_store(&e->decoding, 0);
        atomic_store(&e->layers, 0);
        pthread_mutex_unlock(&e->mu);
        return 0;
    }

    if (e->comp) {
        int fr = rx_compositor_try_fast_apply(e->comp, streams, count);
        if (fr == 0) {
            file_playout_pip_stream old[MP4_PIP_MAX_LANES];
            int old_n = e->cfg.pip_stream_count;
            if (old_n > MP4_PIP_MAX_LANES) old_n = MP4_PIP_MAX_LANES;
            memcpy(old, e->cfg.pip_streams, (size_t)old_n * sizeof(*old));
            if (apply_at_ns != 0) {
                pthread_mutex_unlock(&e->mu);
                engine_wait_until(e, apply_at_ns);
                pthread_mutex_lock(&e->mu);
                if (!e->comp) {
                    pthread_mutex_unlock(&e->mu);
                    return -1;
                }
            }
            memcpy(e->cfg.pip_streams, streams,
                   (size_t)count * sizeof(*streams));
            e->cfg.pip_stream_count = count;
            for (int i = 0; i < old_n; i++) {
                bool gone = true;
                for (int j = 0; j < count; j++) {
                    if (strcmp(old[i].sip, streams[j].sip) == 0 &&
                        old[i].udp_port == streams[j].udp_port) {
                        gone = false;
                        break;
                    }
                }
                if (gone) dvs_session_release(old[i].sip, old[i].udp_port);
            }
            atomic_store(&e->rx_sessions, count);
            atomic_store(&e->decoding, count);
            atomic_store(&e->layers, count);
            pthread_mutex_unlock(&e->mu);
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
        if (fr < 0) {
            pthread_mutex_unlock(&e->mu);
            fprintf(stderr, "dvs_engine: layout validation failed\n");
            return -1;
        }
        rx_compositor_stop(e->comp);
        rx_compositor_free(e->comp);
        e->comp = NULL;
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

    /* Apply the pending audio owner/volume routing. */
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

int dvs_engine_set_output(dvs_engine* e, const char* connector, int width,
                          int height, int refresh_hz, const char* mode) {
    if (!e) return -1;
    if (width < 640 || width > 7680 || height < 480 || height > 4320) return -1;
    if (refresh_hz < 24 || refresh_hz > 120) return -1;
    if (!mode) mode = "force";
    if (strcmp(mode, "force") != 0 && strcmp(mode, "hotplug") != 0) return -1;
    pthread_mutex_lock(&e->mu);
    if (connector && connector[0]) {
        snprintf(e->cfg.drm_connector, sizeof(e->cfg.drm_connector), "%s", connector);
    }
    e->cfg.width = width;
    e->cfg.height = height;
    e->cfg.fps = (float)refresh_hz;
    if (strcmp(mode, "hotplug") == 0) {
        snprintf(e->cfg.drm_mode, sizeof(e->cfg.drm_mode), "auto");
    } else {
        snprintf(e->cfg.drm_mode, sizeof(e->cfg.drm_mode), "%dx%d@%d", width, height, refresh_hz);
    }
    int has_comp = (e->comp != NULL);
    file_playout_pip_stream cur[MP4_PIP_MAX_LANES];
    int cur_n = e->cfg.pip_stream_count;
    if (cur_n > MP4_PIP_MAX_LANES) cur_n = MP4_PIP_MAX_LANES;
    if (cur_n > 0) memcpy(cur, e->cfg.pip_streams, (size_t)cur_n * sizeof(*cur));
    pthread_mutex_unlock(&e->mu);
    if (has_comp && cur_n > 0) return dvs_engine_apply_layout(e, cur, cur_n);
    return 0;
}

int dvs_engine_set_role(dvs_engine* e, const char* role) {
    if (!e || !role) return -1;
    if (strcmp(role, "input") == 0) {
        snprintf(e->role, sizeof(e->role), "input");
        return dvs_engine_apply_layout(e, NULL, 0);
    }
    if (strcmp(role, "output") == 0) {
        snprintf(e->role, sizeof(e->role), "output");
        pthread_mutex_lock(&e->mu);
        dvs_tx_stop(&e->tx);
        pthread_mutex_unlock(&e->mu);
        return 0;
    }
    return -1;
}

int dvs_engine_tx_start(dvs_engine* e, const dvs_tx_params_t* params,
                        char* err, size_t errlen) {
    if (!e || !params) {
        if (err && errlen > 0) snprintf(err, errlen, "missing tx params");
        return -1;
    }
    pthread_mutex_lock(&e->mu);
    /* Role mutual exclusion: an input box never runs the RX compositor. */
    if (e->comp) {
        rx_compositor_stop(e->comp);
        rx_compositor_free(e->comp);
        e->comp = NULL;
    }
    e->cfg.pip_stream_count = 0;
    int rc = dvs_tx_start(&e->tx, params, e->role, err, errlen);
    pthread_mutex_unlock(&e->mu);
    return rc;
}

void dvs_engine_tx_stop(dvs_engine* e) {
    if (!e) return;
    pthread_mutex_lock(&e->mu);
    dvs_tx_stop(&e->tx);
    pthread_mutex_unlock(&e->mu);
}

struct json_object* dvs_engine_tx_status(dvs_engine* e) {
    if (!e) return dvs_tx_status_json(NULL);
    pthread_mutex_lock(&e->mu);
    struct json_object* o = dvs_tx_status_json(&e->tx);
    pthread_mutex_unlock(&e->mu);
    return o ? o : dvs_tx_status_json(NULL);
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
    dvs_tx_stop(&e->tx);
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

struct json_object* dvs_engine_lane_stats(dvs_engine* e) {
    struct json_object* arr = json_object_new_array();
    if (!arr) return NULL;
    if (!e) return arr;

    pthread_mutex_lock(&e->mu);
    rx_compositor* comp = e->comp;
    pthread_mutex_unlock(&e->mu);

    if (comp) {
        int n = rx_compositor_lane_count(comp);
        for (int i = 0; i < n; i++) {
            const file_playout_pip_stream* st = rx_compositor_lane_stream(comp, i);
            if (!st) continue;
            struct json_object* o = json_object_new_object();
            json_object_object_add(o, "lane", json_object_new_int(st->lane));
            json_object_object_add(o, "sip", json_object_new_string(st->sip));
            json_object_object_add(o, "udp_port", json_object_new_int(st->udp_port));
            json_object_object_add(o, "ready",
                                  json_object_new_boolean(rx_compositor_lane_running(comp, i)));
            json_object_object_add(o, "lostFrames",
                                  json_object_new_int64(rx_compositor_lane_lost_frames(comp, i)));
            json_object_object_add(o, "lastPtsNs",
                                  json_object_new_int64(rx_compositor_lane_last_pts_ns(comp, i)));
            json_object_array_add(arr, o);
        }
        return arr;
    }

    char keys[DVS_MAX_SESSIONS][DVS_SESSION_KEY_LEN];
    int lanes[DVS_MAX_SESSIONS];
    int n = dvs_session_snapshot(keys, lanes, DVS_MAX_SESSIONS);
    for (int i = 0; i < n; i++) {
        char* colon = strrchr(keys[i], ':');
        char sip[64] = {0};
        int port = 0;
        if (colon) {
            size_t len = (size_t)(colon - keys[i]);
            if (len >= sizeof(sip)) len = sizeof(sip) - 1;
            memcpy(sip, keys[i], len);
            port = atoi(colon + 1);
        } else {
            snprintf(sip, sizeof(sip), "%.63s", keys[i]);
        }
        struct json_object* o = json_object_new_object();
        json_object_object_add(o, "lane", json_object_new_int(lanes[i]));
        json_object_object_add(o, "sip", json_object_new_string(sip));
        json_object_object_add(o, "udp_port", json_object_new_int(port));
        json_object_object_add(o, "ready", json_object_new_boolean(0));
        json_object_object_add(o, "lostFrames", json_object_new_int64(0));
        json_object_object_add(o, "lastPtsNs", json_object_new_int64(0));
        json_object_array_add(arr, o);
    }
    return arr;
}
