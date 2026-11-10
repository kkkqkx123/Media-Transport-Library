#pragma once
#include <cstdio>
#include <string>
#include <thread>
#include <chrono>

/* Single CPU-utilization source for heartbeat resources and /capacity.
 * Both callers use double-sample deltas over /proc/stat; a single snapshot
 * (busy/total) is not a utilization. Blocking ~120ms per call keeps the
 * 3s heartbeat and the on-demand capacity probe honest without a shared
 * sampler thread. Returns -1 when /proc/stat is unreadable. */
inline bool dvs_read_cpu_times(long long& total_out, long long& idle_out) {
  FILE* f = fopen("/proc/stat", "r");
  if (!f) return false;
  char line[512];
  if (!fgets(line, sizeof(line), f)) {
    fclose(f);
    return false;
  }
  fclose(f);
  long long u = 0, n = 0, s = 0, idle = 0, iow = 0, irq = 0, sirq = 0,
            steal = 0;
  if (sscanf(line + 4, "%lld %lld %lld %lld %lld %lld %lld %lld", &u, &n, &s,
             &idle, &iow, &irq, &sirq, &steal) < 4)
    return false;
  long long idle_all = idle + iow;
  long long total = u + n + s + idle + iow + irq + sirq + steal;
  if (total <= 0) return false;
  total_out = total;
  idle_out = idle_all;
  return true;
}

inline double dvs_cpu_percent_blocking(int interval_ms = 120) {
  long long t0 = 0, i0 = 0, t1 = 0, i1 = 0;
  if (!dvs_read_cpu_times(t0, i0)) return -1.0;
  std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  if (!dvs_read_cpu_times(t1, i1)) return -1.0;
  long long dt = t1 - t0;
  long long di = i1 - i0;
  if (dt <= 0) return -1.0;
  double pct = 100.0 * (double)(dt - di) / (double)dt;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}
