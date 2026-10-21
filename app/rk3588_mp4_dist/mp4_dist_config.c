/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Configuration: defaults, JSON loading, CLI parsing.
 */

#include "mp4_dist_config.h"
#include "shared/drm_mode.h"
#include "shared/display_layout.h"
#include "shared/ptp_time.h"
#include "tx/mp4_demux.h"
#include <errno.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- defaults ---- */
void mp4_dist_cfg_defaults(mp4_dist_cfg* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode             = MP4_DIST_MODE_BOTH;
    cfg->test_time        = 30;
    strcpy(cfg->tx_port,   "native_af_xdp:enP3p49s0");
    strcpy(cfg->rx_port,   "native_af_xdp:eth0");
    strcpy(cfg->tx_lip,    "192.168.80.213");
    strcpy(cfg->rx_lip,    "192.168.80.212");
    strcpy(cfg->tx_dip,    "192.168.80.212");
    strcpy(cfg->rx_sip,    "192.168.80.213");
    cfg->udp_port          = 20000;
    cfg->audio_udp_port    = 20002;
    cfg->width             = 0;
    cfg->height            = 0;
    cfg->fps               = 0.0f;
    cfg->codestream_size   = 1024 * 1024;   /* 1 MB */
    strcpy(cfg->mp4_file,  "");
    cfg->mp4_count         = 0;
    cfg->loop              = true;
    cfg->seek_ms           = 0;
    strcpy(cfg->probe_mp4, "");
    strcpy(cfg->drm_device, "/dev/dri/card0");
    strcpy(cfg->drm_connector, "HDMI-A-1");
    strcpy(cfg->drm_mode,  "auto");
    cfg->drm_buf_count     = 5;
    cfg->playout_delay_ms  = 80;
    strcpy(cfg->alsa_device, "default");
    /*
     * RK3588: CPU 0-3 = A55, 4-7 = A76. MTL prepends main_lcore 0 → -l 0,<workers>.
     * Do NOT put 0 in worker strings.
     *
     * both-mode + MtlManager: lcore IDs are global (DPDK remapped index, not
     * physical CPU). RX mask 0,4,6,7 uses manager slots 1,2; TX mask 0,1,2 also
     * needs slots 1,2 → collision. Use the SAME full worker list for RX and TX;
     * RX (starts first) takes 1,2 for dual sch; TX takes the next free slot (3).
     */
    cfg->cpu_bridge        = 1;
    cfg->cpu_decode        = 4;
    cfg->cpu_render        = 5;
    cfg->cpu_audio         = 3;
    /* Empty = MTL/DPDK auto-select workers. Set per platform in JSON/CLI. */
    cfg->mtl_lcores_shared[0] = '\0';
    cfg->mtl_lcores_rx[0]     = '\0';
    cfg->mtl_lcores_tx[0]     = '\0';
    cfg->mtl_log_level     = MTL_LOG_LEVEL_NOTICE;
    cfg->ptp_mode          = MP4_DIST_PTP_EXTERNAL;
    cfg->ptp_utc_offset    = 0;
    cfg->stats_report_interval_s = 10;
    cfg->e2e_report_interval_s   = 0;
    mp4_dist_viewport_defaults(&cfg->display_viewport);
}

