#define _GNU_SOURCE
/* SPDX-License-Identifier: BSD-3-Clause
 *
 * ALSA PCM capturer for live_ingest (real-time audio source).
 * Modeled after io/v4l2_capturer.c lifecycle and the ZLMediaKit
 * HdmiInAudioCapture block-pool design (see docs/plan/rk3588/
 * RK3588_LIVE_INGEST_AUDIO_CAPTURE_PLAN.md §2). S16LE interleaved PCM only,
 * matching ST30_FMT_PCM16 on the TX side.
 */

#include "alsa_capturer.h"

#ifdef HAVE_ALSA

#include <alsa/asoundlib.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ALSA_MAX_CONSECUTIVE_ERRORS 10
#define ALSA_POLL_TIMEOUT_MS 100
#define ALSA_DEFAULT_BUFFER_PERIODS 4

/* Cache-line aligned in-use flag: capture thread CAS-claims, consumer
 * releases — avoid false sharing on the flags array. */
struct alsa_block_flag {
  alignas(64) atomic_bool in_use;
};

struct alsa_capturer {
  snd_pcm_t* pcm;
  struct alsa_capturer_params cfg;

  int rate;     /* negotiated read-back */
  int channels; /* negotiated read-back */
  int period_frames;
  size_t period_bytes;
  size_t block_count;

  /* PCM block pool (capture thread claims, consumer releases) */
  uint8_t** blocks;
  struct alsa_block_flag* flags;
  size_t scan_idx;  /* capture thread only */
  uint8_t* scratch; /* drain HW when pool is exhausted */

  alsa_pcm_cb cb;
  void* cb_user;
  void (*error_cb)(void* user_data);
  void* error_user;

  atomic_bool running;
  pthread_t thread;
  bool thread_started;

  atomic_uint_fast64_t st_drops;
  atomic_uint_fast64_t st_xruns;
  atomic_uint_fast64_t st_recovers;
  atomic_uint_fast64_t st_errors;
};

static void alsa_release_block(void* user_data, void* data);

/* ------------------------------------------------------------------ open */

