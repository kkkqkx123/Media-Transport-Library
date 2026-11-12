#include "one_way_ptp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <ifaddrs.h>

#define PTP_V2 2
#define PTP_MSG_SYNC 0x0
#define PTP_MSG_FOLLOW_UP 0x8
#define PTP_MSG_ANNOUNCE 0xB
#define PTP_TWOSTEP_FLAG 0x02
#define PTP_HDR_LEN 34
#define PTP_TS_LEN 10
#define PTP_MAX_MSG 256

#define MEDIAN_WIN 7
#define LOCK_THRESH_NS 200000LL
#define LOCK_NEED 50
#define UNLOCK_THRESH_NS 1000000LL
#define REJECT_THRESH_NS 5000000LL
#define REJECT_REANCHOR 10
#define DRIFT_WIN_NS (30ULL * 1000000000ULL)
#define POLL_TIMEOUT_MS 500

static uint16_t be16(const uint8_t* p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be48(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 6; i++) v = (v << 8) | p[i];
  return v;
}

static int64_t be64s(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
  int64_t s;
  memcpy(&s, &v, sizeof(s));
  return s;
}

int64_t one_way_ptp_correction_to_ns(int64_t correction_raw) {
  return correction_raw / 65536;
}

int64_t one_way_ptp_sample_offset_ns(uint64_t t1_ns, int64_t correction_ns,
                                      uint64_t t2_ns, int64_t nominal_delay_ns) {
  return (int64_t)(t2_ns - t1_ns) - correction_ns - nominal_delay_ns;
}

int64_t one_way_ptp_median_ns(const int64_t* samples, int n) {
  int64_t tmp[16];
  if (!samples || n <= 0) return 0;
  if (n > 16) n = 16;
  memcpy(tmp, samples, (size_t)n * sizeof(tmp[0]));
  for (int i = 1; i < n; i++) {
    int64_t v = tmp[i];
    int j = i - 1;
    while (j >= 0 && tmp[j] > v) {
      tmp[j + 1] = tmp[j];
      j--;
    }
    tmp[j + 1] = v;
  }
  return tmp[n / 2];
}

void one_way_ptp_cfg_defaults(struct one_way_ptp_cfg* cfg) {
  if (!cfg) return;
  memset(cfg, 0, sizeof(*cfg));
  cfg->domain = 0;
  cfg->nominal_delay_ns = 20000;
  cfg->holdover_enter_ms = 3000;
  cfg->holdover_exit_ms = 300000;
  cfg->stats_interval_s = 10;
}

struct one_way_ptp {
  struct one_way_ptp_cfg cfg;
  mp4_ptp_time_ctx* tctx;
  int ev_fd;
  int gen_fd;
  pthread_t th;
  pthread_mutex_t lock;
  atomic_bool stop;
  bool running;
  bool have_master;
  uint8_t master_id[8];
  bool pending;
  uint16_t pending_seq;
  uint64_t pending_t1_ns;
  int64_t pending_corr_ns;
  uint64_t pending_t2_ns;
  int64_t med_win[MEDIAN_WIN];
  int med_count;
  bool have_filt;
  int64_t filt_ns;
  int lock_count;
  bool locked;
  bool holdover;
  int consec_reject;
  int64_t win_raw0;
  uint64_t win_mono0;
  bool win_open;
  int64_t drift_ppb;
  int64_t applied_ns;
  uint64_t last_sync_mono_ns;
  uint64_t last_stats_mono_ns;
  uint64_t sync_count;
  uint64_t reject_count;
};

static uint64_t mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static bool parse_addr(const char* s, struct in_addr* out) {
  return s && s[0] && inet_pton(AF_INET, s, out) == 1;
}

static bool is_multicast(const struct in_addr* a) {
  uint32_t h = ntohl(a->s_addr);
  return h >= 0xE0000000U && h <= 0xEFFFFFFFU;
}