/* ---- JSON loader ---- */
int mp4_dist_config_load(const char* json_path, mp4_dist_cfg* cfg) {
    struct json_object* root = json_object_from_file(json_path);
    if (!root) {
        fprintf(stderr, "config: cannot open %s\n", json_path);
        return -1;
    }

    struct json_object* val;

    if (json_object_object_get_ex(root, "tx_port", &val))
        snprintf(cfg->tx_port, sizeof(cfg->tx_port), "%s",
                 json_object_get_string(val));
    if (json_object_object_get_ex(root, "rx_port", &val))
        snprintf(cfg->rx_port, sizeof(cfg->rx_port), "%s",
                 json_object_get_string(val));
    if (json_object_object_get_ex(root, "tx_lip", &val))
        snprintf(cfg->tx_lip, sizeof(cfg->tx_lip), "%s",
                 json_object_get_string(val));
    if (json_object_object_get_ex(root, "rx_lip", &val))
        snprintf(cfg->rx_lip, sizeof(cfg->rx_lip), "%s",
                 json_object_get_string(val));
    if (json_object_object_get_ex(root, "tx_dip", &val))
        snprintf(cfg->tx_dip, sizeof(cfg->tx_dip), "%s",
                 json_object_get_string(val));
    if (json_object_object_get_ex(root, "rx_sip", &val))
        snprintf(cfg->rx_sip, sizeof(cfg->rx_sip), "%s",
                 json_object_get_string(val));
    if (json_object_object_get_ex(root, "udp_port", &val))
        cfg->udp_port = (uint16_t)json_object_get_int(val);
    if (json_object_object_get_ex(root, "audio_udp_port", &val))
        cfg->audio_udp_port = (uint16_t)json_object_get_int(val);
    if (json_object_object_get_ex(root, "width", &val))
        cfg->width = json_object_get_int(val);
    if (json_object_object_get_ex(root, "height", &val))
        cfg->height = json_object_get_int(val);
    if (json_object_object_get_ex(root, "fps", &val))
        cfg->fps = (float)json_object_get_double(val);
    if (json_object_object_get_ex(root, "codestream_size", &val))
        cfg->codestream_size = (size_t)json_object_get_int64(val);
    if (json_object_object_get_ex(root, "mp4_file", &val)) {
        snprintf(cfg->mp4_file, sizeof(cfg->mp4_file), "%s",
                 json_object_get_string(val));
        if (cfg->mp4_file[0]) {
            snprintf(cfg->mp4_files[0], sizeof(cfg->mp4_files[0]), "%s",
                     cfg->mp4_file);
            cfg->mp4_count = 1;
        }
    }
    if (json_object_object_get_ex(root, "loop", &val))
        cfg->loop = json_object_get_boolean(val);
    if (json_object_object_get_ex(root, "seek_ms", &val))
        cfg->seek_ms = (int64_t)json_object_get_int64(val);
    if (json_object_object_get_ex(root, "drm_device", &val))
        snprintf(cfg->drm_device, sizeof(cfg->drm_device), "%s",
                 json_object_get_string(val));
    if (json_object_object_get_ex(root, "drm_connector", &val))
        snprintf(cfg->drm_connector, sizeof(cfg->drm_connector), "%s",
                 json_object_get_string(val));
    if (json_object_object_get_ex(root, "drm_mode", &val))
        snprintf(cfg->drm_mode, sizeof(cfg->drm_mode), "%s",
                 json_object_get_string(val));
    if (json_object_object_get_ex(root, "drm_buf_count", &val))
        cfg->drm_buf_count = json_object_get_int(val);
    if (json_object_object_get_ex(root, "playout_delay_ms", &val))
        cfg->playout_delay_ms = json_object_get_int(val);
    if (json_object_object_get_ex(root, "test_time", &val))
        cfg->test_time = json_object_get_int(val);
    if (json_object_object_get_ex(root, "cpu_bridge", &val))
        cfg->cpu_bridge = json_object_get_int(val);
    if (json_object_object_get_ex(root, "cpu_decode", &val))
        cfg->cpu_decode = json_object_get_int(val);
    if (json_object_object_get_ex(root, "cpu_render", &val))
        cfg->cpu_render = json_object_get_int(val);
    if (json_object_object_get_ex(root, "cpu_audio", &val))
        cfg->cpu_audio = json_object_get_int(val);
    if (json_object_object_get_ex(root, "mtl_lcores_rx", &val))
        snprintf(cfg->mtl_lcores_rx, sizeof(cfg->mtl_lcores_rx), "%s",
                 json_object_get_string(val));
    if (json_object_object_get_ex(root, "mtl_lcores_tx", &val))
        snprintf(cfg->mtl_lcores_tx, sizeof(cfg->mtl_lcores_tx), "%s",
                 json_object_get_string(val));
    if (json_object_object_get_ex(root, "mtl_lcores", &val)) {
        const char* s = json_object_get_string(val);
        snprintf(cfg->mtl_lcores_shared, sizeof(cfg->mtl_lcores_shared), "%s", s);
        snprintf(cfg->mtl_lcores_rx, sizeof(cfg->mtl_lcores_rx), "%s", s);
        snprintf(cfg->mtl_lcores_tx, sizeof(cfg->mtl_lcores_tx), "%s", s);
    }
    if (json_object_object_get_ex(root, "mtl_log_level", &val)) {
        const char* level = json_object_get_string(val);
        if (!strcmp(level, "debug"))
            cfg->mtl_log_level = MTL_LOG_LEVEL_DEBUG;
        else if (!strcmp(level, "info"))
            cfg->mtl_log_level = MTL_LOG_LEVEL_INFO;
        else if (!strcmp(level, "notice"))
            cfg->mtl_log_level = MTL_LOG_LEVEL_NOTICE;
        else if (!strcmp(level, "warning"))
            cfg->mtl_log_level = MTL_LOG_LEVEL_WARNING;
        else if (!strcmp(level, "error"))
            cfg->mtl_log_level = MTL_LOG_LEVEL_ERR;
    }
    if (json_object_object_get_ex(root, "ptp_mode", &val)) {
        const char* mode = json_object_get_string(val);
        if (!strcmp(mode, "builtin"))
            cfg->ptp_mode = MP4_DIST_PTP_BUILTIN;
        else if (!strcmp(mode, "monotonic"))
            cfg->ptp_mode = MP4_DIST_PTP_MONOTONIC;
        else
            cfg->ptp_mode = MP4_DIST_PTP_EXTERNAL;
    }
    if (json_object_object_get_ex(root, "ptp_utc_offset", &val))
        cfg->ptp_utc_offset = json_object_get_int(val);
    if (json_object_object_get_ex(root, "stats_report_interval_s", &val))
        cfg->stats_report_interval_s = json_object_get_int(val);
    if (json_object_object_get_ex(root, "e2e_report_interval_s", &val))
        cfg->e2e_report_interval_s = json_object_get_int(val);

    struct json_object* vp_obj = NULL;
    if (json_object_object_get_ex(root, "display_viewport", &vp_obj) && vp_obj) {
        if (json_object_object_get_ex(vp_obj, "enabled", &val))
            cfg->display_viewport.enabled = json_object_get_boolean(val);
        if (json_object_object_get_ex(vp_obj, "x", &val))
            cfg->display_viewport.x = json_object_get_int(val);
        if (json_object_object_get_ex(vp_obj, "y", &val))
            cfg->display_viewport.y = json_object_get_int(val);
        if (json_object_object_get_ex(vp_obj, "width", &val))
            cfg->display_viewport.width = json_object_get_int(val);
        if (json_object_object_get_ex(vp_obj, "height", &val))
            cfg->display_viewport.height = json_object_get_int(val);
    }

    struct json_object* pip_arr = NULL;
    if (json_object_object_get_ex(root, "pip_streams", &pip_arr) && pip_arr &&
        json_object_is_type(pip_arr, json_type_array)) {
        int n = (int)json_object_array_length(pip_arr);
        if (n > MP4_PIP_MAX_LANES)
            n = MP4_PIP_MAX_LANES;
        cfg->pip_stream_count = 0;
        for (int i = 0; i < n; i++) {
            struct json_object* elem = json_object_array_get_idx(pip_arr, i);
            if (!elem)
                continue;
            mp4_dist_pip_stream* s = &cfg->pip_streams[cfg->pip_stream_count];
            memset(s, 0, sizeof(*s));
            s->lane = -1;
            if (json_object_object_get_ex(elem, "lane", &val))
                s->lane = json_object_get_int(val);
            if (json_object_object_get_ex(elem, "main", &val))
                s->main = json_object_get_boolean(val);
            if (json_object_object_get_ex(elem, "sip", &val))
                snprintf(s->sip, sizeof(s->sip), "%s", json_object_get_string(val));
            if (json_object_object_get_ex(elem, "udp_port", &val))
                s->udp_port = (uint16_t)json_object_get_int(val);
            if (json_object_object_get_ex(elem, "audio_udp_port", &val))
                s->audio_udp_port = (uint16_t)json_object_get_int(val);
            if (json_object_object_get_ex(elem, "width", &val))
                s->width = json_object_get_int(val);
            if (json_object_object_get_ex(elem, "height", &val))
                s->height = json_object_get_int(val);
            if (json_object_object_get_ex(elem, "fps", &val))
                s->fps = (float)json_object_get_double(val);
            if (json_object_object_get_ex(elem, "x", &val))
                s->x = json_object_get_int(val);
            if (json_object_object_get_ex(elem, "y", &val))
                s->y = json_object_get_int(val);
            if (json_object_object_get_ex(elem, "w", &val))
                s->w = json_object_get_int(val);
            if (json_object_object_get_ex(elem, "h", &val))
                s->h = json_object_get_int(val);
            if (json_object_object_get_ex(elem, "codestream_size", &val))
                s->codestream_size = (size_t)json_object_get_int64(val);
            cfg->pip_stream_count++;
        }
    }

    struct json_object* tx_arr = NULL;
    if (json_object_object_get_ex(root, "tx_streams", &tx_arr) && tx_arr &&
        json_object_is_type(tx_arr, json_type_array)) {
        int n = (int)json_object_array_length(tx_arr);
        if (n > MP4_PIP_MAX_LANES)
            n = MP4_PIP_MAX_LANES;
        cfg->tx_stream_count = 0;
        for (int i = 0; i < n; i++) {
            struct json_object* elem = json_object_array_get_idx(tx_arr, i);
            if (!elem)
                continue;
            mp4_dist_tx_stream* s = &cfg->tx_streams[cfg->tx_stream_count];
            memset(s, 0, sizeof(*s));
            if (json_object_object_get_ex(elem, "main", &val))
                s->main = json_object_get_boolean(val);
            if (json_object_object_get_ex(elem, "mp4_file", &val))
                snprintf(s->mp4_file, sizeof(s->mp4_file), "%s",
                         json_object_get_string(val));
            if (json_object_object_get_ex(elem, "udp_port", &val))
                s->udp_port = (uint16_t)json_object_get_int(val);
            if (json_object_object_get_ex(elem, "audio_udp_port", &val))
                s->audio_udp_port = (uint16_t)json_object_get_int(val);
            if (json_object_object_get_ex(elem, "width", &val))
                s->width = json_object_get_int(val);
            if (json_object_object_get_ex(elem, "height", &val))
                s->height = json_object_get_int(val);
            if (json_object_object_get_ex(elem, "fps", &val))
                s->fps = (float)json_object_get_double(val);
            if (json_object_object_get_ex(elem, "codestream_size", &val))
                s->codestream_size = (size_t)json_object_get_int64(val);
            if (json_object_object_get_ex(elem, "loop", &val))
                s->loop = json_object_get_boolean(val);
            if (json_object_object_get_ex(elem, "seek_ms", &val))
                s->seek_ms = (int64_t)json_object_get_int64(val);
            cfg->tx_stream_count++;
        }
    }

    json_object_put(root);
    return 0;
}

