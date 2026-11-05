#define _GNU_SOURCE

#include "tx_pipeline_priv.h"

#include "ring_buffer.h"
#include "slab_pool.h"
#include "codec_plugin.h"
#include "../codec/rga_converter.h"
#include "../io/v4l2_capturer.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

void tx_pipeline_stats(struct tx_pipeline* tp, struct tx_pipeline_stats* out) {
    if (!out) return;
    if (!tp) {
        memset(out, 0, sizeof(*out));
        return;
    }
    out->captured = tp->stat_captured;
    out->encoded = tp->stat_encoded;
    out->encoded_bytes = tp->stat_encoded_bytes;
    out->ring_drops = tp->stat_ring_drops;
    out->sink_drops = tp->stat_sink_drops;
    out->send_fails = tp->stat_send_fails;
    out->recv_timeouts = tp->stat_recv_timeouts;
    out->payload_oversize = tp->stat_payload_oversize;
    out->invalid_frames = tp->stat_invalid_frames;
    out->mtl_acquire_fails = tp->stat_mtl_acquire_fails;
    out->mtl_publish_fails = tp->stat_mtl_publish_fails;
    out->enc2dec_full = tp->stat_enc2dec_full;
    if (tp->capture) {
        struct v4l2_capturer_stats cs;
        v4l2_capturer_stats(tp->capture, &cs);
        out->cap_outstanding = cs.outstanding;
        out->cap_peak = cs.peak_outstanding;
        out->cap_buf_count = cs.buf_count;
        out->cap_effective_fps = cs.effective_fps;
        out->cap_fps = cs.video_fps;
        out->cap_full_events = cs.full_events;
        out->cap_interval_us = cs.cap_interval_us;
        out->cap_ts_clamp = cs.ts_clamp_count;
        out->cap_ts_fallback = cs.ts_fallback_count;
    }
    if (tp->rga) {
        struct rga_stats rs;
        rga_converter_stats(tp->rga, &rs);
        out->rga_calls = rs.calls;
        out->rga_fails = rs.fails;
        out->rga_pool_exhausted = rs.pool_exhausted;
        out->rga_cpu_fallbacks = rs.cpu_fallbacks;
        out->rga_cpu_fails = rs.cpu_fails;
        out->rga_ema_us = rs.time_ema_us;
        out->rga_in_use = rs.pool_in_use;
        out->rga_peak = rs.pool_peak;
        out->rga_size = rs.pool_size;
        out->rga_degraded = rs.degraded;
    }
    if (tp->encoder && tp->encoder->get_encode_stats) {
        struct codec_encode_stats es;
        memset(&es, 0, sizeof(es));
        if (tp->encoder->get_encode_stats(tp->encoder->priv, &es) == 0) {
            out->enc_fps = es.encode_fps;
            out->enc_cost_us = es.encode_cost_us;
            out->enc_p50 = es.latency_p50_us;
            out->enc_p90 = es.latency_p90_us;
            out->enc_p99 = es.latency_p99_us;
            out->enc_avg_qp = es.avg_qp;
            out->enc_start_qp = es.start_qp;
            out->enc_bps_rt = es.bps_rt;
            out->enc_sse = es.sse;
            out->enc_pskip = es.pskip;
            out->enc_mpp_now = es.mpp_buf_now;
            out->enc_mpp_peak = es.mpp_buf_peak;
            out->enc_packets_total = es.packets_total;
            out->enc_oversize = es.oversize_drops;
            out->enc_meta_missing = es.meta_missing;
        }
        if (tp->encoder->get_full_drops) {
            uint64_t n = 0;
            if (tp->encoder->get_full_drops(tp->encoder->priv, &n) == 0)
                out->enc_ring_full = n;
        }
    }
    out->rebuild_streak = tp->consecutive_rebuilds;
    out->rebuild_errors = tp->rebuild_errors;
    out->rebuild_signals = tp->rebuild_signals;
    out->rebuild_cap_hang = tp->rebuild_cap_hang;
    out->rebuild_enc_hang = tp->rebuild_enc_hang;
    out->disabled = tp->disabled ? 1 : 0;
}