static int open_ptp_socket(uint16_t port, const struct in_addr* group,
                           const char* ifname) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) return -1;
  int v = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  if (group) {
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr = *group;
    if (ifname && ifname[0]) {
      struct ifaddrs* list = NULL;
      if (getifaddrs(&list) == 0) {
        for (struct ifaddrs* it = list; it; it = it->ifa_next) {
          if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
          if (strcmp(it->ifa_name, ifname) != 0) continue;
          mreq.imr_interface =
              ((struct sockaddr_in*)it->ifa_addr)->sin_addr;
          break;
        }
        freeifaddrs(list);
      }
    }
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) !=
        0) {
      close(fd);
      return -1;
    }
  }
  return fd;
}

static void slave_push(struct one_way_ptp* s) {
  mp4_ptp_apply_correction(s->tctx, s->applied_ns, s->drift_ppb, s->locked,
                           s->holdover);
}

static void slave_reanchor(struct one_way_ptp* s, int64_t raw) {
  for (int i = 0; i < MEDIAN_WIN; i++) s->med_win[i] = raw;
  s->med_count = MEDIAN_WIN;
  s->have_filt = true;
  s->filt_ns = raw;
  s->consec_reject = 0;
  s->applied_ns = raw;
}

static void slave_drift_track(struct one_way_ptp* s, int64_t raw,
                              uint64_t now) {
  if (!s->win_open) {
    s->win_open = true;
    s->win_raw0 = raw;
    s->win_mono0 = now;
    return;
  }
  if (now - s->win_mono0 < DRIFT_WIN_NS) return;
  uint64_t dt = now - s->win_mono0;
  if (dt > 0)
    s->drift_ppb = (int64_t)(((raw - s->win_raw0) * 1000000000LL) /
                             (int64_t)dt);
  s->win_raw0 = raw;
  s->win_mono0 = now;
}

static void slave_accept(struct one_way_ptp* s, int64_t raw, uint64_t now) {
  if (!s->have_filt) {
    slave_reanchor(s, raw);
    s->win_open = false;
    slave_drift_track(s, raw, now);
    s->last_sync_mono_ns = now;
    s->sync_count++;
    if (s->holdover) {
      s->holdover = false;
      printf("one_way_ptp: master back, leaving holdover\n");
    }
    slave_push(s);
    return;
  }
  int64_t dev = raw - s->filt_ns;
  if (dev < 0) dev = -dev;
  if (dev > REJECT_THRESH_NS) {
    s->reject_count++;
    if (++s->consec_reject >= REJECT_REANCHOR) {
      slave_reanchor(s, raw);
      s->win_open = false;
      slave_drift_track(s, raw, now);
    }
    return;
  }
  s->consec_reject = 0;
  if (s->med_count < MEDIAN_WIN)
    s->med_win[s->med_count++] = raw;
  else {
    memmove(s->med_win, s->med_win + 1,
            (MEDIAN_WIN - 1) * sizeof(s->med_win[0]));
    s->med_win[MEDIAN_WIN - 1] = raw;
  }
  int64_t med = one_way_ptp_median_ns(s->med_win, s->med_count);
  s->filt_ns += (med - s->filt_ns) / 8;
  slave_drift_track(s, raw, now);
  s->last_sync_mono_ns = now;
  s->sync_count++;
  if (s->holdover) {
    s->holdover = false;
    printf("one_way_ptp: master back, leaving holdover\n");
  }
  int64_t a = s->filt_ns < 0 ? -s->filt_ns : s->filt_ns;
  if (!s->locked) {
    if (a <= LOCK_THRESH_NS && ++s->lock_count >= LOCK_NEED) {
      s->locked = true;
      printf("one_way_ptp: locked (offset %lld ns)\n",
             (long long)s->filt_ns);
    }
  } else if (a > UNLOCK_THRESH_NS) {
    s->locked = false;
    s->lock_count = 0;
    printf("one_way_ptp: unlock (offset %lld ns)\n", (long long)s->filt_ns);
  }
  s->applied_ns = s->filt_ns;
  slave_push(s);
}