/* ---- CLI parser ---- */
void mp4_dist_tx_usage(const char* prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s [sec] [--config FILE] [--port P] [--lip IP] [--dip IP]"
        " [--udp PORT] [--audio-udp PORT] [--mp4 FILE]... [--no-loop]"
        " [--seek-ms MS] [--width W] [--height H] [--fps FPS]\n"
        "\n"
        "  --mtl-log-level: debug|info|notice|warning|error (default: notice)\n"
        "  --ptp-mode: external|builtin|monotonic (default: external)\n"
        "  --ptp-utc-offset: seconds subtracted from TAI (default: 0)\n",
        prog);
}

void mp4_dist_rx_usage(const char* prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s [sec] [--config FILE] [--port P] [--lip IP] [--sip IP]"
        " [--udp PORT] [--audio-udp PORT] [--width W] [--height H] [--fps FPS]"
        " [--probe-mp4 FILE] [--mtl-log-level LEVEL]\n"
        "\n"
        "  --fps must match TX (critical for ST22 slot assembly).\n"
        "  --mtl-log-level: debug|info|notice|warning|error (default: notice)\n"
        "  --ptp-mode: external|builtin|monotonic (default: external)\n"
        "  --ptp-utc-offset: seconds subtracted from TAI (default: 0)\n",
        prog);
}