static int alsa_setup_hw(struct alsa_capturer* c) {
  snd_pcm_hw_params_t* hw = NULL;
  snd_pcm_sw_params_t* sw = NULL;
  snd_pcm_uframes_t period = (snd_pcm_uframes_t)c->cfg.period_frames;
  snd_pcm_uframes_t buffer;
  unsigned int rate = (unsigned int)c->cfg.sample_rate;
  int dir = 0;
  int rc;

  snd_pcm_hw_params_alloca(&hw);
  snd_pcm_sw_params_alloca(&sw);

  rc = snd_pcm_hw_params_any(c->pcm, hw);
  if (rc < 0) return rc;
  rc = snd_pcm_hw_params_set_access(c->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  if (rc < 0) return rc;
  rc = snd_pcm_hw_params_set_format(c->pcm, hw, SND_PCM_FORMAT_S16_LE);
  if (rc < 0) return rc;
  rc = snd_pcm_hw_params_set_channels(c->pcm, hw, (unsigned int)c->cfg.channels);
  if (rc < 0) return rc;
  rc = snd_pcm_hw_params_set_rate_near(c->pcm, hw, &rate, &dir);
  if (rc < 0) return rc;
  rc = snd_pcm_hw_params_set_period_size_near(c->pcm, hw, &period, &dir);
  if (rc < 0) return rc;
  buffer = period * ALSA_DEFAULT_BUFFER_PERIODS;
  rc = snd_pcm_hw_params_set_buffer_size_near(c->pcm, hw, &buffer);
  if (rc < 0) return rc;
  rc = snd_pcm_hw_params(c->pcm, hw);
  if (rc < 0) return rc;

  rc = snd_pcm_sw_params_current(c->pcm, sw);
  if (rc < 0) return rc;
  /* monotonic timestamps for snd_pcm_htimestamp (matches video clock) */
  snd_pcm_sw_params_set_tstamp_type(c->pcm, sw, SND_PCM_TSTAMP_TYPE_MONOTONIC);
  rc = snd_pcm_sw_params(c->pcm, sw);
  if (rc < 0) return rc;

  /* negotiated read-back — single source of truth */
  rc = snd_pcm_hw_params_get_rate(hw, &rate, &dir);
  if (rc < 0) return rc;
  c->rate = (int)rate;
  rc = snd_pcm_hw_params_get_channels(hw, (unsigned int*)&c->channels);
  if (rc < 0) return rc;
  rc = snd_pcm_hw_params_get_period_size(hw, &period, &dir);
  if (rc < 0) return rc;
  c->period_frames = (int)period;
  c->period_bytes = (size_t)period * (size_t)c->channels * 2;

  if (c->rate != 48000) {
    fprintf(stderr, "alsa_capturer: negotiated rate %d != 48000, not supported\n",
            c->rate);
    return -EINVAL;
  }
  return 0;
}

/* Return a block to the pool (consumer side). Exactly-once contract. */
static void alsa_release_block(void* user_data, void* data) {
  struct alsa_capturer* c = user_data;
  if (!c || !data) return;
  for (size_t i = 0; i < c->block_count; i++) {
    if (c->blocks[i] == (uint8_t*)data) {
      atomic_store_explicit(&c->flags[i].in_use, false, memory_order_release);
      return;
    }
  }
}

/* Capture thread: claim a free block by rotating scan (balanced reuse). */
static uint8_t* alsa_claim_block(struct alsa_capturer* c) {
  for (size_t n = 0; n < c->block_count; n++) {
    size_t i = (c->scan_idx + n) % c->block_count;
    bool expected = false;
    if (atomic_compare_exchange_strong_explicit(&c->flags[i].in_use, &expected, true,
                                                memory_order_acq_rel,
                                                memory_order_relaxed)) {
      c->scan_idx = (i + 1) % c->block_count;
      return c->blocks[i];
    }
  }
  return NULL;
}

/* In-place recovery to RUNNING: after overrun recover, the stream sits in
 * PREPARED and must be started explicitly to produce data again. */
static int alsa_restart_stream(struct alsa_capturer* c) {
  int rc = snd_pcm_prepare(c->pcm);
  if (rc < 0) return rc;
  return snd_pcm_start(c->pcm);
}

/* Restore the stream after an overrun/suspend; returns 0 on success. */
static int alsa_recover(struct alsa_capturer* c, int err) {
  atomic_fetch_add(&c->st_xruns, 1);
  if (err == -ESTRPIPE) {
    /* suspended: drain resume until the driver accepts */
    while ((err = snd_pcm_resume(c->pcm)) == -EAGAIN) usleep(10000);
    if (err < 0) err = snd_pcm_prepare(c->pcm);
    if (err < 0) return err;
    return snd_pcm_start(c->pcm);
  }
  if (err == -EPIPE) {
    int rc = alsa_restart_stream(c);
    if (rc < 0) return rc;
    atomic_fetch_add(&c->st_recovers, 1);
    return 0;
  }
  return err;
}

/* ------------------------------------------------------------ capture loop */

static void* alsa_capture_loop(void* arg) {
  struct alsa_capturer* c = arg;
  struct pollfd pfd;
  int nfds = snd_pcm_poll_descriptors_count(c->pcm);
  unsigned consecutive_errors = 0;

  if (nfds != 1) goto stop_err;
  snd_pcm_poll_descriptors(c->pcm, &pfd, 1);

  int rc = alsa_restart_stream(c);
  if (rc < 0) {
    fprintf(stderr, "alsa_capturer: initial start failed: %s\n", snd_strerror(rc));
    goto stop_err;
  }

  while (atomic_load_explicit(&c->running, memory_order_relaxed)) {
    pfd.revents = 0;
    int prc = poll(&pfd, 1, ALSA_POLL_TIMEOUT_MS);
    if (prc < 0) {
      if (errno == EINTR) continue;
      goto stop_err;
    }
    if (prc == 0) continue; /* timeout: re-check running flag */
    if (pfd.revents & POLLERR) {
      if (alsa_recover(c, -EPIPE) < 0) {
        if (++consecutive_errors >= ALSA_MAX_CONSECUTIVE_ERRORS) goto stop_err;
      }
      continue;
    }
    if (!(pfd.revents & POLLIN)) continue;

    /* first-sample timestamp of the period about to be read */
    snd_htimestamp_t ht = {0, 0};
    snd_pcm_uframes_t avail = 0;
    int hts_ok = (snd_pcm_htimestamp(c->pcm, &avail, &ht) == 0 && ht.tv_sec != 0);

    uint8_t* block = alsa_claim_block(c);
    uint8_t* dst = block ? block : c->scratch;
    if (!dst) {
      /* pool exhausted: drain HW into scratch to avoid overrun */
      snd_pcm_uframes_t dropped = 0;
      while (dropped < (snd_pcm_uframes_t)c->period_frames) {
        snd_pcm_sframes_t n =
            snd_pcm_readi(c->pcm, c->scratch, (snd_pcm_uframes_t)c->period_frames);
        if (n < 0) {
          if (alsa_recover(c, (int)n) < 0) goto read_fail;
          continue;
        }
        dropped += (snd_pcm_uframes_t)n;
      }
      atomic_fetch_add(&c->st_drops, 1);
      consecutive_errors = 0;
      continue;
    }

    snd_pcm_sframes_t got =
        snd_pcm_readi(c->pcm, dst, (snd_pcm_uframes_t)c->period_frames);
    if (got < 0) {
      if (alsa_recover(c, (int)got) < 0) {
      read_fail:
        if (block) alsa_release_block(c, block);
        if (++consecutive_errors >= ALSA_MAX_CONSECUTIVE_ERRORS) goto stop_err;
        continue;
      }
      if (block) alsa_release_block(c, block);
      consecutive_errors = 0;
      continue;
    }
    if ((snd_pcm_uframes_t)got < (snd_pcm_uframes_t)c->period_frames) {
      /* short read: pad the remainder with silence */
      size_t used = (size_t)got * (size_t)c->channels * 2;
      memset(dst + used, 0, c->period_bytes - used);
    }

    uint64_t pts_ns;
    if (hts_ok) {
      pts_ns = (uint64_t)ht.tv_sec * 1000000000ULL + (uint64_t)ht.tv_nsec;
    } else {
      /* fallback: read completion time minus period duration */
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      uint64_t period_ns = (uint64_t)c->period_frames * 1000000000ULL / (uint64_t)c->rate;
      pts_ns = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec - period_ns;
    }

    consecutive_errors = 0;
    if (block) {
      c->cb(c->cb_user, block, c->period_bytes, pts_ns, alsa_release_block);
    }
  }

  /* normal stop: running flag cleared by alsa_capturer_stop() */
  return NULL;

stop_err:
  atomic_fetch_add(&c->st_errors, 1);
  if (c->error_cb) c->error_cb(c->error_user);
  return NULL;
}

/* --------------------------------------------------------------- lifecycle */

struct alsa_capturer* alsa_capturer_create(const struct alsa_capturer_params* p,
                                           alsa_pcm_cb cb, void* user_data) {
  if (!p || !p->device || !cb) return NULL;
  if (p->sample_bit && p->sample_bit != 16) return NULL;
  if (p->channels < 1 || p->channels > 2) return NULL;

  struct alsa_capturer* c = calloc(1, sizeof(*c));
  if (!c) return NULL;
  c->cfg = *p;
  c->cb = cb;
  c->cb_user = user_data;
  c->rate = c->cfg.sample_rate;
  c->channels = c->cfg.channels;
  c->period_frames = c->cfg.period_frames > 0 ? c->cfg.period_frames : 1024;
  c->block_count = c->cfg.block_count > 0 ? (size_t)c->cfg.block_count : 8;
  atomic_init(&c->running, false);
  atomic_init(&c->st_drops, 0);
  atomic_init(&c->st_xruns, 0);
  atomic_init(&c->st_recovers, 0);
  atomic_init(&c->st_errors, 0);

  int rc = snd_pcm_open(&c->pcm, p->device, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
  if (rc < 0) {
    fprintf(stderr, "alsa_capturer: open %s failed: %s\n", p->device, snd_strerror(rc));
    free(c);
    errno = ENOENT;
    return NULL;
  }

  rc = alsa_setup_hw(c);
  if (rc < 0) {
    fprintf(stderr, "alsa_capturer: hw setup failed: %s\n", snd_strerror(rc));
    snd_pcm_close(c->pcm);
    free(c);
    errno = rc == -EINVAL ? EINVAL : ENOTSUP;
    return NULL;
  }

  c->period_bytes = (size_t)c->period_frames * (size_t)c->channels * 2;
  c->blocks = calloc(c->block_count, sizeof(uint8_t*));
  c->flags = calloc(c->block_count, sizeof(struct alsa_block_flag));
  c->scratch = malloc(c->period_bytes);
  if (!c->blocks || !c->flags || !c->scratch) goto fail;
  for (size_t i = 0; i < c->block_count; i++) {
    if (posix_memalign((void**)&c->blocks[i], 64, c->period_bytes) != 0) goto fail;
    atomic_init(&c->flags[i].in_use, false);
  }

  printf(
      "alsa_capturer: %s rate=%d ch=%d period=%d frames (%zu B, %u us) "
      "pool=%zu\n",
      p->device, c->rate, c->channels, c->period_frames, c->period_bytes,
      (unsigned)(c->period_frames * 1000000ULL / (unsigned)c->rate), c->block_count);
  return c;

fail:
  errno = ENOMEM;
  alsa_capturer_free(c);
  return NULL;
}

void alsa_capturer_free(struct alsa_capturer* c) {
  if (!c) return;
  alsa_capturer_stop(c);
  if (c->pcm) snd_pcm_close(c->pcm);
  if (c->blocks) {
    for (size_t i = 0; i < c->block_count; i++) free(c->blocks[i]);
  }
  free(c->blocks);
  free(c->flags);
  free(c->scratch);
  free(c);
}

void alsa_capturer_set_error_cb(struct alsa_capturer* c, void (*cb)(void* user_data),
                                void* user_data) {
  if (!c) return;
  c->error_cb = cb;
  c->error_user = user_data;
}

int alsa_capturer_start(struct alsa_capturer* c) {
  if (!c) return -EINVAL;
  if (atomic_load(&c->running)) return 0;
  atomic_store(&c->running, true);
  if (pthread_create(&c->thread, NULL, alsa_capture_loop, c) != 0) {
    atomic_store(&c->running, false);
    return -1;
  }
  c->thread_started = true;
  return 0;
}

void alsa_capturer_stop(struct alsa_capturer* c) {
  if (!c || !c->thread_started) return;
  atomic_store(&c->running, false);
  pthread_join(c->thread, NULL); /* poll timeout bounds the wait to 100ms */
  c->thread_started = false;
}

int alsa_capturer_sample_rate(const struct alsa_capturer* c) {
  return c ? c->rate : 0;
}

int alsa_capturer_channels(const struct alsa_capturer* c) {
  return c ? c->channels : 0;
}

int alsa_capturer_period_bytes(const struct alsa_capturer* c) {
  return c ? (int)c->period_bytes : 0;
}

void alsa_capturer_stats(const struct alsa_capturer* c, struct alsa_capturer_stats* out) {
  if (!out) return;
  if (!c) {
    memset(out, 0, sizeof(*out));
    return;
  }
  out->drops = atomic_load(&c->st_drops);
  out->xruns = atomic_load(&c->st_xruns);
  out->recovers = atomic_load(&c->st_recovers);
  out->errors = atomic_load(&c->st_errors);
}

#else /* !HAVE_ALSA */

#include <errno.h>

/* Stub: capturer cannot be created when ALSA is compiled out; the owner
 * falls back to video-only (same semantic as the file path). */
struct alsa_capturer* alsa_capturer_create(const struct alsa_capturer_params* p,
                                           alsa_pcm_cb cb, void* user_data) {
  (void)p;
  (void)cb;
  (void)user_data;
  errno = ENOTSUP;
  return NULL;
}
void alsa_capturer_free(struct alsa_capturer* c) {
  (void)c;
}
void alsa_capturer_set_error_cb(struct alsa_capturer* c, void (*cb)(void* user_data),
                                void* user_data) {
  (void)c;
  (void)cb;
  (void)user_data;
}
int alsa_capturer_start(struct alsa_capturer* c) {
  (void)c;
  return -ENOTSUP;
}
void alsa_capturer_stop(struct alsa_capturer* c) {
  (void)c;
}
int alsa_capturer_sample_rate(const struct alsa_capturer* c) {
  (void)c;
  return 0;
}
int alsa_capturer_channels(const struct alsa_capturer* c) {
  (void)c;
  return 0;
}
int alsa_capturer_period_bytes(const struct alsa_capturer* c) {
  (void)c;
  return 0;
}
void alsa_capturer_stats(const struct alsa_capturer* c, struct alsa_capturer_stats* out) {
  (void)c;
  if (out) memset(out, 0, sizeof(*out));
}

#endif /* HAVE_ALSA */