static void slave_holdover_tick(struct one_way_ptp* s, uint64_t now) {
  if (!s->have_filt || s->last_sync_mono_ns == 0) return;
  uint64_t quiet_ms = (now - s->last_sync_mono_ns) / 1000000ULL;
  if (!s->holdover) {
    if (quiet_ms < s->cfg.holdover_enter_ms) return;
    s->holdover = true;
    s->locked = false;
    s->lock_count = 0;
    s->have_master = false;
    printf("one_way_ptp: no Sync for %llu ms, holdover (drift %lld ppb)\n",
           (unsigned long long)quiet_ms, (long long)s->drift_ppb);
  } else if (quiet_ms > s->cfg.holdover_exit_ms) {
    return;
  }
  uint64_t dt_ns = now - s->last_sync_mono_ns;
  int64_t extra = (int64_t)((s->drift_ppb * (int64_t)dt_ns) / 1000000000LL);
  s->applied_ns = s->filt_ns + extra;
  slave_push(s);
}

static void slave_stats_tick(struct one_way_ptp* s, uint64_t now) {
  if (s->cfg.stats_interval_s == 0) return;
  if (s->last_stats_mono_ns &&
      now - s->last_stats_mono_ns <
          (uint64_t)s->cfg.stats_interval_s * 1000000000ULL)
    return;
  s->last_stats_mono_ns = now;
  printf("one_way_ptp: %s%s offset=%lld ns drift=%lld ppb sync=%llu rej=%llu\n",
         s->locked ? "locked" : "free", s->holdover ? "+holdover" : "",
         (long long)s->filt_ns, (long long)s->drift_ppb,
         (unsigned long long)s->sync_count,
         (unsigned long long)s->reject_count);
}

static bool slave_check_master(struct one_way_ptp* s, const uint8_t* id) {
  if (!s->have_master) {
    memcpy(s->master_id, id, sizeof(s->master_id));
    s->have_master = true;
    return true;
  }
  return memcmp(s->master_id, id, sizeof(s->master_id)) == 0;
}

static void slave_on_sync(struct one_way_ptp* s, const uint8_t* msg, int len,
                          uint64_t t2_ns) {
  if (len < PTP_HDR_LEN + PTP_TS_LEN) return;
  pthread_mutex_lock(&s->lock);
  if (!slave_check_master(s, msg + 18)) {
    pthread_mutex_unlock(&s->lock);
    return;
  }
  bool twostep = (be16(msg + 6) & PTP_TWOSTEP_FLAG) != 0;
  uint16_t seq = be16(msg + 28);
  int64_t corr = one_way_ptp_correction_to_ns(be64s(msg + 8));
  uint64_t t1 = be48(msg + PTP_HDR_LEN) * 1000000000ULL +
                be32(msg + PTP_HDR_LEN + 6);
  if (twostep) {
    s->pending = true;
    s->pending_seq = seq;
    s->pending_t1_ns = t1;
    s->pending_corr_ns = corr;
    s->pending_t2_ns = t2_ns;
    pthread_mutex_unlock(&s->lock);
    return;
  }
  int64_t raw =
      one_way_ptp_sample_offset_ns(t1, corr, t2_ns, s->cfg.nominal_delay_ns);
  slave_accept(s, raw, mono_ns());
  pthread_mutex_unlock(&s->lock);
}

static void slave_on_follow_up(struct one_way_ptp* s, const uint8_t* msg,
                               int len) {
  if (len < PTP_HDR_LEN + PTP_TS_LEN) return;
  pthread_mutex_lock(&s->lock);
  if (!s->pending || !slave_check_master(s, msg + 18)) {
    pthread_mutex_unlock(&s->lock);
    return;
  }
  uint16_t seq = be16(msg + 28);
  if (seq != s->pending_seq) {
    pthread_mutex_unlock(&s->lock);
    return;
  }
  s->pending = false;
  uint64_t t1 = be48(msg + PTP_HDR_LEN) * 1000000000ULL +
                be32(msg + PTP_HDR_LEN + 6);
  int64_t corr =
      s->pending_corr_ns + one_way_ptp_correction_to_ns(be64s(msg + 8));
  int64_t raw = one_way_ptp_sample_offset_ns(t1, corr, s->pending_t2_ns,
                                             s->cfg.nominal_delay_ns);
  slave_accept(s, raw, mono_ns());
  pthread_mutex_unlock(&s->lock);
}