static void mp4_dist_add_mp4(mp4_dist_cfg* cfg, const char* path) {
    if (!path || !path[0]) return;
    if (cfg->mp4_count < MP4_DIST_MAX_PLAYLIST) {
        snprintf(cfg->mp4_files[cfg->mp4_count], MP4_DIST_MAX_PATH_LEN, "%s", path);
        cfg->mp4_count++;
    }
    if (!cfg->mp4_file[0])
        snprintf(cfg->mp4_file, sizeof(cfg->mp4_file), "%s", path);
}

int mp4_dist_apply_mp4_probe(mp4_dist_cfg* cfg) {
    const char* path = NULL;
    if (cfg->mp4_count > 0)
        path = cfg->mp4_files[0];
    else if (cfg->probe_mp4[0])
        path = cfg->probe_mp4;
    else if (cfg->mp4_file[0])
        path = cfg->mp4_file;

    if (!path || !path[0]) return -EINVAL;

    int w = 0, h = 0;
    float fps = 0.0f;
    int ret = mp4_probe_file(path, &w, &h, &fps);
    if (ret < 0) {
        fprintf(stderr, "probe: cannot probe %s: %d\n", path, ret);
        return ret;
    }

    if (!cfg->width_override && cfg->width <= 0)  cfg->width  = w;
    if (!cfg->height_override && cfg->height <= 0) cfg->height = h;
    if (!cfg->fps_override && cfg->fps <= 0.0f)   cfg->fps    = fps;

    printf("probe: %s → %dx%d @ %.3ffps\n", path, w, h, (double)fps);
    return 0;
}

void mp4_dist_print_rx_hint(const mp4_dist_cfg* cfg, const char* prog) {
    printf("\nSuggested RX command (match TX fps):\n");
    printf("  %s %d --config config/rx_dual_host.json --fps %.3f\n",
           prog, cfg->test_time, (double)cfg->fps);
}

int mp4_dist_warn_probe_mismatch(const mp4_dist_cfg* cfg) {
    const char* path = NULL;
    if (cfg->mp4_count > 0)
        path = cfg->mp4_files[0];
    else if (cfg->probe_mp4[0])
        path = cfg->probe_mp4;
    else if (cfg->mp4_file[0])
        path = cfg->mp4_file;

    if (!path || !path[0])
        return 0;

    int pw = 0, ph = 0;
    float pfps = 0.0f;
    if (mp4_probe_file(path, &pw, &ph, &pfps) < 0)
        return 0;

    int warns = 0;
    if (cfg->width > 0 && cfg->width != pw) {
        fprintf(stderr, "WARN: config width=%d != probe %d (%s)\n",
                cfg->width, pw, path);
        warns++;
    }
    if (cfg->height > 0 && cfg->height != ph) {
        fprintf(stderr, "WARN: config height=%d != probe %d (%s)\n",
                cfg->height, ph, path);
        warns++;
    }
    if (cfg->fps > 0.0f && pfps > 0.0f) {
        float diff = cfg->fps - pfps;
        if (diff < 0.0f) diff = -diff;
        if (diff > 0.05f) {
            fprintf(stderr, "WARN: config fps=%.3f != probe %.3f (%s)\n",
                    (double)cfg->fps, (double)pfps, path);
            warns++;
        }
    }
    return warns;
}

void mp4_dist_apply_drm_mode(mp4_dist_cfg* cfg) {
    if (!cfg || !mp4_drm_mode_is_auto(cfg->drm_mode))
        return;

    char built[MP4_DIST_MAX_NAME_LEN];
    if (mp4_drm_build_mode_string(cfg->width, cfg->height, cfg->fps,
                                  built, sizeof(built)) != 0)
        return;

    snprintf(cfg->drm_mode, sizeof(cfg->drm_mode), "%s", built);
    printf("drm_mode: auto → %s (from %dx%d @ %.3ffps, refresh %d Hz)\n",
           built, cfg->width, cfg->height, (double)cfg->fps,
           mp4_drm_suggest_refresh(cfg->fps));
}

int mp4_dist_mode_dims(const mp4_dist_cfg* cfg, int* mode_w, int* mode_h) {
    if (!cfg || !mode_w || !mode_h)
        return -1;

    int w = 0, h = 0, v = 0;
    if (cfg->drm_mode[0] &&
        sscanf(cfg->drm_mode, "%dx%d@%d", &w, &h, &v) >= 2 && w > 0 && h > 0) {
        *mode_w = w;
        *mode_h = h;
        return 0;
    }

    *mode_w = cfg->width > 0 ? cfg->width : 1920;
    *mode_h = cfg->height > 0 ? cfg->height : 1080;
    return 0;
}