int tx_pipeline_format_metrics(struct tx_pipeline* tp, char* buf, size_t len) {
    struct tx_pipeline_stats s;
    tx_pipeline_stats(tp, &s);
    return snprintf(
        buf, len,
        "cap=%llu enc=%llu ring=%u sink=%u(send=%llu recv=%llu ovs=%llu inv=%llu "
        "acq=%llu pub=%llu e2e=%llu) cap_fps=%u cap_int=%lluus eff=%d out=%d/%d "
        "enc_fps=%u cost=%uus lat=%u/%u/%u qp=%d/%d bps=%d mpp=%u/%u rga=%llu/%llu/%llu/%llu use=%u/%u deg=%d rb=%llu/%llu/%llu/%llu%s",
        (unsigned long long)s.captured, (unsigned long long)s.encoded,
        s.ring_drops, s.sink_drops, (unsigned long long)s.send_fails,
        (unsigned long long)s.recv_timeouts,
        (unsigned long long)s.payload_oversize,
        (unsigned long long)s.invalid_frames,
        (unsigned long long)s.mtl_acquire_fails,
        (unsigned long long)s.mtl_publish_fails,
        (unsigned long long)s.enc2dec_full, s.cap_fps,
        (unsigned long long)s.cap_interval_us, s.cap_effective_fps,
        s.cap_outstanding, s.cap_buf_count, s.enc_fps, s.enc_cost_us, s.enc_p50,
        s.enc_p90, s.enc_p99, (int)s.enc_avg_qp, (int)s.enc_start_qp,
        (int)s.enc_bps_rt, s.enc_mpp_now, s.enc_mpp_peak,
        (unsigned long long)s.rga_calls, (unsigned long long)s.rga_fails,
        (unsigned long long)s.rga_pool_exhausted,
        (unsigned long long)s.rga_cpu_fallbacks, s.rga_in_use, s.rga_size,
        (int)s.rga_degraded, (unsigned long long)s.rebuild_errors,
        (unsigned long long)s.rebuild_signals,
        (unsigned long long)s.rebuild_cap_hang,
        (unsigned long long)s.rebuild_enc_hang, s.disabled ? " disabled" : "");
}

