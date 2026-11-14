#ifndef _ONE_WAY_PTP_H_
#define _ONE_WAY_PTP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ptp_time.h"

#define ONE_WAY_PTP_GROUP_DEFAULT "224.0.1.129"
#define ONE_WAY_PTP_EVENT_PORT    319
#define ONE_WAY_PTP_GENERAL_PORT  320

/** Slave configuration. Unicast master means static pre-configured destination without negotiation. */
struct one_way_ptp_cfg {
  char master[64]; /* group or unicast master IP, empty = default group */
  char ifname[16]; /* bind/join interface, empty = kernel route choice */
  uint8_t domain;  /* PTP domain, SMPTE ST 2059-2 uses 127 */
  int64_t nominal_delay_ns;  /* one-way path delay constant (E2E TC / direct) */
  uint32_t holdover_enter_ms; /* no-Sync time before holdover extrapolate */
  uint32_t holdover_exit_ms;  /* holdover time before giving up to free-run */
  uint32_t stats_interval_s;  /* periodic status log, 0 disables */
};

/** Snapshot for monitoring (dvs_platform query_state). */
struct one_way_ptp_status {
  int64_t offset_ns;    /* filtered raw offset (local minus master) */
  int64_t filtered_ns;  /* filtered raw offset applied, ctx holds negation */
  int64_t drift_ppb;    /* frequency estimate, positive = local clock fast */
  bool locked;
  bool holdover;
  uint64_t sync_count;
  uint64_t reject_count;
  uint64_t last_sync_mono_ns;
  uint8_t master_id[8];
};

struct one_way_ptp;

/** Fill defaults (group join, domain 127, 20us nominal delay, 3s/300s holdover). */
void one_way_ptp_cfg_defaults(struct one_way_ptp_cfg* cfg);

/**
 * Create slave bound to cfg->master ctx. Sockets open on start, so create
 * never fails on network state. Process-lifetime use needs no stop: exit
 * reclaims the thread and sockets; stop/free exist for reconfigure paths.
 */
struct one_way_ptp* one_way_ptp_create(const struct one_way_ptp_cfg* cfg,
                                       mp4_ptp_time_ctx* tctx);
int one_way_ptp_start(struct one_way_ptp* s);
void one_way_ptp_stop(struct one_way_ptp* s);
void one_way_ptp_free(struct one_way_ptp* s);
int one_way_ptp_status(const struct one_way_ptp* s,
                       struct one_way_ptp_status* out);

/* Pure helpers without sockets, kept for unit checks. */

/** correctionField raw units (ns << 16) to whole nanoseconds. */
int64_t one_way_ptp_correction_to_ns(int64_t correction_raw);

/** Offset sample: t2 minus (t1 + switch correction + nominal path delay). */
int64_t one_way_ptp_sample_offset_ns(uint64_t t1_ns, int64_t correction_ns,
                                      uint64_t t2_ns, int64_t nominal_delay_ns);

/** Median of n samples (n <= 16), insertion sort on a stack copy. */
int64_t one_way_ptp_median_ns(const int64_t* samples, int n);

#endif /* _ONE_WAY_PTP_H_ */