int mp4_dist_tx_streams_probe(mp4_dist_cfg* cfg) {
    if (!cfg || cfg->tx_stream_count <= 0)
        return -EINVAL;

    for (int i = 0; i < cfg->tx_stream_count; i++) {
        mp4_dist_tx_stream* s = &cfg->tx_streams[i];
        if (!s->mp4_file[0])
            return -EINVAL;

        if (s->width > 0 && s->height > 0 && s->fps > 0.0f)
            continue;

        int w = 0, h = 0;
        float fps = 0.0f;
        int ret = mp4_probe_file(s->mp4_file, &w, &h, &fps);
        if (ret < 0) {
            fprintf(stderr, "probe: cannot probe tx_streams[%d] %s: %d\n",
                    i, s->mp4_file, ret);
            return ret;
        }
        if (s->width <= 0)
            s->width = w;
        if (s->height <= 0)
            s->height = h;
        if (s->fps <= 0.0f)
            s->fps = fps;

        printf("probe: tx_streams[%d] %s → %dx%d @ %.3ffps\n",
               i, s->mp4_file, s->width, s->height, (double)s->fps);
    }

    for (int i = 0; i < cfg->tx_stream_count; i++) {
        mp4_dist_tx_stream* s = &cfg->tx_streams[i];
        if (!s->codestream_size && cfg->codestream_size)
            s->codestream_size = cfg->codestream_size;
        if (!s->audio_udp_port && s->main && cfg->audio_udp_port)
            s->audio_udp_port = cfg->audio_udp_port;
    }
    return 0;
}

static int mp4_dist_parse_mtl_log_level(const char* level, enum mtl_log_level* out) {
    if (!strcmp(level, "debug"))
        *out = MTL_LOG_LEVEL_DEBUG;
    else if (!strcmp(level, "info"))
        *out = MTL_LOG_LEVEL_INFO;
    else if (!strcmp(level, "notice"))
        *out = MTL_LOG_LEVEL_NOTICE;
    else if (!strcmp(level, "warning"))
        *out = MTL_LOG_LEVEL_WARNING;
    else if (!strcmp(level, "error"))
        *out = MTL_LOG_LEVEL_ERR;
    else
        return -1;
    return 0;
}

static int mp4_dist_parse_ptp_mode(const char* mode, mp4_dist_ptp_mode* out) {
    if (!mode || !mode[0] || !strcmp(mode, "external")) {
        *out = MP4_DIST_PTP_EXTERNAL;
        return 0;
    }
    if (!strcmp(mode, "builtin")) {
        *out = MP4_DIST_PTP_BUILTIN;
        return 0;
    }
    if (!strcmp(mode, "monotonic")) {
        *out = MP4_DIST_PTP_MONOTONIC;
        return 0;
    }
    return -1;
}

static const char* mp4_dist_ptp_mode_str(mp4_dist_ptp_mode mode) {
    switch (mode) {
    case MP4_DIST_PTP_BUILTIN:   return "builtin";
    case MP4_DIST_PTP_MONOTONIC: return "monotonic";
    default:                     return "external";
    }
}

