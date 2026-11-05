#ifndef _FILE_PLAYOUT_CONFIG_H_
#define _FILE_PLAYOUT_CONFIG_H_

#include "file_playout_app.h"

/** Load configuration from a JSON file. Returns 0 on success. */
int file_playout_config_load(const char* json_path, file_playout_cfg* cfg);

/** Parse CLI for file_playout_rx. Returns 0 on success. */
int file_playout_config_parse_rx_cli(int argc, char** argv, file_playout_cfg* cfg);

/** Print the current configuration for verification. */
void file_playout_config_print(const file_playout_cfg* cfg);

/** Probe first MP4 and fill width/height/fps when not CLI-overridden. */
int file_playout_apply_mp4_probe(file_playout_cfg* cfg);

/** Warn when config width/height/fps differs from probed MP4 (returns warn count). */
int file_playout_warn_probe_mismatch(const file_playout_cfg* cfg);

/** Fill cfg->drm_mode from width/height/fps when mode is "auto". */
void file_playout_apply_drm_mode(file_playout_cfg* cfg);

/** Parse cfg->drm_mode (or width/height fallback) into mode dimensions. */
int file_playout_mode_dims(const file_playout_cfg* cfg, int* mode_w, int* mode_h);

#endif /* _FILE_PLAYOUT_CONFIG_H_ */
