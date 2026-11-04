#ifndef _LIVE_INGEST_RX_PIPELINE_H_
#define _LIVE_INGEST_RX_PIPELINE_H_

#include <stddef.h>
#include <stdint.h>
#include <st20_api.h>

struct ring_buffer;
struct slab_pool;
struct mtl_rx;
struct mtl_rx_bridge;
struct rx_pipeline;

struct rx_pipeline_params {
    const char* drm_device;     /* "/dev/dri/card0" */
    const char* drm_connector;  /* "HDMI-A-1" */
    const char* drm_mode;       /* "1920x1080@60" */
    int         width, height;
    int         drm_buf_count;  /* display buffer count (3-4) */
    int         num_slices;     /* matches tx codec config */

    /* input: connected to tx_pipeline output ring */
    struct ring_buffer* input_ring;

    /* Optional live-MTL sources for the 1s aggregation (NULL in loopback). */
    struct mtl_rx*        mtl_src;
    struct mtl_rx_bridge* bridge_src;

    /* slab pool: shared with tx_pipeline; NULL for MTL-only RX path */
    struct slab_pool*  coded_pool;

    /* Phase 5 MTL: return framebuff to ST22 RX after decode */
    st22_rx_handle     rx_handle;

    /* PTP clock for cross-machine E2E latency (NULL → CLOCK_MONOTONIC) */
    uint64_t (*time_now)(void* priv);
    void*              time_priv;

    /* affinity */
    int         cpu_decode;     /* CPU core for decode+render thread */
};

struct rx_pipeline* rx_pipeline_create(struct rx_pipeline_params* p);
void  rx_pipeline_free(struct rx_pipeline* rp);

int   rx_pipeline_start(struct rx_pipeline* rp);
void  rx_pipeline_stop(struct rx_pipeline* rp);

/** Get E2E latency stats after stop. All values in nanoseconds. */
void  rx_pipeline_get_latency(struct rx_pipeline* rp,
                              uint64_t* min_ns, uint64_t* avg_ns, uint64_t* max_ns);

/** RX-side observability snapshot (lock-free poll, cumulative unless noted). */
struct rx_pipeline_stats {
  uint64_t received, decoded, rendered;
  uint32_t rx_fps, dec_fps, render_fps; /* last 1s tick rates */
  uint64_t decode_errs;
  uint64_t reasm_drops;                 /* feed failures (total) */
  uint64_t reasm_timeout_flushes;
  uint64_t reasm_stop_flushes;
  uint64_t render_fails;
  /* latency */
  uint64_t lat_valid;
  uint64_t lat_zero;
  uint64_t lat_back;
  uint64_t lat_over;
  uint64_t lat_invalid;                 /* compat total: back + over */
  uint32_t lat_p50_us, lat_p90_us, lat_p99_us; /* 1s window percentiles */
  uint64_t lat_win_avg_us;
  uint64_t lat_cum_min_us, lat_cum_max_us;
  /* stream tracking */
  uint64_t sei_missing;
  uint64_t empty_payload;
  uint64_t seq_gap_events, seq_lost, seq_dups;
  /* hold ring */
  int      hold_in_use, hold_peak;
  /* decoder detail */
  uint64_t dec_frames_out;
  uint32_t dec_out_fps;
  uint32_t dec_take_ema_us;
  uint32_t dec_p50_us, dec_p90_us, dec_p99_us;
  uint64_t dec_feed_chunks, dec_pull_empty;
  uint64_t dec_corrupt, dec_discarded;
  uint64_t dec_input_full, dec_put_fails;
  uint64_t dec_info_changes, dec_res_changes;
  uint32_t dec_mpp_now, dec_mpp_peak;
  /* reassembler detail */
  uint64_t reasm_complete, reasm_waiting, reasm_errors;
  uint64_t reasm_oversize, reasm_alloc_fails, reasm_flushes;
  uint64_t reasm_slab_frames, reasm_malloc_frames;
  size_t   reasm_pending_peak;
  /* renderer detail */
  uint64_t disp_displays;
  uint32_t disp_fps;
  uint64_t disp_ema_us, flip_wait_ema_us;
  uint64_t flip_timeouts;
  int      flip_pending_peak;
  uint64_t addfb2_fails, flip_fails, disp_bad_args;
  int      disp_mode_fps;
  /* MTL RX detail */
  uint64_t mtl_received, mtl_bytes, mtl_pool_full;
  uint32_t mtl_drops;
  uint64_t mtl_ring_drops, mtl_stopping_drops;
  uint32_t mtl_fps;
  size_t   mtl_frame_max;
  int      mtl_inflight, mtl_peak;
  /* bridge detail */
  uint64_t bridge_bridged, bridge_bytes;
  uint32_t bridge_drops;
  uint64_t bridge_bad, bridge_slab, bridge_malloc, bridge_ring;
  uint32_t bridge_fps;
  uint64_t bridge_copy_ema_us;
  /* supervision */
  uint32_t rebuild_streak;
  uint64_t rebuild_decode_hang, rebuild_decode_errors, rebuild_render_fails;
  uint64_t silence_events;
  uint64_t last_rebuild_us;
  int      disabled;
  int      disabled_reason; /* 0 = none, 1 = rebuild storm */
};

/** One-shot RX snapshot (NULL-safe, never blocks the data path). */
void rx_pipeline_stats(struct rx_pipeline* rp, struct rx_pipeline_stats* out);

/** One-line human-readable snapshot, same fields as the 1s log. */
int  rx_pipeline_format_metrics(struct rx_pipeline* rp, char* buf, size_t len);

#endif /* _LIVE_INGEST_RX_PIPELINE_H_ */
