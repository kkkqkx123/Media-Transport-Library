#include "live_tx_config.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* string storage for loaded config */
struct live_ingest_cfg_strings {
  char tx_port[128];
  char tx_lip[32];
  char tx_dip[32];
  char v4l2_device[64];
  char drm_device[64];
  char log_level[16];
  char pacing[16];
  char audio_device[64];
};

static struct live_ingest_cfg_strings g_str;

static void dup_str(const char** dst, char* buf, size_t bufsz, const char* src) {
  if (!src) return;
  strncpy(buf, src, bufsz - 1);
  buf[bufsz - 1] = '\0';
  *dst = buf;
}

struct live_ingest_cfg live_ingest_cfg_defaults(void) {
  struct live_ingest_cfg c = {
      .test_time = 30,
      .width = 1920,
      .height = 1080,
      .fps = 60.0f,
      .udp_port = 15000,
      .audio_udp_port = 15002,
      .num_slices = 1,
      .bitrate_bps = 40000000,
      .gop_size = 60,
      .tx_port = "native_af_xdp:enP3p49s0",
      .tx_lip = "192.168.80.213",
      .tx_dip = "192.168.80.212",
      .v4l2_device = "/dev/video0",
      .drm_device = "/dev/dri/card0",
      .cpu_capture = 1,
      .cpu_encode = 2,
      .log_level = "info",
      .pacing = "tsc_narrow",
      .file_source = false,
      .mp4_count = 0,
      .loop = true,
      .seek_ms = 0,
      .codestream_size = 512 * 1024,
      .use_tai = false,
      .audio_source = AUDIO_SOURCE_NONE,
      .audio_sample_rate = 48000,
      .audio_channels = 2,
      .audio_period_frames = 1024,
  };
  return c;
}

static int json_get_int(struct json_object* o, const char* key, int def) {
  struct json_object* v = NULL;
  if (!o || !json_object_object_get_ex(o, key, &v)) return def;
  return json_object_get_int(v);
}

static double json_get_double(struct json_object* o, const char* key, double def) {
  struct json_object* v = NULL;
  if (!o || !json_object_object_get_ex(o, key, &v)) return def;
  return json_object_get_double(v);
}

static const char* json_get_string(struct json_object* o, const char* key) {
  struct json_object* v = NULL;
  if (!o || !json_object_object_get_ex(o, key, &v)) return NULL;
  return json_object_get_string(v);
}

