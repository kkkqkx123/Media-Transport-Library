/* PTP Sync delivery probe (L1): proves Sync/Follow_Up reach the board and
 * shows per-hop correctionField accumulation, without the app or MTL.
 *
 * Build on the board: gcc ptp_sync_probe.c -o ~/code/mtl/bin/ptp_sync_probe
 * Usage: ptp_sync_probe [master] [domain] [seconds]
 *   master  = multicast group or unicast master IP (default 224.0.1.129)
 *   domain  = PTP domain (default 0)
 *   seconds = sample window (default 15)
 *
 * Exit 0 = at least one Sync/Follow_Up pair; 1 = none seen.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PTP_EVENT_PORT 319
#define PTP_GENERAL_PORT 320
#define PTP_GROUP_DEFAULT "224.0.1.129"

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

static int open_sock(uint16_t port, const struct in_addr* grp) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) return -1;
  int v = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons(port);
  if (bind(fd, (struct sockaddr*)&a, sizeof(a)) != 0) return -1;
  if (grp) {
    struct ip_mreq m;
    memset(&m, 0, sizeof(m));
    m.imr_multiaddr = *grp;
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) != 0)
      return -1;
  }
  return fd;
}

int main(int argc, char** argv) {
  const char* master = argc > 1 ? argv[1] : PTP_GROUP_DEFAULT;
  int domain = argc > 2 ? atoi(argv[2]) : 0;
  int secs = argc > 3 ? atoi(argv[3]) : 15;
  if (secs <= 0) secs = 15;

  struct in_addr grp, parsed;
  const struct in_addr* join = NULL;
  if (inet_pton(AF_INET, master, &parsed) != 1) {
    fprintf(stderr, "FAIL: bad master IP %s\n", master);
    return 1;
  }
  uint32_t h = ntohl(parsed.s_addr);
  if (h >= 0xE0000000U && h <= 0xEFFFFFFFU) {
    grp = parsed;
    join = &grp;
  }

  int ev = open_sock(PTP_EVENT_PORT, join);
  int gen = open_sock(PTP_GENERAL_PORT, join);
  if (ev < 0 || gen < 0) {
    fprintf(stderr, "FAIL: bind 319/320 (%s)\n", join ? "multicast" : "unicast");
    return 1;
  }
  int one = 1;
  setsockopt(ev, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof(one));
  printf("probe: listening %s domain=%d for %ds\n", master, domain, secs);

  uint8_t buf[256];
  int have_sync = 0;
  uint16_t sync_seq = 0;
  uint64_t sync_t2 = 0;
  int64_t sync_corr = 0;
  long pairs = 0, syncs = 0;
  int64_t corr_min = 0, corr_max = 0, off_min = 0, off_max = 0;

  struct timeval end;
  gettimeofday(&end, NULL);
  end.tv_sec += secs;
  while (1) {
    struct timeval now, tv = {0, 200000};
    gettimeofday(&now, NULL);
    if (now.tv_sec > end.tv_sec ||
        (now.tv_sec == end.tv_sec && now.tv_usec >= end.tv_usec))
      break;
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(ev, &rf);
    FD_SET(gen, &rf);
    int nfds = (ev > gen ? ev : gen) + 1;
    if (select(nfds, &rf, NULL, NULL, &tv) <= 0) continue;

    int fd = FD_ISSET(ev, &rf) ? ev : gen;
    struct msghdr mh;
    struct iovec iov;
    char ctl[CMSG_SPACE(sizeof(struct timespec))];
    memset(&mh, 0, sizeof(mh));
    iov.iov_base = buf;
    iov.iov_len = sizeof(buf);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = ctl;
    mh.msg_controllen = sizeof(ctl);
    ssize_t n = recvmsg(fd, &mh, 0);
    if (n < 34 || (buf[1] & 0x0F) != 2 || buf[4] != domain) continue;
    uint8_t type = buf[0] & 0x0F;

    uint64_t rx_ns = 0;
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
      if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS) {
        struct timespec* ts = (struct timespec*)CMSG_DATA(c);
        rx_ns = (uint64_t)ts->tv_sec * 1000000000ULL + ts->tv_nsec;
      }
    }
    if (!rx_ns) {
      struct timespec now;
      clock_gettime(CLOCK_REALTIME, &now);
      rx_ns = (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
    }

    if (fd == ev && type == 0x0 && n >= 44) { /* Sync */
      syncs++;
      uint16_t seq = be16(buf + 28);
      int64_t corr = be64s(buf + 8) / 65536;
      if ((be16(buf + 6) & 0x02) == 0) { /* one-step: Sync stands alone */
        uint64_t t1 = be48(buf + 34) * 1000000000ULL + be32(buf + 40);
        int64_t off = (int64_t)(rx_ns - t1) - corr;
        if (!pairs || corr < corr_min) corr_min = corr;
        if (!pairs || corr > corr_max) corr_max = corr;
        if (!pairs || off < off_min) off_min = off;
        if (!pairs || off > off_max) off_max = off;
        pairs++;
        printf("pair seq=%u one-step t1=%llu corr=%lld ns raw_off=%lld ns\n",
               seq, (unsigned long long)t1, (long long)corr, (long long)off);
        continue;
      }
      have_sync = 1;
      sync_seq = seq;
      sync_corr = corr;
      sync_t2 = rx_ns;
    } else if (fd == gen && type == 0x8 && n >= 44 && have_sync &&
               be16(buf + 28) == sync_seq) { /* Follow_Up */
      have_sync = 0;
      uint64_t t1 = be48(buf + 34) * 1000000000ULL + be32(buf + 40);
      int64_t corr = sync_corr + be64s(buf + 8) / 65536;
      int64_t off = (int64_t)(sync_t2 - t1) - corr;
      if (!pairs || corr < corr_min) corr_min = corr;
      if (!pairs || corr > corr_max) corr_max = corr;
      if (!pairs || off < off_min) off_min = off;
      if (!pairs || off > off_max) off_max = off;
      pairs++;
      printf("pair seq=%u t1=%llu corr=%lld ns t2=%llu raw_off=%lld ns\n",
             sync_seq, (unsigned long long)t1, (long long)corr,
             (unsigned long long)sync_t2, (long long)off);
    }
  }
  printf("probe: syncs=%ld pairs=%ld corr=[%lld,%lld] ns raw_off=[%lld,%lld] ns\n",
         syncs, pairs, (long long)corr_min, (long long)corr_max,
         (long long)off_min, (long long)off_max);
  if (!pairs) {
    printf("FAIL: no Sync/Follow_Up pair in %ds\n", secs);
    return 1;
  }
  printf("PASS\n");
  return 0;
}
