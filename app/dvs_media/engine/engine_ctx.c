#include "engine_ctx.h"

#include <json-c/json.h>

#include "file_playout_config.h"      /* file_playout_mtl_init_dual, mode_dims, apply_drm_mode */
#include "engine/tx_backend.h"
#include "display_layout.h" /* file_playout_pip_validate, MP4_PIP_MAX_LANES */
#include "session_admit.h"         /* whitelist + LRU admission */
#include "audio_arbiter.h"         /* ALSA audio routing */
#include "base_image.h"            /* wall still decode */
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
#if defined(__linux__) && defined(_GNU_SOURCE)
    pthread_setname_np(pthread_self(), "dvs_loop");
#endif
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
    dvs_tx_set_backend(&e->tx, dvs_tx_backend_entry);
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
    free(e->base_px);
    e->base_px = NULL;
    free(e);
}

void dvs_capability_refresh_mtl(dvs_capability* cap, void* mtl_handle) {
    if (!cap) return;
    if (mtl_handle) {
        const char* v = mtl_version();
        if (v && v[0])
            snprintf(cap->mtl_version, sizeof(cap->mtl_version), "%s", v);
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

/* Recompute the live counters from the running compositor. Caller holds mu. */
static void engine_refresh_counts_locked(dvs_engine* e) {
    if (!e->comp) {
        atomic_store(&e->rx_sessions, dvs_session_count());
        atomic_store(&e->decoding, 0);
        atomic_store(&e->layers, 0);
        return;
    }
    atomic_store(&e->rx_sessions, rx_compositor_lane_count(e->comp));
    atomic_store(&e->decoding, rx_compositor_ready_count(e->comp));
    atomic_store(&e->layers, rx_compositor_layer_count(e->comp));
}

static void engine_release_gone(const file_playout_pip_stream* old_streams,
                                int old_n,
                                const file_playout_pip_stream* new_streams,
                                int new_n) {
    for (int i = 0; i < old_n; i++) {
        bool gone = true;
        for (int j = 0; j < new_n; j++) {
            if (strcmp(old_streams[i].sip, new_streams[j].sip) == 0 &&
                old_streams[i].udp_port == new_streams[j].udp_port) {
                gone = false;
                break;
            }
        }
        if (gone) dvs_session_release(old_streams[i].sip, old_streams[i].udp_port);
    }
}

/* Wait for the shadow compositor to produce its first ready lane. */
static int engine_shadow_wait_ready(rx_compositor* shadow) {
    for (int i = 0; i < 50; i++) {
        if (rx_compositor_ready_count(shadow) > 0) return 0;
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000L};
        nanosleep(&ts, NULL);
    }
    return rx_compositor_ready_count(shadow) > 0 ? 0 : -1;
}

static void engine_push_tx_ctx(dvs_engine* e) {
    dvs_tx_backend_cfg bc;
    memset(&bc, 0, sizeof(bc));
    snprintf(bc.tx_port, sizeof(bc.tx_port), "%s", e->cfg.tx_port);
    snprintf(bc.drm_device, sizeof(bc.drm_device), "%s", e->cfg.drm_device);
    snprintf(bc.alsa_device, sizeof(bc.alsa_device), "%s", e->cfg.alsa_device);
    /* No capture/encode affinity in file_playout_cfg; 0 selects the backend
     * engineering defaults. */
    bc.cpu_capture = 0;
    bc.cpu_encode = 0;
    dvs_tx_backend_set_ctx(e->st, &bc);
}

int dvs_engine_init_mtl(dvs_engine* e) {
    if (!e) return -1;
    pthread_mutex_lock(&e->mu);
    if (e->st == NULL) {
        e->st = file_playout_mtl_init_dual(&e->cfg, 1, 1,
                                           e->cfg.mtl_log_level, NULL);
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
    if (count > 0) {
        if (!streams) return NULL;
        memcpy(cfg->pip_streams, streams, (size_t)count * sizeof(*streams));
    }
    cfg->pip_stream_count = count;

    if (cfg->width <= 0) cfg->width = 1920;
    if (cfg->height <= 0) cfg->height = 1080;
    if (cfg->fps <= 0.0f) {
        if (count > 0 && streams)
            cfg->fps = streams[0].fps > 0 ? streams[0].fps : 24.0f;
        else
            cfg->fps = 24.0f;
    }
    file_playout_apply_drm_mode(cfg);

    int mode_w = 0, mode_h = 0;
    file_playout_mode_dims(cfg, &mode_w, &mode_h);
    if (count > 0 &&
        file_playout_pip_validate(cfg->pip_streams, count, mode_w, mode_h) < 0) {
        fprintf(stderr, "dvs_engine: layout validation failed\n");
        return NULL;
    }

    /* Admission after validation: a rejected layout never pollutes the LRU.
     * Any source failing the whitelist rejects the whole layout. Empty
     * layouts skip both (backdrop-only: nothing to receive). */
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
    return dvs_engine_apply_layout_frame(e, streams, count, apply_at_ns,
                                          0, 60.0, -1);
}

/* Frame number math shared with the platform (same truncation):
 * frame(t) = floor((t - epoch) * fps). */
static int64_t engine_frame_at_ns(uint64_t epoch_ns, double fps,
                                  uint64_t now_ns) {
    if (epoch_ns == 0 || !(fps > 0.0) || now_ns <= epoch_ns) return 0;
    double f = (double)(now_ns - epoch_ns) / 1000000000.0 * fps;
    if (!(f >= 0.0)) return 0;
    if (f > 9007199254740991.0) return 9007199254740991LL;
    return (int64_t)f;
}

static uint64_t engine_frame_start_ns(uint64_t epoch_ns, double fps,
                                      int64_t frame) {
    if (epoch_ns == 0 || !(fps > 0.0) || frame <= 0) return epoch_ns;
    double s = (double)frame / fps;
    if (!(s >= 0.0)) return epoch_ns;
    /* Round like the platform helper so both sides name the same instant. */
    return epoch_ns + (uint64_t)(s * 1000000000.0 + 0.5);
}

static void engine_set_fail(dvs_engine* e, const char* code) {
    if (!e) return;
    snprintf(e->last_fail, sizeof(e->last_fail), "%s", code ? code : "");
}

/* Push the cached still + adjustment to the current compositor. Called
 * after every successful swap (the renderer is recreated each time).
 * Caller holds mu. */
static void engine_push_display_locked(dvs_engine* e) {
    if (!e || !e->comp) return;
    /* Still: reload when the staged revision moved, else re-push the cache
     * (a fresh renderer starts black). */
    if (e->pending_base.revision > 0 &&
        e->pending_base.revision != e->base_revision) {
        snprintf(e->base_state, sizeof(e->base_state), "loading");
        e->base_error[0] = '\0';
        int mw = 0, mh = 0;
        file_playout_mode_dims(&e->cfg, &mw, &mh);
        if (mw <= 0) mw = 1920;
        if (mh <= 0) mh = 1080;
        /* The agent downloads to a file and passes its path; the url alone
         * is not fetchable from C. */
        unsigned char* px = NULL;
        char err[128] = {0};
        int rc = -1;
        if (e->pending_base.path[0])
            rc = dvs_base_decode(e->pending_base.path, e->pending_base.sub_x,
                                 e->pending_base.sub_y, e->pending_base.sub_w,
                                 e->pending_base.sub_h, mw, mh, &px,
                                 err, sizeof(err));
        else
            snprintf(err, sizeof(err), "still path missing, fetch pending");
        if (rc == 0 && px) {
            free(e->base_px);
            e->base_px = px;
            e->base_px_w = mw;
            e->base_px_h = mh;
            e->base_revision = e->pending_base.revision;
            snprintf(e->base_state, sizeof(e->base_state), "ready");
        } else {
            snprintf(e->base_state, sizeof(e->base_state), "failed");
            snprintf(e->base_error, sizeof(e->base_error), "%s",
                     err[0] ? err : "still decode failed");
        }
    }
    if (e->base_px && e->base_px_w > 0 && e->base_px_h > 0) {
        if (rx_compositor_set_background(e->comp, e->base_px, e->base_px_w,
                                         e->base_px_h) != 0) {
            snprintf(e->base_state, sizeof(e->base_state), "unsupported");
            snprintf(e->base_error, sizeof(e->base_error),
                     "display backend refused still");
        } else if (strcmp(e->base_state, "ready") != 0 &&
                   e->base_revision > 0) {
            snprintf(e->base_state, sizeof(e->base_state), "ready");
            e->base_error[0] = '\0';
        }
    }
    /* Adjustment: recorded for query; the dumb-buffer background path has
     * no plane properties, so application is reported as unsupported. */
    if (e->pending_adjust.has_value) {
        e->applied_adjust = e->pending_adjust;
        e->adjust_supported = 0;
        e->pending_adjust.has_value = 0;
    }
}

/* Record the commit report right after a successful swap. */
static void engine_record_commit_locked(dvs_engine* e, uint64_t epoch_ns,
                                        double fps, int64_t target_frame) {
    uint64_t now = engine_now_ns(e);
    e->commit_applied_at = now;
    if (epoch_ns > 0 && target_frame >= 0) {
        int64_t actual = engine_frame_at_ns(epoch_ns, fps, now);
        e->commit_frame = actual;
        e->commit_late = (actual != target_frame) ? 1 : 0;
    } else {
        e->commit_frame = 0;
        e->commit_late = 0;
    }
    e->last_fail[0] = '\0';
}

int dvs_engine_apply_layout_frame(dvs_engine* e,
                                  const file_playout_pip_stream* streams,
                                  int count, uint64_t apply_at_ns,
                                  uint64_t epoch_ns, double fps,
                                  int64_t frame_index) {
    if (!e) return -1;
    if (count < 0 || count > MP4_PIP_MAX_LANES) return -1;
    if (count > 0 && !streams) return -1;

    /* Resolve the wait target: an anchored commit fires at its frame start
     * so every node swaps on the same instant; a legacy commit keeps the
     * bare-timestamp wait. */
    uint64_t wait_ns = apply_at_ns;
    if (epoch_ns > 0 && fps > 0.0 && frame_index >= 0)
        wait_ns = engine_frame_start_ns(epoch_ns, fps, frame_index);

    pthread_mutex_lock(&e->mu);

    if (count > 0 && strcmp(e->role, "input") == 0) {
        engine_set_fail(e, "LAYOUT_CONFLICT");
        pthread_mutex_unlock(&e->mu);
        fprintf(stderr, "dvs_engine: layout rejected in input role\n");
        return -1;
    }

    if (e->st == NULL) {
        e->st = file_playout_mtl_init_dual(&e->cfg, 1, 1,
                                           e->cfg.mtl_log_level, NULL);
        if (!e->st) {
            pthread_mutex_unlock(&e->mu);
            fprintf(stderr, "dvs_engine: MTL init failed\n");
            return -1;
        }
        fprintf(stderr, "dvs_engine: MTL %s up\n", mtl_version());
    }

    if (count <= 0) {
        /* backdrop-only commit: drop the sessions and swap to a 0-lane
         * compositor so the still (or black) keeps scanning out. MTL stays
         * up for fast future commits. */
        dvs_tx_backend_stop();
        dvs_tx_stop(&e->tx);
        file_playout_pip_stream old[MP4_PIP_MAX_LANES];
        int old_n = e->cfg.pip_stream_count;
        if (old_n > MP4_PIP_MAX_LANES) old_n = MP4_PIP_MAX_LANES;
        if (old_n > 0)
            memcpy(old, e->cfg.pip_streams, (size_t)old_n * sizeof(*old));
        rx_compositor* shadow = build_compositor(e, NULL, 0);
        if (!shadow) {
            engine_set_fail(e, "MEDIA_SESSION_FAILED");
            pthread_mutex_unlock(&e->mu);
            fprintf(stderr, "dvs_engine: empty compositor create failed\n");
            return -1;
        }
        if (rx_compositor_start(shadow) != 0) {
            rx_compositor_free(shadow);
            engine_set_fail(e, "MEDIA_SESSION_FAILED");
            pthread_mutex_unlock(&e->mu);
            fprintf(stderr, "dvs_engine: empty compositor start failed\n");
            return -1;
        }
        if (wait_ns != 0) {
            pthread_mutex_unlock(&e->mu);
            engine_wait_until(e, wait_ns);
            pthread_mutex_lock(&e->mu);
        }
        rx_compositor* prev = e->comp;
        e->comp = shadow;
        e->cfg.pip_stream_count = 0;
        for (int i = 0; i < old_n; i++)
            dvs_session_release(old[i].sip, old[i].udp_port);
        engine_refresh_counts_locked(e);
        engine_record_commit_locked(e, epoch_ns, fps, frame_index);
        engine_push_display_locked(e);
        pthread_mutex_unlock(&e->mu);
        if (prev) {
            rx_compositor_stop(prev);
            rx_compositor_free(prev);
        }
        /* clear mutes: no lane carries audio anymore */
        dvs_audio_set(e, "", 0);
        fprintf(stderr, "dvs_engine: layout cleared to backdrop\n");
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

    if (e->comp) {
        int fr = rx_compositor_try_fast_apply(e->comp, streams, count);
        if (fr == 0) {
            if (wait_ns != 0) {
                pthread_mutex_unlock(&e->mu);
                engine_wait_until(e, wait_ns);
                pthread_mutex_lock(&e->mu);
                if (!e->comp) {
                    engine_set_fail(e, "MEDIA_SESSION_FAILED");
                    pthread_mutex_unlock(&e->mu);
                    return -1;
                }
            }
            memcpy(e->cfg.pip_streams, streams,
                   (size_t)count * sizeof(*streams));
            e->cfg.pip_stream_count = count;
            engine_refresh_counts_locked(e);
            engine_record_commit_locked(e, epoch_ns, fps, frame_index);
            engine_push_display_locked(e);
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
            engine_set_fail(e, "LAYOUT_CONFLICT");
            pthread_mutex_unlock(&e->mu);
            fprintf(stderr, "dvs_engine: layout validation failed\n");
            return -1;
        }
        /* source change: fall through to the shadow exchange below. The live
         * compositor keeps displaying until the shadow is ready. */
    }

    /* Shadow exchange: build and start the new compositor before touching the
     * live one, then swap. A failed shadow never disturbs the picture. */
    file_playout_pip_stream old[MP4_PIP_MAX_LANES];
    int old_n = e->cfg.pip_stream_count;
    if (old_n > MP4_PIP_MAX_LANES) old_n = MP4_PIP_MAX_LANES;
    memcpy(old, e->cfg.pip_streams, (size_t)old_n * sizeof(*old));
    file_playout_pip_stream saved_new[MP4_PIP_MAX_LANES];
    memcpy(saved_new, streams, (size_t)count * sizeof(*streams));

    rx_compositor* shadow = build_compositor(e, streams, count);
    if (!shadow) {
        /* build touched cfg.pip_streams: restore the live layout view. */
        memcpy(e->cfg.pip_streams, old, (size_t)old_n * sizeof(*old));
        e->cfg.pip_stream_count = old_n;
        for (int i = 0; i < old_n; i++) {
            int lane = 0;
            dvs_session_admit(old[i].sip, old[i].udp_port, &lane);
        }
        engine_set_fail(e, "MEDIA_SESSION_FAILED");
        pthread_mutex_unlock(&e->mu);
        fprintf(stderr, "dvs_engine: shadow prepare failed, picture kept\n");
        return -1;
    }
    if (rx_compositor_start(shadow) != 0) {
        rx_compositor_free(shadow);
        memcpy(e->cfg.pip_streams, old, (size_t)old_n * sizeof(*old));
        e->cfg.pip_stream_count = old_n;
        engine_set_fail(e, "MEDIA_SESSION_FAILED");
        pthread_mutex_unlock(&e->mu);
        fprintf(stderr, "dvs_engine: shadow start failed, picture kept\n");
        return -1;
    }
    /* cfg already carries the new layout via build_compositor; snapshot it
     * back in case the build normalized anything, then wait for content. */
    memcpy(saved_new, e->cfg.pip_streams, (size_t)count * sizeof(*saved_new));
    int need_wait = (e->comp != NULL);
    pthread_mutex_unlock(&e->mu);
    int ready_rc = 0;
    if (need_wait) ready_rc = engine_shadow_wait_ready(shadow);
    pthread_mutex_lock(&e->mu);
    if (ready_rc != 0) {
        pthread_mutex_unlock(&e->mu);
        rx_compositor_stop(shadow);
        rx_compositor_free(shadow);
        pthread_mutex_lock(&e->mu);
        memcpy(e->cfg.pip_streams, old, (size_t)old_n * sizeof(*old));
        e->cfg.pip_stream_count = old_n;
        engine_set_fail(e, "DECODE_NOT_READY");
        pthread_mutex_unlock(&e->mu);
        fprintf(stderr, "dvs_engine: shadow no first frame, picture kept\n");
        return -1;
    }
    if (wait_ns != 0) {
        pthread_mutex_unlock(&e->mu);
        engine_wait_until(e, wait_ns);
        pthread_mutex_lock(&e->mu);
        if (!e->comp && old_n > 0) {
            pthread_mutex_unlock(&e->mu);
            rx_compositor_stop(shadow);
            rx_compositor_free(shadow);
            return -1;
        }
    }
    rx_compositor* prev = e->comp;
    e->comp = shadow;
    memcpy(e->cfg.pip_streams, saved_new, (size_t)count * sizeof(*saved_new));
    e->cfg.pip_stream_count = count;
    engine_release_gone(old, old_n, saved_new, count);
    engine_refresh_counts_locked(e);
    engine_record_commit_locked(e, epoch_ns, fps, frame_index);
    engine_push_display_locked(e);
    pthread_mutex_unlock(&e->mu);

    if (prev) {
        rx_compositor_stop(prev);
        rx_compositor_free(prev);
    }
    fprintf(stderr, "dvs_engine: layout swapped %d -> %d\n", old_n, count);

    /* Audio follows the commit (owner resolved against the new lanes). */
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
    /* Rebuild through the frame path so a backdrop-only compositor survives
     * a mode change instead of being torn down. */
    if (has_comp) return dvs_engine_apply_layout_frame(e, cur, cur_n, 0, 0, 60.0, -1);
    return 0;
}

const char* dvs_engine_role(const dvs_engine* e) {
    if (!e || !e->role[0]) return "";
    return e->role;
}

int dvs_engine_media_idle(dvs_engine* e) {
    if (!e) return 1;
    pthread_mutex_lock(&e->mu);
    /* An empty (backdrop-only) compositor counts as idle display: no
     * session is admitted and no TX runs. */
    int idle = (e->cfg.pip_stream_count == 0) &&
               (e->tx.state == DVS_TX_IDLE);
    pthread_mutex_unlock(&e->mu);
    return idle;
}

int dvs_engine_set_role(dvs_engine* e, const char* role) {
    if (!e || !role) return -1;
    if (strcmp(role, "input") != 0 && strcmp(role, "output") != 0) return -1;
    /* Same-role switches are no-ops so repeated calls never disturb a
     * running TX chain or tear down an already idle RX side. */
    if (strcmp(e->role, role) == 0) return 0;
    if (strcmp(role, "input") == 0) {
        pthread_mutex_lock(&e->mu);
        snprintf(e->role, sizeof(e->role), "input");
        /* RX-only teardown: keep any TX session untouched so a repeated
         * switch does not drop a running input chain. */
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
    snprintf(e->role, sizeof(e->role), "output");
    pthread_mutex_lock(&e->mu);
    dvs_tx_backend_stop();
    dvs_tx_stop(&e->tx);
    pthread_mutex_unlock(&e->mu);
    return 0;
}

int dvs_engine_tx_start(dvs_engine* e, const dvs_tx_params_t* params,
                        char* err, size_t errlen) {
    if (!e || !params) {
        if (err && errlen > 0) snprintf(err, errlen, "missing tx params");
        return -1;
    }
    /* Role and parameter gate before touching the running compositor: a
     * rejected TX start must not clear the current picture. The session
     * start below remains the authoritative gate under the engine lock. */
    if (strcmp(e->role, "input") != 0) {
        if (err && errlen > 0) snprintf(err, errlen, "tx needs input role");
        return -1;
    }
    if (dvs_tx_validate(params, err, errlen) != 0) return -1;
    pthread_mutex_lock(&e->mu);
    /* Role mutual exclusion: an input box never runs the RX compositor. */
    if (e->comp) {
        rx_compositor_stop(e->comp);
        rx_compositor_free(e->comp);
        e->comp = NULL;
    }
    {
        file_playout_pip_stream old[MP4_PIP_MAX_LANES];
        int old_n = e->cfg.pip_stream_count;
        if (old_n > MP4_PIP_MAX_LANES) old_n = MP4_PIP_MAX_LANES;
        memcpy(old, e->cfg.pip_streams, (size_t)old_n * sizeof(*old));
        e->cfg.pip_stream_count = 0;
        for (int i = 0; i < old_n; i++)
            dvs_session_release(old[i].sip, old[i].udp_port);
        atomic_store(&e->rx_sessions, 0);
        atomic_store(&e->decoding, 0);
        atomic_store(&e->layers, 0);
    }
    if (e->st == NULL) {
        e->st = file_playout_mtl_init_dual(&e->cfg, 1, 1,
                                           e->cfg.mtl_log_level, NULL);
        if (!e->st) {
            pthread_mutex_unlock(&e->mu);
            if (err && errlen > 0) snprintf(err, errlen, "MTL init failed");
            return -1;
        }
    }
    engine_push_tx_ctx(e);
    int rc = dvs_tx_start(&e->tx, params, e->role, err, errlen);
    pthread_mutex_unlock(&e->mu);
    return rc;
}

void dvs_engine_tx_stop(dvs_engine* e) {
    if (!e) return;
    pthread_mutex_lock(&e->mu);
    dvs_tx_backend_stop();
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
    dvs_tx_backend_stop();
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

struct json_object* dvs_engine_commit_report(dvs_engine* e) {
    struct json_object* o = json_object_new_object();
    if (!o) return NULL;
    int64_t frame = 0;
    uint64_t at = 0;
    int late = 0;
    if (e) {
        pthread_mutex_lock(&e->mu);
        frame = e->commit_frame;
        at = e->commit_applied_at;
        late = e->commit_late;
        pthread_mutex_unlock(&e->mu);
    }
    json_object_object_add(o, "frameIndex", json_object_new_int64(frame));
    json_object_object_add(o, "appliedAtTaiNs", json_object_new_int64((int64_t)at));
    json_object_object_add(o, "late", json_object_new_int(late));
    return o;
}

const char* dvs_engine_last_fail(dvs_engine* e) {
    if (!e || !e->last_fail[0]) return "";
    return e->last_fail;
}

void dvs_engine_set_pending_base(dvs_engine* e,
                                 const dvs_base_image_desc_t* base) {
    if (!e || !base) return;
    pthread_mutex_lock(&e->mu);
    e->pending_base = *base;
    pthread_mutex_unlock(&e->mu);
}

void dvs_engine_set_pending_adjust(dvs_engine* e,
                                   const dvs_image_adjust_t* adjust) {
    if (!e || !adjust) return;
    pthread_mutex_lock(&e->mu);
    e->pending_adjust = *adjust;
    pthread_mutex_unlock(&e->mu);
}

struct json_object* dvs_engine_display_status(dvs_engine* e) {
    struct json_object* o = json_object_new_object();
    if (!o) return NULL;
    struct json_object* bi = json_object_new_object();
    struct json_object* ia = json_object_new_object();
    if (e) {
        pthread_mutex_lock(&e->mu);
        json_object_object_add(bi, "revision", json_object_new_int(e->base_revision));
        json_object_object_add(bi, "state",
            json_object_new_string(e->base_state[0] ? e->base_state : "none"));
        if (e->base_error[0])
            json_object_object_add(bi, "error",
                json_object_new_string(e->base_error));
        json_object_object_add(ia, "brightness",
            json_object_new_int(e->applied_adjust.brightness));
        json_object_object_add(ia, "contrast",
            json_object_new_int(e->applied_adjust.contrast));
        json_object_object_add(ia, "saturation",
            json_object_new_int(e->applied_adjust.saturation));
        json_object_object_add(ia, "hue", json_object_new_int(e->applied_adjust.hue));
        json_object_object_add(ia, "supported",
            json_object_new_boolean(e->adjust_supported));
        pthread_mutex_unlock(&e->mu);
    }
    json_object_object_add(o, "baseImage", bi);
    json_object_object_add(o, "imageAdjust", ia);
    return o;
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

struct json_object* dvs_engine_audio_status(dvs_engine* e) {
    struct json_object* o = json_object_new_object();
    if (!o) return NULL;
    if (!e) {
        json_object_object_add(o, "owner", json_object_new_string(""));
        json_object_object_add(o, "volume", json_object_new_int(0));
        json_object_object_add(o, "muted", json_object_new_boolean(1));
        json_object_object_add(o, "applied", json_object_new_boolean(0));
        return o;
    }
    pthread_mutex_lock(&e->mu);
    char owner[64], sip[32];
    uint16_t port;
    int vol, ok;
    snprintf(owner, sizeof(owner), "%s", e->audio_applied_owner);
    snprintf(sip, sizeof(sip), "%s", e->audio_applied_sip);
    port = e->audio_applied_port;
    vol = e->audio_applied_volume;
    ok = e->audio_applied_ok;
    int sub = e->comp ? rx_compositor_audio_running(e->comp) : 0;
    pthread_mutex_unlock(&e->mu);
    json_object_object_add(o, "owner", json_object_new_string(owner));
    json_object_object_add(o, "sip", json_object_new_string(sip));
    json_object_object_add(o, "audioPort", json_object_new_int(port));
    json_object_object_add(o, "volume", json_object_new_int(vol));
    json_object_object_add(o, "muted", json_object_new_boolean(port == 0));
    json_object_object_add(o, "subscribed", json_object_new_boolean(sub));
    json_object_object_add(o, "applied", json_object_new_boolean(ok));
    return o;
}

void dvs_engine_counts(dvs_engine* e, int* rx_sessions, int* decoding,
                       int* layers) {
    if (!e) {
        if (rx_sessions) *rx_sessions = 0;
        if (decoding) *decoding = 0;
        if (layers) *layers = 0;
        return;
    }
    pthread_mutex_lock(&e->mu);
    int rx = 0, dec = 0, lay = 0;
    if (e->comp) {
        rx = rx_compositor_lane_count(e->comp);
        dec = rx_compositor_ready_count(e->comp);
        lay = rx_compositor_layer_count(e->comp);
    } else {
        rx = dvs_session_count();
        dec = 0;
        lay = 0;
    }
    atomic_store(&e->rx_sessions, rx);
    atomic_store(&e->decoding, dec);
    atomic_store(&e->layers, lay);
    pthread_mutex_unlock(&e->mu);
    if (rx_sessions) *rx_sessions = rx;
    if (decoding) *decoding = dec;
    if (layers) *layers = lay;
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
            int ready = rx_compositor_lane_ready(comp, i);
            json_object_object_add(o, "ready", json_object_new_boolean(ready));
            json_object_object_add(o, "decoding", json_object_new_boolean(ready));
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
        json_object_object_add(o, "decoding", json_object_new_boolean(0));
        json_object_object_add(o, "lostFrames", json_object_new_int64(0));
        json_object_object_add(o, "lastPtsNs", json_object_new_int64(0));
        json_object_array_add(arr, o);
    }
    return arr;
}