void tx_stats_log_1s(struct tx_pipeline* tp, struct tx_tick_state* tick) {
    uint64_t captured_now = tp->stat_captured;
    struct rga_stats rs;
    struct codec_encode_stats es;
    uint64_t packets_win = 0;
    memset(&es, 0, sizeof(es));
    es.avg_qp = es.start_qp = es.bps_rt = es.pskip = -1;
    es.sse = -1;
    rga_converter_stats(tp->rga, &rs);
    uint64_t full_drops = 0;
    if (tp->encoder && tp->encoder->get_full_drops)
        tp->encoder->get_full_drops(tp->encoder->priv, &full_drops);
    if (tp->encoder && tp->encoder->get_encode_stats)
        tp->encoder->get_encode_stats(tp->encoder->priv, &es);
    if (tp->encoder && tp->encoder->take_packets_in_window)
        tp->encoder->take_packets_in_window(tp->encoder->priv, &packets_win);
    int rc_bitrate = 0, rc_gop = 0, rc_qp = 0, rc_pending = 0;
    if (tp->encoder && tp->encoder->get_rc_state)
        tp->encoder->get_rc_state(tp->encoder->priv, &rc_bitrate, &rc_gop,
                                  &rc_qp, &rc_pending);
    printf(
        "tx_pipe[1s]: cap=%llu/s enc=%llu/s %llukbps "
        "ring_drop=%u sink_drop=%u "
        "(send=%llu recv=%llu oversize=%llu invalid=%llu "
        "acq=%llu pub=%llu e2e_full=%llu) "
        "cap_fps=%u cap_int=%lluus eff_fps=%d out=%d peak=%d full_ev=%llu "
        "ts_clamp=%llu ts_fb=%llu enc_fps=%u enc_cost=%uus "
        "lat=%u/%u/%uus qp=%d/%d bps=%d sse=%lld pskip=%d "
        "mpp=%u/%uB win_pkts=%llu "
        "enc_full=%llu streak=%u rc_pend=%d | "
        "rga ok=%llu fail=%llu pool_full=%llu cpu_fb=%llu "
        "cpu_fail=%llu use=%u/%u deg=%d ema=%lluus rb=%llu/%llu/%llu/%llu\n",
        (unsigned long long)(captured_now - tick->prev_captured),
        (unsigned long long)(tp->stat_encoded - tick->prev_encoded),
        (unsigned long long)((tp->stat_encoded_bytes - tick->prev_bytes) * 8 /
                             1000),
        tp->stat_ring_drops, tp->stat_sink_drops,
        (unsigned long long)(tp->stat_send_fails - tick->prev_send),
        (unsigned long long)(tp->stat_recv_timeouts - tick->prev_recv),
        (unsigned long long)(tp->stat_payload_oversize - tick->prev_oversize),
        (unsigned long long)(tp->stat_invalid_frames - tick->prev_invalid),
        (unsigned long long)(tp->stat_mtl_acquire_fails - tick->prev_acq),
        (unsigned long long)(tp->stat_mtl_publish_fails - tick->prev_pub),
        (unsigned long long)(tp->stat_enc2dec_full - tick->prev_e2e),
        tp->capture ? v4l2_capturer_video_fps(tp->capture) : 0,
        tp->capture
            ? (unsigned long long)v4l2_capturer_cap_interval_us(tp->capture)
            : 0ULL,
        tp->capture ? v4l2_capturer_effective_fps(tp->capture) : 0,
        tp->capture ? v4l2_capturer_outstanding(tp->capture) : -1,
        tp->capture ? v4l2_capturer_peak_outstanding(tp->capture) : -1,
        tp->capture
            ? (unsigned long long)v4l2_capturer_full_events(tp->capture)
            : 0ULL,
        tp->capture
            ? (unsigned long long)v4l2_capturer_ts_clamp_count(tp->capture)
            : 0ULL,
        tp->capture
            ? (unsigned long long)v4l2_capturer_ts_fallback_count(tp->capture)
            : 0ULL,
        (unsigned)es.encode_fps, (unsigned)es.encode_cost_us,
        (unsigned)es.latency_p50_us, (unsigned)es.latency_p90_us,
        (unsigned)es.latency_p99_us, (int)es.avg_qp, (int)es.start_qp,
        (int)es.bps_rt, (long long)es.sse, (int)es.pskip,
        (unsigned)es.mpp_buf_now, (unsigned)es.mpp_buf_peak,
        (unsigned long long)packets_win,
        (unsigned long long)(full_drops - tick->prev_full_drops),
        tp->consecutive_rebuilds, rc_pending,
        (unsigned long long)(rs.calls - tick->prev_rga_calls),
        (unsigned long long)(rs.fails - tick->prev_rga_fails),
        (unsigned long long)(rs.pool_exhausted - tick->prev_exhausted),
        (unsigned long long)(rs.cpu_fallbacks - tick->prev_cpu_fb),
        (unsigned long long)rs.cpu_fails, (unsigned)rs.pool_in_use,
        (unsigned)rs.pool_size, (int)rs.degraded,
        (unsigned long long)rs.time_ema_us,
        (unsigned long long)tp->rebuild_errors,
        (unsigned long long)tp->rebuild_signals,
        (unsigned long long)tp->rebuild_cap_hang,
        (unsigned long long)tp->rebuild_enc_hang);
    tick->prev_captured = captured_now;
    tick->prev_encoded = tp->stat_encoded;
    tick->prev_bytes = tp->stat_encoded_bytes;
    tick->prev_rga_calls = rs.calls;
    tick->prev_rga_fails = rs.fails;
    tick->prev_exhausted = rs.pool_exhausted;
    tick->prev_cpu_fb = rs.cpu_fallbacks;
    tick->prev_full_drops = full_drops;
    tick->prev_send = tp->stat_send_fails;
    tick->prev_recv = tp->stat_recv_timeouts;
    tick->prev_oversize = tp->stat_payload_oversize;
    tick->prev_invalid = tp->stat_invalid_frames;
    tick->prev_acq = tp->stat_mtl_acquire_fails;
    tick->prev_pub = tp->stat_mtl_publish_fails;
    tick->prev_e2e = tp->stat_enc2dec_full;
}