static void slave_on_announce(struct one_way_ptp* s, const uint8_t* msg,
                              int len) {
  if (len < PTP_HDR_LEN) return;
  pthread_mutex_lock(&s->lock);
  slave_check_master(s, msg + 18);
  pthread_mutex_unlock(&s->lock);
}

static uint64_t slave_recv_t2(int fd, uint8_t* buf, int* len_out) {
  struct msghdr mh;
  struct iovec iov;
  union {
    char b[CMSG_SPACE(sizeof(struct timespec))];
  } ctl;
  memset(&mh, 0, sizeof(mh));
  iov.iov_base = buf;
  iov.iov_len = PTP_MAX_MSG;
  mh.msg_iov = &iov;
  mh.msg_iovlen = 1;
  mh.msg_control = ctl.b;
  mh.msg_controllen = sizeof(ctl.b);
  ssize_t n = recvmsg(fd, &mh, 0);
  if (n < PTP_HDR_LEN) return 0;
  *len_out = (int)n;
  for (struct cmsghdr* c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS) {
      struct timespec* ts = (struct timespec*)CMSG_DATA(c);
      int tai = mp4_ptp_tai_offset_sec();
      return (uint64_t)(ts->tv_sec + tai) * 1000000000ULL + ts->tv_nsec;
    }
  }
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  int tai = mp4_ptp_tai_offset_sec();
  return (uint64_t)(now.tv_sec + tai) * 1000000000ULL + now.tv_nsec;
}

static void* slave_thread(void* arg) {
  struct one_way_ptp* s = arg;
  struct pollfd pf[2];
  pf[0].fd = s->ev_fd;
  pf[0].events = POLLIN;
  pf[1].fd = s->gen_fd;
  pf[1].events = POLLIN;
  uint8_t buf[PTP_MAX_MSG];
  while (!atomic_load(&s->stop)) {
    int r = poll(pf, 2, POLL_TIMEOUT_MS);
    uint64_t now = mono_ns();
    if (r > 0) {
      if (pf[0].revents & POLLIN) {
        int len = 0;
        uint64_t t2 = slave_recv_t2(s->ev_fd, buf, &len);
        if (t2 && (buf[0] & 0x0F) == PTP_MSG_SYNC &&
            (buf[1] & 0x0F) == PTP_V2 && buf[4] == s->cfg.domain)
          slave_on_sync(s, buf, len, t2);
      }
      if (pf[1].revents & POLLIN) {
        ssize_t n = recv(s->gen_fd, buf, sizeof(buf), 0);
        if (n >= PTP_HDR_LEN && (buf[1] & 0x0F) == PTP_V2 &&
            buf[4] == s->cfg.domain) {
          uint8_t type = buf[0] & 0x0F;
          if (type == PTP_MSG_FOLLOW_UP)
            slave_on_follow_up(s, buf, (int)n);
          else if (type == PTP_MSG_ANNOUNCE)
            slave_on_announce(s, buf, (int)n);
        }
      }
    }
    pthread_mutex_lock(&s->lock);
    slave_holdover_tick(s, now);
    slave_stats_tick(s, now);
    pthread_mutex_unlock(&s->lock);
  }
  return NULL;
}

