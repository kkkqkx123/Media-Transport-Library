/* RX tracking logic unit test: x86-runnable, no hardware deps. */
#include "../hal/sei_timestamp.h"
#include "../pipeline/rx_track.h"
#include <stdio.h>

static int g_fails = 0;

#define CHECK(cond, ...)                                   \
  do {                                                     \
    if (!(cond)) {                                         \
      printf("FAIL: " __VA_ARGS__);                        \
      printf("\n");                                        \
      g_fails++;                                         \
    }                                                      \
  } while (0)

static void test_seq_basic(void) {
  printf("  [seq] contiguous + gap + dup...\n");
  struct rx_seq_tracker t;
  rx_seq_reset(&t);
  rx_seq_observe(&t, 10);
  rx_seq_observe(&t, 11);
  rx_seq_observe(&t, 12);
  CHECK(t.gap_events == 0 && t.lost == 0, "contiguous must not gap");
  rx_seq_observe(&t, 15); /* 13,14 missing */
  CHECK(t.gap_events == 1 && t.lost == 2, "gap ev=1 lost=2, got %llu/%llu",
        (unsigned long long)t.gap_events, (unsigned long long)t.lost);
  rx_seq_observe(&t, 15); /* duplicate */
  CHECK(t.dups == 1, "dup must count");
  rx_seq_observe(&t, 16);
  CHECK(t.gap_events == 1 && t.lost == 2, "resume must not add gap");
}

static void test_seq_wrap(void) {
  printf("  [seq] 32-bit wrap...\n");
  struct rx_seq_tracker t;
  rx_seq_reset(&t);
  rx_seq_observe(&t, 0xFFFFFFFEu);
  rx_seq_observe(&t, 0xFFFFFFFFu);
  CHECK(t.gap_events == 0, "pre-wrap contiguous");
  rx_seq_observe(&t, 0u);
  CHECK(t.gap_events == 0 && t.lost == 0, "wrap 0xFFFFFFFF->0 is contiguous");
  rx_seq_observe(&t, 2u); /* 1 missing across wrap */
  CHECK(t.gap_events == 1 && t.lost == 1, "wrapped gap ev=1 lost=1");
}

static void test_seq_reorder(void) {
  printf("  [seq] late/reordered frame...\n");
  struct rx_seq_tracker t;
  rx_seq_reset(&t);
  rx_seq_observe(&t, 100);
  rx_seq_observe(&t, 50); /* far behind: late, not a 2^32-50 gap */
  CHECK(t.gap_events == 0 && t.dups == 1, "late frame counts as dup");
  CHECK(t.last == 100, "baseline must not move backwards");
}

static void test_lat_valid(void) {
  printf("  [lat] validity window...\n");
  uint64_t d = 0;
  CHECK(rx_lat_valid(100, 200, &d) && d == 100, "100ns delta valid");
  CHECK(!rx_lat_valid(0, 200, NULL), "zero cap_ts invalid");
  CHECK(!rx_lat_valid(300, 200, NULL), "now<cap invalid (clock step)");
  CHECK(!rx_lat_valid(1, 1 + RX_LAT_MAX_NS + 1, NULL), ">1s delta invalid");
  CHECK(rx_lat_valid(1, 1 + RX_LAT_MAX_NS, &d) && d == RX_LAT_MAX_NS,
        "exactly 1s delta valid");
}

static void test_sei_peek_offset(void) {
  printf("  [sei] peek + payload offset...\n");
  uint8_t buf[SEI_TS_NAL_SIZE + 16];
  uint32_t seq_in = 1234567, seq_out = 0;
  uint64_t ts_in = 987654321012345ULL, ts_out = 0;
  sei_timestamp_build(buf, seq_in, ts_in);
  for (int i = 0; i < 16; i++) buf[SEI_TS_NAL_SIZE + i] = (uint8_t)i;
  int found = sei_timestamp_peek(buf, sizeof(buf), &seq_out, &ts_out);
  CHECK(found == 1, "peek must find SEI");
  CHECK(seq_out == seq_in && ts_out == ts_in, "peek seq/ts round-trip");
  CHECK(rx_sei_payload_offset(found, SEI_TS_NAL_SIZE) == SEI_TS_NAL_SIZE,
        "offset skips SEI");
  CHECK(sizeof(buf) - rx_sei_payload_offset(found, SEI_TS_NAL_SIZE) == 16,
        "payload size excludes SEI");
  /* garbage prefix: no SEI, offset 0 */
  uint8_t junk[32] = {0x00, 0x00, 0x01, 0x65};
  int miss = sei_timestamp_peek(junk, sizeof(junk), &seq_out, &ts_out);
  CHECK(miss == 0, "peek must miss garbage");
  CHECK(rx_sei_payload_offset(miss, SEI_TS_NAL_SIZE) == 0, "offset 0 on miss");
}

int main(void) {
  printf("=== test_rx_track ===\n");
  test_seq_basic();
  test_seq_wrap();
  test_seq_reorder();
  test_lat_valid();
  test_sei_peek_offset();
  if (g_fails) {
    printf("test_rx_track: %d FAILURES\n", g_fails);
    return 1;
  }
  printf("test_rx_track: all passed\n");
  return 0;
}
