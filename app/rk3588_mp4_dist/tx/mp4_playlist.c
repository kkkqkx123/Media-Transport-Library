/* SPDX-License-Identifier: BSD-3-Clause
 *
 * MP4 playlist — separate video/audio demux per file, loop and seek support.
 */

#include "mp4_playlist.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mp4_playlist {
    char   paths[MP4_DIST_MAX_PLAYLIST][MP4_DIST_MAX_PATH_LEN];
    int    count;
    int    video_index;
    int    audio_index;
    bool   loop;
    int64_t seek_ms;
    bool   seek_pending;

    mp4_demux_ctx* video_demux;
    mp4_demux_ctx* audio_demux;
    pthread_mutex_t lock;
};

static int playlist_open_video(mp4_playlist* pl, int idx) {
    if (idx < 0 || idx >= pl->count) return -EINVAL;

    if (pl->video_demux) {
        mp4_demux_close(pl->video_demux);
        pl->video_demux = NULL;
    }

    int ret = mp4_demux_open(pl->paths[idx], &pl->video_demux);
    if (ret < 0) return ret;

    if (pl->seek_pending && pl->seek_ms > 0)
        mp4_demux_seek_ms(pl->video_demux, pl->seek_ms);

    pl->video_index = idx;
    printf("playlist: video [%d/%d] %s\n", idx + 1, pl->count, pl->paths[idx]);
    return 0;
}

static int playlist_open_audio(mp4_playlist* pl, int idx) {
    if (idx < 0 || idx >= pl->count) return -EINVAL;

    if (pl->audio_demux) {
        mp4_demux_close(pl->audio_demux);
        pl->audio_demux = NULL;
    }

    int ret = mp4_demux_open(pl->paths[idx], &pl->audio_demux);
    if (ret < 0) return ret;

    if (pl->seek_pending && pl->seek_ms > 0)
        mp4_demux_seek_ms(pl->audio_demux, pl->seek_ms);

    pl->audio_index = idx;
    printf("playlist: audio [%d/%d] %s\n", idx + 1, pl->count, pl->paths[idx]);
    return 0;
}

int mp4_playlist_create(const mp4_dist_cfg* cfg, mp4_playlist** out) {
    *out = NULL;
    if (!cfg || cfg->mp4_count <= 0) return -EINVAL;

    mp4_playlist* pl = calloc(1, sizeof(*pl));
    if (!pl) return -ENOMEM;

    pl->count = cfg->mp4_count;
    pl->loop  = cfg->loop;
    pl->seek_ms = cfg->seek_ms;
    pl->seek_pending = (cfg->seek_ms > 0);
    pthread_mutex_init(&pl->lock, NULL);

    for (int i = 0; i < cfg->mp4_count; i++)
        snprintf(pl->paths[i], sizeof(pl->paths[i]), "%s", cfg->mp4_files[i]);

    int ret = playlist_open_video(pl, 0);
    if (ret < 0) goto fail;

    ret = playlist_open_audio(pl, 0);
    if (ret < 0) goto fail;

    pl->seek_pending = false;
    *out = pl;
    return 0;

fail:
    mp4_playlist_free(pl);
    return ret;
}

void mp4_playlist_free(mp4_playlist* pl) {
    if (!pl) return;
    if (pl->video_demux) mp4_demux_close(pl->video_demux);
    if (pl->audio_demux) mp4_demux_close(pl->audio_demux);
    pthread_mutex_destroy(&pl->lock);
    free(pl);
}

mp4_demux_ctx* mp4_playlist_video_demux(mp4_playlist* pl) {
    return pl ? pl->video_demux : NULL;
}

mp4_demux_ctx* mp4_playlist_audio_demux(mp4_playlist* pl) {
    return pl ? pl->audio_demux : NULL;
}

const mp4_stream_info* mp4_playlist_video_info(mp4_playlist* pl) {
    return pl && pl->video_demux ? mp4_demux_get_video_stream(pl->video_demux) : NULL;
}

const mp4_stream_info* mp4_playlist_audio_info(mp4_playlist* pl) {
    return pl && pl->audio_demux ? mp4_demux_get_audio_stream(pl->audio_demux) : NULL;
}

static int playlist_advance_locked(mp4_playlist* pl, int* index, int (*open_fn)(mp4_playlist*, int)) {
    int next = *index + 1;
    if (next >= pl->count) {
        if (!pl->loop) return -ENOENT;
        next = 0;
    }
    int ret = open_fn(pl, next);
    if (ret < 0) return ret;
    *index = next;
    return 0;
}

int mp4_playlist_advance_video(mp4_playlist* pl) {
    if (!pl) return -EINVAL;
    pthread_mutex_lock(&pl->lock);
    int ret = playlist_advance_locked(pl, &pl->video_index, playlist_open_video);
    pthread_mutex_unlock(&pl->lock);
    return ret;
}

int mp4_playlist_advance_audio(mp4_playlist* pl) {
    if (!pl) return -EINVAL;
    pthread_mutex_lock(&pl->lock);
    int ret = playlist_advance_locked(pl, &pl->audio_index, playlist_open_audio);
    pthread_mutex_unlock(&pl->lock);
    return ret;
}

const char* mp4_playlist_current_path(const mp4_playlist* pl) {
    if (!pl || pl->video_index < 0 || pl->video_index >= pl->count) return NULL;
    return pl->paths[pl->video_index];
}