struct one_way_ptp* one_way_ptp_create(const struct one_way_ptp_cfg* cfg,
                                       mp4_ptp_time_ctx* tctx) {
  if (!tctx) return NULL;
  struct one_way_ptp* s = calloc(1, sizeof(*s));
  if (!s) return NULL;
  if (cfg)
    s->cfg = *cfg;
  else
    one_way_ptp_cfg_defaults(&s->cfg);
  s->tctx = tctx;
  s->ev_fd = -1;
  s->gen_fd = -1;
  pthread_mutex_init(&s->lock, NULL);
  atomic_init(&s->stop, false);
  return s;
}

int one_way_ptp_start(struct one_way_ptp* s) {
  if (!s) return -1;
  pthread_mutex_lock(&s->lock);
  if (s->running) {
    pthread_mutex_unlock(&s->lock);
    return 0;
  }
  pthread_mutex_unlock(&s->lock);

  struct in_addr group;
  const struct in_addr* join = NULL;
  struct in_addr parsed;
  if (s->cfg.master[0]) {
    if (!parse_addr(s->cfg.master, &parsed)) return -1;
    if (is_multicast(&parsed)) join = &parsed;
  } else {
    if (!parse_addr(ONE_WAY_PTP_GROUP_DEFAULT, &group)) return -1;
    join = &group;
  }
  const char* ifname = s->cfg.ifname[0] ? s->cfg.ifname : NULL;

  int ev = open_ptp_socket(ONE_WAY_PTP_EVENT_PORT, join, ifname);
  if (ev < 0) return -1;
  int one = 1;
  setsockopt(ev, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof(one));
  int gen = open_ptp_socket(ONE_WAY_PTP_GENERAL_PORT, join, ifname);
  if (gen < 0) {
    close(ev);
    return -1;
  }
  s->ev_fd = ev;
  s->gen_fd = gen;
  atomic_store(&s->stop, false);
  if (pthread_create(&s->th, NULL, slave_thread, s) != 0) {
    close(ev);
    close(gen);
    s->ev_fd = s->gen_fd = -1;
    return -1;
  }
  pthread_mutex_lock(&s->lock);
  s->running = true;
  pthread_mutex_unlock(&s->lock);
  printf("one_way_ptp: listening %s domain=%u nominal_delay=%lld ns%s%s\n",
         s->cfg.master[0] ? s->cfg.master : ONE_WAY_PTP_GROUP_DEFAULT,
         s->cfg.domain, (long long)s->cfg.nominal_delay_ns,
         ifname ? " if=" : "", ifname ? ifname : "");
  return 0;
}

void one_way_ptp_stop(struct one_way_ptp* s) {
  if (!s) return;
  pthread_mutex_lock(&s->lock);
  bool was = s->running;
  pthread_mutex_unlock(&s->lock);
  if (!was) return;
  atomic_store(&s->stop, true);
  pthread_join(s->th, NULL);
  pthread_mutex_lock(&s->lock);
  s->running = false;
  pthread_mutex_unlock(&s->lock);
  if (s->ev_fd >= 0) {
    close(s->ev_fd);
    s->ev_fd = -1;
  }
  if (s->gen_fd >= 0) {
    close(s->gen_fd);
    s->gen_fd = -1;
  }
}

void one_way_ptp_free(struct one_way_ptp* s) {
  if (!s) return;
  one_way_ptp_stop(s);
  pthread_mutex_destroy(&s->lock);
  free(s);
}

int one_way_ptp_status(const struct one_way_ptp* s,
                       struct one_way_ptp_status* out) {
  if (!s || !out) return -1;
  struct one_way_ptp* m = (struct one_way_ptp*)s;
  pthread_mutex_lock(&m->lock);
  out->offset_ns = m->have_filt ? m->filt_ns : 0;
  out->filtered_ns = m->applied_ns;
  out->drift_ppb = m->drift_ppb;
  out->locked = m->locked;
  out->holdover = m->holdover;
  out->sync_count = m->sync_count;
  out->reject_count = m->reject_count;
  out->last_sync_mono_ns = m->last_sync_mono_ns;
  memcpy(out->master_id, m->master_id, sizeof(out->master_id));
  pthread_mutex_unlock(&m->lock);
  return 0;
}
