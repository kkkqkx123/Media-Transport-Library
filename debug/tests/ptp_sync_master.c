/* Lab PTP master (no switch, two boards): userspace two-step Sync sender.
 * Fills the gap where ptp4l cannot run (YT6801 has no TX timestamp).
 * Production still uses x86 ptp4l; this tool is verification-only.
 *
 * Build on the board: gcc ptp_sync_master.c -o ~/code/mtl/bin/ptp_sync_master
 * Usage: ptp_sync_master [group] [domain] [sync_hz]
 *   group   = multicast group (default 224.0.1.129)
 *   domain  = PTP domain (default 0)
 *   sync_hz = Sync rate (default 8)
 *
 * Sends Sync (two-step) + Follow_Up pairs and periodic Announces.
 * correctionField is zero (no switch in path). Runs until SIGINT.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PTP_EVENT_PORT 319
#define PTP_GENERAL_PORT 320
#define PTP_GROUP_DEFAULT "224.0.1.129"
#define PTP_HDR_LEN 34

static volatile int g_run = 1;
static void on_sig(int s) {
  (void)s;
  g_run = 0;
}

static void put16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

static void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static void put48(uint8_t* p, uint64_t v) {
  for (int i = 5; i >= 0; i--) {
    p[i] = (uint8_t)v;
    v >>= 8;
  }
}

static uint64_t tai_now_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_TAI, &ts) != 0) clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void hdr(uint8_t* m, uint8_t type, uint8_t domain, uint16_t seq,
                const uint8_t* id, uint8_t ctrl, int8_t interval) {
  memset(m, 0, PTP_HDR_LEN);
  m[0] = type;
  m[1] = 0x02;
  put16(m + 2, PTP_HDR_LEN + 10);
  m[4] = domain;
  if (type == 0x0) put16(m + 6, 0x02); /* Sync: two-step flag */
  memcpy(m + 18, id, 8);
  put16(m + 26, 1);
  put16(m + 28, seq);
  m[30] = ctrl;
  m[31] = (uint8_t)interval;
}

int main(int argc, char** argv) {
  const char* group = argc > 1 ? argv[1] : PTP_GROUP_DEFAULT;
  int domain = argc > 2 ? atoi(argv[2]) : 0;
  int hz = argc > 3 ? atoi(argv[3]) : 8;
  if (hz < 1) hz = 1;
  if (hz > 64) hz = 64;

  struct in_addr dst;
  if (inet_pton(AF_INET, group, &dst) != 1) {
    fprintf(stderr, "FAIL: bad group %s\n", group);
    return 1;
  }
  signal(SIGINT, on_sig);
  signal(SIGTERM, on_sig);

  int ev = socket(AF_INET, SOCK_DGRAM, 0);
  int gen = socket(AF_INET, SOCK_DGRAM, 0);
  if (ev < 0 || gen < 0) return 1;
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr = dst;
  uint8_t id[8] = {0x00, 0x1B, 0x19, 'P', 'T', 'P', 0x00, 0x00};
  id[7] = (uint8_t)(ntohl(dst.s_addr) & 0xFF);

  uint8_t sync[PTP_HDR_LEN + 10], fu[PTP_HDR_LEN + 10];
  uint8_t ann[PTP_HDR_LEN + 30];
  uint16_t sync_seq = 0, ann_seq = 0;
  long pairs = 0;
  printf("master: sending to %s domain=%d at %dHz (id %02x..%02x)\n", group,
         domain, hz, id[0], id[7]);

  struct timespec gap = {0, 1000000000L / hz};
  while (g_run) {
    a.sin_port = htons(PTP_EVENT_PORT);
    uint64_t est = tai_now_ns();
    hdr(sync, 0x0, (uint8_t)domain, sync_seq, id, 0x00, -3);
    put48(sync + PTP_HDR_LEN, est / 1000000000ULL);
    put32(sync + PTP_HDR_LEN + 6, (uint32_t)(est % 1000000000ULL));
    sendto(ev, sync, sizeof(sync), 0, (struct sockaddr*)&a, sizeof(a));

    uint64_t precise = tai_now_ns();
    a.sin_port = htons(PTP_GENERAL_PORT);
    hdr(fu, 0x8, (uint8_t)domain, sync_seq, id, 0x02, -3);
    put48(fu + PTP_HDR_LEN, precise / 1000000000ULL);
    put32(fu + PTP_HDR_LEN + 6, (uint32_t)(precise % 1000000000ULL));
    sendto(gen, fu, sizeof(fu), 0, (struct sockaddr*)&a, sizeof(a));
    sync_seq++;
    pairs++;

    if (pairs % (hz * 2) == 0) { /* Announce ~every 2s */
      memset(ann, 0, sizeof(ann));
      hdr(ann, 0xB, (uint8_t)domain, ann_seq++, id, 0x05, -2);
      put48(ann + PTP_HDR_LEN, precise / 1000000000ULL);
      put32(ann + PTP_HDR_LEN + 6, (uint32_t)(precise % 1000000000ULL));
      memcpy(ann + PTP_HDR_LEN + 20, id, 8); /* grandmasterIdentity */
      sendto(gen, ann, sizeof(ann), 0, (struct sockaddr*)&a, sizeof(a));
    }
    if (pairs % (hz * 10) == 0)
      printf("master: %ld pairs sent\n", pairs);
    nanosleep(&gap, NULL);
  }
  printf("master: stopped after %ld pairs\n", pairs);
  return 0;
}
