/* SPDX-License-Identifier: BSD-3-Clause
 *
 * MP4 playlist — multi-file, loop, seek; separate video/audio demux instances.
 */

#ifndef _MP4_PLAYLIST_H_
#define _MP4_PLAYLIST_H_

#include "../mp4_dist_app.h"
#include "mp4_demux.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct mp4_playlist mp4_playlist;

int mp4_playlist_create(const mp4_dist_cfg* cfg, mp4_playlist** out);
void mp4_playlist_free(mp4_playlist* pl);

mp4_demux_ctx* mp4_playlist_video_demux(mp4_playlist* pl);
mp4_demux_ctx* mp4_playlist_audio_demux(mp4_playlist* pl);

const mp4_stream_info* mp4_playlist_video_info(mp4_playlist* pl);
const mp4_stream_info* mp4_playlist_audio_info(mp4_playlist* pl);

/** Advance video to next file (or loop). Returns 0 on success, -ENOENT at EOF if !loop. */
int mp4_playlist_advance_video(mp4_playlist* pl);

/** Advance audio to next file (or loop). Returns 0 on success, -ENOENT at EOF if !loop. */
int mp4_playlist_advance_audio(mp4_playlist* pl);

/** Current file path, or NULL. */
const char* mp4_playlist_current_path(const mp4_playlist* pl);

#endif /* _MP4_PLAYLIST_H_ */
