#ifndef _LIVE_INGEST_CONFIG_H_
#define _LIVE_INGEST_CONFIG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LIVE_INGEST_MAX_FILES 16
#define LIVE_INGEST_MAX_PATH 256

enum live_ingest_audio_source {
  AUDIO_SOURCE_NONE = 0, /* video-only (default, keeps current behavior) */
  AUDIO_SOURCE_ALSA,     /* real-time ALSA PCM capture */
};

struct live_ingest_cfg {
  int test_time;
  int width, height;
  float fps;
  uint16_t udp_port;
  uint16_t audio_udp_port;
  int num_slices;
  int bitrate_bps;
  int gop_size;
  int intra_refresh_mode;
  int intra_refresh_arg;
  const char* tx_port;
  const char* tx_lip;
  const char* tx_dip;
  const char* v4l2_device;
  const char* drm_device;
  int cpu_capture, cpu_encode;
  const char* log_level;
  /* file frame-source plugin (phase2): hdmi (default) or file */
  bool file_source;
  char mp4_files[LIVE_INGEST_MAX_FILES][LIVE_INGEST_MAX_PATH];
  int mp4_count;
  bool loop;
  int64_t seek_ms;
  size_t codestream_size;
  bool use_tai;
  /* real-time audio capture (live source path) */
  enum live_ingest_audio_source audio_source;
  const char* audio_device; /* ALSA device, e.g. "hw:0,0" */
  int audio_sample_rate;    /* requested; negotiated read-back wins */
  int audio_channels;       /* 1..2, S16LE interleaved */
  int audio_period_frames;  /* frames per ALSA read, 0 = default */
};

struct live_ingest_cfg live_ingest_cfg_defaults(void);

/** Load JSON config; overlays defaults. Returns 0 on success. */
int live_ingest_config_load(const char* path, struct live_ingest_cfg* cfg);

#endif
