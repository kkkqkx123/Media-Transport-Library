#ifndef _LIVE_INGEST_ALSA_CAPTURER_H_
#define _LIVE_INGEST_ALSA_CAPTURER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Opaque ALSA capturer handle (requires HAVE_ALSA at build time). */
struct alsa_capturer;

struct alsa_capturer_params {
  const char* device; /* ALSA device name: "hw:0,0" / "default" */
  int sample_rate;    /* requested rate; actual is negotiated read-back */
  int channels;       /* requested channels; actual is negotiated read-back */
  int sample_bit;     /* only 16 supported (matches ST30_FMT_PCM16) */
  int period_frames;  /* frames per read; default 1024 (~21ms @48k) */
  int block_count;    /* PCM block pool size; default 8 */
};

/** Stats snapshot (lock-free counters). */
struct alsa_capturer_stats {
  uint64_t drops;    /* periods discarded because the block pool was full */
  uint64_t xruns;    /* ALSA overrun recovery events */
  uint64_t recovers; /* successful in-place recoveries */
  uint64_t errors;   /* fatal stop (consecutive-error threshold reached) */
};

/**
 * Called on the capture thread for one full period of S16LE interleaved PCM.
 * Must not block. pts_ns is the monotonic timestamp of the FIRST sample of
 * this block (snd_pcm_htimestamp corrected; falls back to read-completion
 * time minus period duration when unavailable).
 *
 * release must be called exactly once when the block is consumed (including
 * error paths) — it returns the block to the pool. The data pointer stays
 * valid until release.
 */
typedef void (*alsa_pcm_cb)(void* user_data, const void* data, size_t len,
                            uint64_t pts_ns,
                            void (*release)(void* user_data, void* data));

/**
 * Create an ALSA capturer. Non-blocking open with HW param negotiation and
 * read-back verification. The capture thread is NOT started yet.
 * @return handle, NULL on failure (errno set):
 *         ENOENT = device cannot be opened, EINVAL = bad params,
 *         ENOTSUP = negotiated rate is not 48k family or sample_bit != 16.
 */
struct alsa_capturer* alsa_capturer_create(const struct alsa_capturer_params* p,
                                           alsa_pcm_cb cb, void* user_data);
void alsa_capturer_free(struct alsa_capturer* c);

/** Optional error hook: fires once when the capture loop stopped after
 *  repeated non-transient errors. Owner should rebuild (create again). */
void alsa_capturer_set_error_cb(struct alsa_capturer* c, void (*cb)(void* user_data),
                                void* user_data);

/** Start the capture thread. */
int alsa_capturer_start(struct alsa_capturer* c);
/** Signal the capture thread to stop and join (bounded, poll timeout backed). */
void alsa_capturer_stop(struct alsa_capturer* c);

/** Negotiated read-back values (single source of truth). */
int alsa_capturer_sample_rate(const struct alsa_capturer* c);
int alsa_capturer_channels(const struct alsa_capturer* c);
int alsa_capturer_period_bytes(const struct alsa_capturer* c);

void alsa_capturer_stats(const struct alsa_capturer* c, struct alsa_capturer_stats* out);

#endif /* _LIVE_INGEST_ALSA_CAPTURER_H_ */
