#define _GNU_SOURCE
/* SPDX-License-Identifier: BSD-3-Clause
 *
 * ST30 RX session — notify_frame_ready enqueues to SPSC; ALSA thread drains.
 */

#include "mtl_rx_audio.h"
#include "alsa_audio.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include <st_api.h>

#define MTL_RX_AUDIO_Q_SIZE 32

struct mtl_rx_audio_slot {
    void*    fb;
    uint64_t tai_ns;
    size_t   size;
};

struct mtl_rx_audio {
    mtl_rx_audio_params     cfg;
    st30_rx_handle          handle;
    size_t                  frame_bytes;

    struct mtl_rx_audio_slot slots[MTL_RX_AUDIO_Q_SIZE];
    atomic_uint             q_head;
    atomic_uint             q_tail;

    pthread_t                 thd;
    atomic_bool               running;
    uint64_t                  stat_frames;
    uint64_t                  stat_drops;
};

static uint64_t audio_frame_tai_ns(struct st30_rx_frame_meta* meta) {
    if (!meta) return 0;
    if (meta->timestamp_first_pkt)
        return meta->timestamp_first_pkt;
    if (meta->timestamp)
        return st10_get_tai(meta->tfmt, meta->timestamp,
                            st30_get_sample_rate(meta->sampling));
    return 0;
}

static int rx_notify_frame_ready(void* priv, void* frame, struct st30_rx_frame_meta* meta) {
    mtl_rx_audio* rx = priv;

    uint32_t head = atomic_load_explicit(&rx->q_head, memory_order_relaxed);
    uint32_t next = (head + 1) % MTL_RX_AUDIO_Q_SIZE;
    uint32_t tail = atomic_load_explicit(&rx->q_tail, memory_order_acquire);
    if (next == tail) {
        rx->stat_drops++;
        return -ENOSPC;
    }

    rx->slots[head].fb     = frame;
    rx->slots[head].tai_ns = audio_frame_tai_ns(meta);
    rx->slots[head].size   = meta ? meta->frame_recv_size : rx->frame_bytes;
    atomic_store_explicit(&rx->q_head, next, memory_order_release);
    return 0;
}

static void* audio_play_thread(void* arg) {
    mtl_rx_audio* rx = arg;

    while (atomic_load(&rx->running)) {
        uint32_t tail = atomic_load_explicit(&rx->q_tail, memory_order_relaxed);
        uint32_t head = atomic_load_explicit(&rx->q_head, memory_order_acquire);
        if (tail == head) {
            usleep(500);
            continue;
        }

        struct mtl_rx_audio_slot* slot = &rx->slots[tail];
        void* fb = slot->fb;
        size_t bytes = slot->size ? slot->size : rx->frame_bytes;

        if (rx->cfg.alsa && fb && bytes >= 4) {
            snd_pcm_uframes_t nframes = (snd_pcm_uframes_t)(bytes / 4);
            if (alsa_audio_write(rx->cfg.alsa, (const int16_t*)fb, nframes) == 0) {
                if (rx->cfg.sync && slot->tai_ns)
                    av_sync_note_audio_frame(rx->cfg.sync, slot->tai_ns);
            }
        }

        st30_rx_put_framebuff(rx->handle, fb);
        rx->stat_frames++;

        uint32_t next = (tail + 1) % MTL_RX_AUDIO_Q_SIZE;
        atomic_store_explicit(&rx->q_tail, next, memory_order_release);
    }

    /* drain pending slots on shutdown */
    for (;;) {
        uint32_t tail = atomic_load_explicit(&rx->q_tail, memory_order_relaxed);
        uint32_t head = atomic_load_explicit(&rx->q_head, memory_order_acquire);
        if (tail == head) break;
        st30_rx_put_framebuff(rx->handle, rx->slots[tail].fb);
        atomic_store_explicit(&rx->q_tail, (tail + 1) % MTL_RX_AUDIO_Q_SIZE,
                              memory_order_release);
    }
    return NULL;
}

mtl_rx_audio* mtl_rx_audio_create(const mtl_rx_audio_params* p) {
    if (!p || !p->st) return NULL;

    mtl_rx_audio* rx = calloc(1, sizeof(*rx));
    if (!rx) return NULL;
    rx->cfg = *p;
    atomic_init(&rx->q_head, 0);
    atomic_init(&rx->q_tail, 0);
    atomic_init(&rx->running, false);

    enum st30_fmt fmt = ST30_FMT_PCM16;
    enum st30_sampling sampling = ST30_SAMPLING_48K;
    enum st30_ptime ptime = ST30_PTIME_1MS;
    uint16_t channel = 2;

    rx->frame_bytes = st30_calculate_framebuff_size(
        fmt, ptime, sampling, channel, MP4_ST30_FRAME_TIME_NS, NULL);

    struct st30_rx_ops ops = {0};
    ops.name           = "mp4_dist_audio_rx";
    ops.priv           = rx;
    ops.type           = ST30_TYPE_FRAME_LEVEL;
    ops.fmt            = fmt;
    ops.channel        = channel;
    ops.sampling       = sampling;
    ops.ptime          = ptime;
    ops.num_port       = 1;
    ops.framebuff_cnt  = 16;
    ops.framebuff_size = (uint32_t)rx->frame_bytes;
    ops.notify_frame_ready = rx_notify_frame_ready;
    ops.payload_type   = p->payload_type ? p->payload_type : 98;
    ops.udp_port[0]    = p->audio_udp_port;
    if (p->rx_port)
        strncpy(ops.port[0], p->rx_port, MTL_PORT_MAX_LEN - 1);
    if (p->rx_sip) {
        unsigned int a, b, c, d;
        if (sscanf(p->rx_sip, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            ops.ip_addr[0][0] = (uint8_t)a;
            ops.ip_addr[0][1] = (uint8_t)b;
            ops.ip_addr[0][2] = (uint8_t)c;
            ops.ip_addr[0][3] = (uint8_t)d;
        }
    }

    rx->handle = st30_rx_create(p->st, &ops);
    if (!rx->handle) {
        fprintf(stderr, "mtl_rx_audio: st30_rx_create failed\n");
        free(rx);
        return NULL;
    }

    printf("mtl_rx_audio: st30_rx ready port=%u fb=%zu B\n",
           p->audio_udp_port, rx->frame_bytes);
    return rx;
}

int mtl_rx_audio_start(mtl_rx_audio* rx) {
    if (!rx) return -EINVAL;
    atomic_store(&rx->running, true);
    if (pthread_create(&rx->thd, NULL, audio_play_thread, rx) != 0)
        return -EIO;
    if (rx->cfg.cpu >= 0) {
        cpu_set_t cset;
        CPU_ZERO(&cset);
        CPU_SET(rx->cfg.cpu, &cset);
        pthread_setaffinity_np(rx->thd, sizeof(cset), &cset);
    }
    return 0;
}

void mtl_rx_audio_stop(mtl_rx_audio* rx) {
    if (!rx) return;
    if (!atomic_exchange(&rx->running, false)) return;
    if (rx->thd) {
        pthread_join(rx->thd, NULL);
        rx->thd = 0;
    }
    printf("mtl_rx_audio: %llu frames, %llu drops\n",
           (unsigned long long)rx->stat_frames,
           (unsigned long long)rx->stat_drops);
}

void mtl_rx_audio_free(mtl_rx_audio* rx) {
    if (!rx) return;
    mtl_rx_audio_stop(rx);
    if (rx->handle) st30_rx_free(rx->handle);
    free(rx);
}