int live_ingest_config_load(const char* path, struct live_ingest_cfg* cfg) {
  if (!path || !cfg) return -1;
  *cfg = live_ingest_cfg_defaults();

  struct json_object* root = json_object_from_file(path);
  if (!root) {
    fprintf(stderr, "live_ingest_config: failed to parse %s\n", path);
    return -1;
  }

  cfg->test_time = json_get_int(root, "test_time_s", cfg->test_time);

  struct json_object* iface = NULL;
  if (json_object_object_get_ex(root, "interfaces", &iface) &&
      json_object_is_type(iface, json_type_array) &&
      json_object_array_length(iface) > 0) {
    struct json_object* i0 = json_object_array_get_idx(iface, 0);
    const char* name = json_get_string(i0, "name");
    const char* ip = json_get_string(i0, "ip");
    if (name) dup_str(&cfg->tx_port, g_str.tx_port, sizeof(g_str.tx_port), name);
    if (ip) dup_str(&cfg->tx_lip, g_str.tx_lip, sizeof(g_str.tx_lip), ip);
  }

  struct json_object* streams = NULL;
  if (json_object_object_get_ex(root, "streams", &streams) &&
      json_object_is_type(streams, json_type_array) &&
      json_object_array_length(streams) > 0) {
    struct json_object* s0 = json_object_array_get_idx(streams, 0);

    struct json_object* cap = NULL;
    if (json_object_object_get_ex(s0, "capture", &cap)) {
      cfg->width = json_get_int(cap, "width", cfg->width);
      cfg->height = json_get_int(cap, "height", cfg->height);
      cfg->fps = (float)json_get_double(cap, "fps", cfg->fps);
      const char* dev = json_get_string(cap, "device");
      if (dev)
        dup_str(&cfg->v4l2_device, g_str.v4l2_device, sizeof(g_str.v4l2_device), dev);
    }

    struct json_object* codec = NULL;
    if (json_object_object_get_ex(s0, "codec", &codec)) {
      const char* name = json_get_string(codec, "name");
      if (!name) name = json_get_string(codec, "codec");
      if (name && (!strcmp(name, "h265") || !strcmp(name, "hevc") ||
                   !strcmp(name, "H265") || !strcmp(name, "HEVC")))
        cfg->codec = 1;
      cfg->bitrate_bps = json_get_int(codec, "bitrate_bps", cfg->bitrate_bps);
      cfg->gop_size = json_get_int(codec, "gop_size", cfg->gop_size);
      cfg->num_slices = json_get_int(codec, "num_slices", cfg->num_slices);
      cfg->intra_refresh_mode = json_get_int(codec, "intra_refresh_mode", 0);
      cfg->intra_refresh_arg = json_get_int(codec, "intra_refresh_arg", 0);
    }

    struct json_object* xport = NULL;
    if (json_object_object_get_ex(s0, "transport", &xport)) {
      cfg->udp_port = (uint16_t)json_get_int(xport, "udp_port", cfg->udp_port);
      const char* dip = json_get_string(xport, "dip_addr");
      if (dip) dup_str(&cfg->tx_dip, g_str.tx_dip, sizeof(g_str.tx_dip), dip);
    }
  }

  struct json_object* aff = NULL;
  if (json_object_object_get_ex(root, "affinity", &aff)) {
    cfg->cpu_capture = json_get_int(aff, "capture", cfg->cpu_capture);
    cfg->cpu_encode = json_get_int(aff, "encoder", cfg->cpu_encode);
  }

  /* file frame-source plugin (phase2): source=file selects MP4 input */
  const char* src = json_get_string(root, "source");
  if (src && !strcmp(src, "file")) cfg->file_source = true;
  const char* mp4 = json_get_string(root, "mp4_file");
  if (mp4 && mp4[0] && cfg->mp4_count < LIVE_INGEST_MAX_FILES) {
    snprintf(cfg->mp4_files[cfg->mp4_count++], LIVE_INGEST_MAX_PATH, "%s", mp4);
    cfg->file_source = true;
  }
  struct json_object* plist = NULL;
  if (json_object_object_get_ex(root, "mp4_files", &plist) &&
      json_object_is_type(plist, json_type_array)) {
    int n = (int)json_object_array_length(plist);
    for (int i = 0; i < n && cfg->mp4_count < LIVE_INGEST_MAX_FILES; i++) {
      struct json_object* e = json_object_array_get_idx(plist, i);
      if (e) {
        snprintf(cfg->mp4_files[cfg->mp4_count++], LIVE_INGEST_MAX_PATH, "%s",
                 json_object_get_string(e));
        cfg->file_source = true;
      }
    }
  }
  struct json_object* lv = NULL;
  if (json_object_object_get_ex(root, "loop", &lv))
    cfg->loop = json_object_get_boolean(lv);
  struct json_object* sk = NULL;
  if (json_object_object_get_ex(root, "seek_ms", &sk))
    cfg->seek_ms = json_object_get_int64(sk);
  struct json_object* au = NULL;
  if (json_object_object_get_ex(root, "audio_udp_port", &au))
    cfg->audio_udp_port = (uint16_t)json_object_get_int(au);
  struct json_object* cs = NULL;
  if (json_object_object_get_ex(root, "codestream_size", &cs))
    cfg->codestream_size = (size_t)json_object_get_int64(cs);
  struct json_object* tai = NULL;
  if (json_object_object_get_ex(root, "use_tai", &tai))
    cfg->use_tai = json_object_get_boolean(tai);

  /* real-time audio capture: streams[0].audio { source, device, ... } */
  if (streams && json_object_is_type(streams, json_type_array) &&
      json_object_array_length(streams) > 0) {
    struct json_object* s0 = json_object_array_get_idx(streams, 0);
    struct json_object* aud = NULL;
    if (json_object_object_get_ex(s0, "audio", &aud)) {
      const char* asrc = json_get_string(aud, "source");
      if (asrc && !strcmp(asrc, "alsa"))
        cfg->audio_source = AUDIO_SOURCE_ALSA;
      else if (asrc && !strcmp(asrc, "none"))
        cfg->audio_source = AUDIO_SOURCE_NONE;
      const char* adev = json_get_string(aud, "device");
      if (adev)
        dup_str(&cfg->audio_device, g_str.audio_device, sizeof(g_str.audio_device), adev);
      cfg->audio_sample_rate = json_get_int(aud, "sample_rate", cfg->audio_sample_rate);
      cfg->audio_channels = json_get_int(aud, "channels", cfg->audio_channels);
      cfg->audio_period_frames =
          json_get_int(aud, "period_frames", cfg->audio_period_frames);
    }
  }

  const char* ll = json_get_string(root, "log_level");
  if (ll) dup_str(&cfg->log_level, g_str.log_level, sizeof(g_str.log_level), ll);

  const char* pacing = json_get_string(root, "st21_pacing");
  if (pacing) dup_str(&cfg->pacing, g_str.pacing, sizeof(g_str.pacing), pacing);

  json_object_put(root);
  printf("live_ingest_config: loaded %s\n", path);
  return 0;
}
