/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Configuration management: JSON file loading + CLI argument parsing.
 */

#ifndef _MP4_DIST_CONFIG_H_
#define _MP4_DIST_CONFIG_H_

#include "mp4_dist_app.h"

/** Load configuration from a JSON file. Returns 0 on success. */
int mp4_dist_config_load(const char* json_path, mp4_dist_cfg* cfg);

/** Parse CLI for mp4_dist_tx (no tx/rx subcommand). Returns 0 on success. */
int mp4_dist_config_parse_tx_cli(int argc, char** argv, mp4_dist_cfg* cfg);

/** Parse CLI for mp4_dist_rx (no tx/rx subcommand). Returns 0 on success. */
int mp4_dist_config_parse_rx_cli(int argc, char** argv, mp4_dist_cfg* cfg);

/** Print the current configuration for verification. */
void mp4_dist_config_print(const mp4_dist_cfg* cfg);

/** Print usage for mp4_dist_tx or mp4_dist_rx. */
void mp4_dist_tx_usage(const char* prog);
void mp4_dist_rx_usage(const char* prog);

/** Probe first MP4 and fill width/height/fps when not CLI-overridden. */
int mp4_dist_apply_mp4_probe(mp4_dist_cfg* cfg);

/** Print suggested RX command line for matching TX parameters. */
void mp4_dist_print_rx_hint(const mp4_dist_cfg* cfg, const char* prog);

/** Warn when config width/height/fps differs from probed MP4 (returns warn count). */
int mp4_dist_warn_probe_mismatch(const mp4_dist_cfg* cfg);

/** Fill cfg->drm_mode from width/height/fps when mode is "auto". */
void mp4_dist_apply_drm_mode(mp4_dist_cfg* cfg);

/** Parse cfg->drm_mode (or width/height fallback) into mode dimensions. */
int mp4_dist_mode_dims(const mp4_dist_cfg* cfg, int* mode_w, int* mode_h);

/** Probe width/height/fps for each tx_streams[] entry when unset. */
int mp4_dist_tx_streams_probe(mp4_dist_cfg* cfg);

#endif /* _MP4_DIST_CONFIG_H_ */