static int mp4_dist_config_parse_args(mp4_dist_cfg* cfg, int argc, char** argv,
                                      int start_i) {
    int i = start_i;

    while (i < argc) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            if (mp4_dist_config_load(argv[++i], cfg) != 0) return -1;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            if (cfg->mode == MP4_DIST_MODE_RX)
                snprintf(cfg->rx_port, sizeof(cfg->rx_port), "%s", argv[++i]);
            else
                snprintf(cfg->tx_port, sizeof(cfg->tx_port), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--lip") == 0 && i + 1 < argc) {
            if (cfg->mode == MP4_DIST_MODE_RX)
                snprintf(cfg->rx_lip, sizeof(cfg->rx_lip), "%s", argv[++i]);
            else
                snprintf(cfg->tx_lip, sizeof(cfg->tx_lip), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--dip") == 0 && i + 1 < argc) {
            snprintf(cfg->tx_dip, sizeof(cfg->tx_dip), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--sip") == 0 && i + 1 < argc) {
            snprintf(cfg->rx_sip, sizeof(cfg->rx_sip), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--udp") == 0 && i + 1 < argc) {
            cfg->udp_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--audio-udp") == 0 && i + 1 < argc) {
            cfg->audio_udp_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--mp4") == 0 && i + 1 < argc) {
            mp4_dist_add_mp4(cfg, argv[++i]);
        } else if (strcmp(argv[i], "--no-loop") == 0) {
            cfg->loop = false;
        } else if (strcmp(argv[i], "--seek-ms") == 0 && i + 1 < argc) {
            cfg->seek_ms = (int64_t)atoll(argv[++i]);
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            cfg->width = atoi(argv[++i]);
            cfg->width_override = true;
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            cfg->height = atoi(argv[++i]);
            cfg->height_override = true;
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            cfg->fps = (float)atof(argv[++i]);
            cfg->fps_override = true;
        } else if (strcmp(argv[i], "--probe-mp4") == 0 && i + 1 < argc) {
            snprintf(cfg->probe_mp4, sizeof(cfg->probe_mp4), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--mtl-log-level") == 0 && i + 1 < argc) {
            if (mp4_dist_parse_mtl_log_level(argv[++i], &cfg->mtl_log_level) != 0) {
                fprintf(stderr, "Invalid --mtl-log-level: %s\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "--ptp-mode") == 0 && i + 1 < argc) {
            if (mp4_dist_parse_ptp_mode(argv[++i], &cfg->ptp_mode) != 0) {
                fprintf(stderr, "Invalid --ptp-mode: %s\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "--ptp-utc-offset") == 0 && i + 1 < argc) {
            cfg->ptp_utc_offset = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--stats-interval") == 0 && i + 1 < argc) {
            cfg->stats_report_interval_s = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--e2e-interval") == 0 && i + 1 < argc) {
            cfg->e2e_report_interval_s = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--cpu-bridge") == 0 && i + 1 < argc) {
            cfg->cpu_bridge = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--cpu-decode") == 0 && i + 1 < argc) {
            cfg->cpu_decode = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--cpu-render") == 0 && i + 1 < argc) {
            cfg->cpu_render = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--cpu-audio") == 0 && i + 1 < argc) {
            cfg->cpu_audio = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--mtl-lcores-rx") == 0 && i + 1 < argc) {
            snprintf(cfg->mtl_lcores_rx, sizeof(cfg->mtl_lcores_rx), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--mtl-lcores-tx") == 0 && i + 1 < argc) {
            snprintf(cfg->mtl_lcores_tx, sizeof(cfg->mtl_lcores_tx), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--mtl-lcores") == 0 && i + 1 < argc) {
            const char* s = argv[++i];
            snprintf(cfg->mtl_lcores_shared, sizeof(cfg->mtl_lcores_shared), "%s", s);
            snprintf(cfg->mtl_lcores_rx, sizeof(cfg->mtl_lcores_rx), "%s", s);
            snprintf(cfg->mtl_lcores_tx, sizeof(cfg->mtl_lcores_tx), "%s", s);
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            return -1;
        }
        i++;
    }

    if (cfg->mp4_count == 0 && cfg->mp4_file[0])
        mp4_dist_add_mp4(cfg, cfg->mp4_file);

    return 0;
}

static int mp4_dist_config_parse_cli_common(int argc, char** argv,
                                            mp4_dist_cfg* cfg,
                                            enum mp4_dist_mode mode,
                                            void (*usage)(const char*)) {
    if (argc < 1) {
        usage(argv[0]);
        return -1;
    }

    cfg->mode = mode;

    int i = 1;
    if (i < argc && argv[i][0] != '-') {
        cfg->test_time = atoi(argv[i]);
        i++;
    }

    return mp4_dist_config_parse_args(cfg, argc, argv, i);
}

int mp4_dist_config_parse_tx_cli(int argc, char** argv, mp4_dist_cfg* cfg) {
    if (argc < 1) {
        mp4_dist_tx_usage("mp4_dist_tx");
        return -1;
    }
    return mp4_dist_config_parse_cli_common(argc, argv, cfg, MP4_DIST_MODE_TX,
                                            mp4_dist_tx_usage);
}

int mp4_dist_config_parse_rx_cli(int argc, char** argv, mp4_dist_cfg* cfg) {
    if (argc < 1) {
        mp4_dist_rx_usage("mp4_dist_rx");
        return -1;
    }
    return mp4_dist_config_parse_cli_common(argc, argv, cfg, MP4_DIST_MODE_RX,
                                            mp4_dist_rx_usage);
}

void mp4_dist_config_print(const mp4_dist_cfg* cfg) {
    const char* mode_names[] = {"tx", "rx", "both"};
    printf("=== MP4 Distribution Config ===\n");
    printf("  mode:            %s\n", mode_names[cfg->mode]);
    printf("  test_time:       %d s\n", cfg->test_time);
    printf("  TX: port=%s lip=%s dip=%s\n", cfg->tx_port, cfg->tx_lip, cfg->tx_dip);
    printf("  RX: port=%s lip=%s sip=%s\n", cfg->rx_port, cfg->rx_lip, cfg->rx_sip);
    printf("  video: %dx%d @ %.3ffps, codestream=%zu\n",
           cfg->width, cfg->height, (double)cfg->fps, cfg->codestream_size);
    printf("  UDP: video=%u audio=%u\n", cfg->udp_port, cfg->audio_udp_port);
    if (cfg->mp4_count > 0) {
        printf("  MP4 playlist (%d file(s), loop=%s, seek=%lld ms):\n",
               cfg->mp4_count, cfg->loop ? "on" : "off",
               (long long)cfg->seek_ms);
        for (int j = 0; j < cfg->mp4_count; j++)
            printf("    [%d] %s\n", j + 1, cfg->mp4_files[j]);
    } else {
        printf("  MP4:  %s\n", cfg->mp4_file[0] ? cfg->mp4_file : "(none)");
    }
    if (cfg->probe_mp4[0])
        printf("  probe_mp4: %s\n", cfg->probe_mp4);
    printf("  DRM:  %s/%s mode=%s bufs=%d\n",
           cfg->drm_device, cfg->drm_connector, cfg->drm_mode, cfg->drm_buf_count);
    if (cfg->display_viewport.enabled) {
        printf("  display_viewport: %dx%d@(%d,%d)\n",
               cfg->display_viewport.width, cfg->display_viewport.height,
               cfg->display_viewport.x, cfg->display_viewport.y);
    } else {
        printf("  display_viewport: fullscreen (default)\n");
    }
    if (cfg->pip_stream_count > 0) {
        printf("  pip_streams: %d lane(s)\n", cfg->pip_stream_count);
        for (int i = 0; i < cfg->pip_stream_count; i++) {
            const mp4_dist_pip_stream* s = &cfg->pip_streams[i];
            printf("    [%d] %s:%u %dx%d@%.3ffps rect(%d,%d %dx%d) main=%s audio=%u\n",
                   i, s->sip, s->udp_port, s->width, s->height, (double)s->fps,
                   s->x, s->y, s->w, s->h, s->main ? "yes" : "no",
                   s->audio_udp_port);
        }
    }
    if (cfg->tx_stream_count > 0) {
        printf("  tx_streams: %d lane(s)\n", cfg->tx_stream_count);
        for (int i = 0; i < cfg->tx_stream_count; i++) {
            const mp4_dist_tx_stream* s = &cfg->tx_streams[i];
            printf("    [%d] %s udp=%u %dx%d@%.3ffps main=%s audio=%u loop=%s\n",
                   i, s->mp4_file, s->udp_port, s->width, s->height,
                   (double)s->fps, s->main ? "yes" : "no", s->audio_udp_port,
                   s->loop ? "on" : "off");
        }
    }
    printf("  playout_delay:   %d ms\n", cfg->playout_delay_ms);
    printf("  ALSA: %s\n", cfg->alsa_device);
    printf("  CPU pthread: bridge=%d decode=%d render=%d audio=%d\n",
           cfg->cpu_bridge, cfg->cpu_decode, cfg->cpu_render, cfg->cpu_audio);
    printf("  MTL lcores:  rx=%s tx=%s",
           cfg->mtl_lcores_rx[0] ? cfg->mtl_lcores_rx : "(auto)",
           cfg->mtl_lcores_tx[0] ? cfg->mtl_lcores_tx : "(auto)");
    if (cfg->mode == MP4_DIST_MODE_BOTH &&
        strcmp(cfg->mtl_lcores_rx, cfg->mtl_lcores_tx) != 0)
        printf("  [WARN: both-mode should use same mtl_lcores for MtlManager]");
    printf("\n");
    printf("  mtl_log_level:   %d\n", cfg->mtl_log_level);
    printf("  ptp_mode:        %s (utc_offset=%d)\n",
           mp4_dist_ptp_mode_str(cfg->ptp_mode), cfg->ptp_utc_offset);
    printf("  stats_report:    %d s (0=exit only)\n", cfg->stats_report_interval_s);
    if (cfg->e2e_report_interval_s > 0)
        printf("  e2e_report:      %d s\n", cfg->e2e_report_interval_s);
}

/* ---- MTL init helper ---- */
static mp4_ptp_time_ctx g_ptp_ctx;
static enum mtl_pmd_type mp4_dist_pmd_from_port(const char* port_name) {
    if (!port_name || !port_name[0])
        return MTL_PMD_NATIVE_AF_XDP;
    if (strncmp(port_name, "kernel:", 7) == 0)
        return MTL_PMD_KERNEL_SOCKET;
    if (strncmp(port_name, "native_af_xdp:", 14) == 0)
        return MTL_PMD_NATIVE_AF_XDP;
    if (strncmp(port_name, "dpdk_af_xdp:", 12) == 0)
        return MTL_PMD_DPDK_AF_XDP;
    if (strncmp(port_name, "dpdk_af_packet:", 15) == 0)
        return MTL_PMD_DPDK_AF_PACKET;
    /* bare "ethN" or similar — DPDK poll-mode */
    return MTL_PMD_DPDK_USER;
}

static void mp4_dist_warn_lcores(const char* tag, const char* lcores) {
    if (!lcores || !lcores[0]) return;
    if (!strcmp(lcores, "0") ||
        (lcores[0] == '0' && (lcores[1] == '\0' || lcores[1] == ','))) {
        fprintf(stderr,
                "config: %s mtl_lcores=%s includes CPU0; MTL also uses 0 as "
                "main_lcore → EAL -l 0,0,... and sch_get_lcore fails. "
                "Use worker-only list (e.g. tx 1,2 / rx 4,6,7).\n",
                tag, lcores);
    }
}

/* Drop worker IDs >= online CPU count (e.g. RK list 1-7 on a 4-core PC). */
static const char* mp4_dist_sanitize_lcores(const char* lcores) {
    static char out[64];

    if (!lcores || !lcores[0])
        return lcores;

    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu <= 1)
        return lcores;

    out[0] = '\0';
    const char* p = lcores;
    int n = 0;
    bool clipped = false;

    while (*p) {
        char* end = NULL;
        long id = strtol(p, &end, 10);
        if (end == p)
            break;
        if (id > 0 && id < ncpu) {
            if (n > 0)
                strncat(out, ",", sizeof(out) - strlen(out) - 1);
            char buf[8];
            snprintf(buf, sizeof(buf), "%ld", id);
            strncat(out, buf, sizeof(out) - strlen(out) - 1);
            n++;
        } else if (id >= ncpu) {
            clipped = true;
        }
        p = end;
        while (*p == ',') p++;
    }

    if (clipped) {
        fprintf(stderr,
                "config: mtl_lcores trimmed \"%s\" → \"%s\" "
                "(online CPUs 0-%ld, worker IDs must be 1-%ld)\n",
                lcores, n > 0 ? out : "(auto)", ncpu - 1, ncpu - 1);
    }

    if (n == 0)
        return NULL;

    return out;
}

mtl_handle mp4_dist_mtl_init(const mp4_dist_cfg* cfg,
                             const char* port_name, const char* lip,
                             uint16_t tx_q, uint16_t rx_q,
                             enum mtl_log_level log_level,
                             const char* lcores) {
    mp4_dist_ptp_mode ptp_mode = cfg ? cfg->ptp_mode : MP4_DIST_PTP_EXTERNAL;
    int utc_offset = cfg ? cfg->ptp_utc_offset : 0;

    struct mtl_init_params mtp;
    memset(&mtp, 0, sizeof(mtp));

    mtp.num_ports = 1;
    strncpy(mtp.port[MTL_PORT_P], port_name, MTL_PORT_MAX_LEN - 1);
    mtp.pmd[MTL_PORT_P] = mp4_dist_pmd_from_port(port_name);
    mtp.net_proto[MTL_PORT_P] = MTL_PROTO_STATIC;
    strncpy((char*)mtp.sip_addr[MTL_PORT_P], lip, MTL_IP_ADDR_LEN - 1);
    mtp.log_level = log_level;

    mtp.tx_queues_cnt[MTL_PORT_P] = tx_q ? tx_q : 1;
    mtp.rx_queues_cnt[MTL_PORT_P] = rx_q ? rx_q : 1;

    mtp.flags |= MTL_FLAG_BIND_NUMA;
    switch (ptp_mode) {
    case MP4_DIST_PTP_BUILTIN:
        mtp.flags |= MTL_FLAG_PTP_ENABLE;
        printf("MTL: PTP mode=built-in (requires NIC HW timesync)\n");
        break;
    case MP4_DIST_PTP_MONOTONIC:
        printf("MTL: PTP mode=monotonic (relative anchor only)\n");
        break;
    default:
        mp4_ptp_time_ctx_init(&g_ptp_ctx, utc_offset);
        mtp.ptp_get_time_fn = mp4_ptp_mtl_time_fn;
        mtp.priv            = &g_ptp_ctx;
        printf("MTL: PTP mode=external (linuxptp → CLOCK_TAI via ptp_get_time_fn)\n");
        break;
    }
    if (tx_q > 0)
        mtp.flags |= MTL_FLAG_TX_VIDEO_MIGRATE;
    if (rx_q > 0) {
        mtp.flags |= MTL_FLAG_RX_VIDEO_MIGRATE;
        mtp.flags |= MTL_FLAG_RX_SEPARATE_VIDEO_LCORE;
    }

    /*
     * Single-queue AF_XDP: ST22 (video) + ST30 (audio) on the same port must
     * use shared RX/TX queue demux (rsq/tsq). Otherwise the second session
     * overwrites xdp queue rx_entry and the audio tasklet SIGSEGVs in
     * rx_audio_session_tasklet → mt_rxq_burst.
     */
    if (rx_q > 0 && mtp.rx_queues_cnt[MTL_PORT_P] == 1)
        mtp.flags |= MTL_FLAG_SHARED_RX_QUEUE;
    if (tx_q > 0 && mtp.tx_queues_cnt[MTL_PORT_P] == 1)
        mtp.flags |= MTL_FLAG_SHARED_TX_QUEUE;

    if (lcores && lcores[0]) {
        const char* use = mp4_dist_sanitize_lcores(lcores);
        if (use && use[0]) {
            mp4_dist_warn_lcores(port_name, use);
            mtp.lcores = (char*)use;
            printf("MTL: pinning lcores to %s (EAL -l 0,%s)\n", use, use);
        } else {
            printf("MTL: lcores auto (requested \"%s\" invalid for this host)\n",
                   lcores);
        }
    }

    mtl_handle st = mtl_init(&mtp);
    if (!st) {
        fprintf(stderr, "MTL init failed for %s\n", port_name);
        return NULL;
    }

    if (mtl_start(st) != 0) {
        fprintf(stderr, "MTL start failed for %s\n", port_name);
        mtl_uninit(st);
        return NULL;
    }

    printf("MTL: %s (lip=%s) started ptp=%s%s%s\n", port_name, lip,
           mp4_dist_ptp_mode_str(ptp_mode),
           (mtp.flags & MTL_FLAG_SHARED_RX_QUEUE) ? " shared-rx" : "",
           (mtp.flags & MTL_FLAG_SHARED_TX_QUEUE) ? " shared-tx" : "");

    if (ptp_mode == MP4_DIST_PTP_EXTERNAL)
        mp4_ptp_log_status(&g_ptp_ctx, port_name);

    return st;
}
