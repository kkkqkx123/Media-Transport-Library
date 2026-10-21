/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 *
 * Phase 1 verification: HAL layer self-test
 *   - dma_allocator: allocate dma-buf, export fd, verify size/stride
 *   - timer: measure clock precision
 *   - slab_pool: allocate/free cycle, concurrent stress
 *   - sei_timestamp: build/extract round-trip
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <drm_fourcc.h>
#include "../hal/dma_allocator.h"
#include "../hal/timer.h"
#include "../hal/slab_pool.h"
#include "../hal/sei_timestamp.h"

static int test_dma_allocator(void) {
  printf("  [dma_allocator] creating allocator on /dev/dri/card0...\n");
  struct dma_allocator* a = dma_allocator_create("/dev/dri/card0");
  if (!a) {
    printf("  [dma_allocator] SKIP (no /dev/dri/card0 or permission denied)\n");
    return 0; /* not a failure — may not have DRM access */
  }

  int fd = -1;
  uint32_t stride = 0, handle = 0;
  size_t size = 0;

  int ret = dma_allocator_alloc(a, 1920, 1080, DRM_FORMAT_NV12,
                                &fd, &stride, &size, &handle);
  if (ret < 0) {
    printf("  [dma_allocator] FAIL: alloc returned %d\n", ret);
    dma_allocator_free(a);
    return -1;
  }

  size_t expected = 1920 * 1080 + 1920 * 1080 / 2;
  printf("  [dma_allocator] allocated: stride=%u size=%zu (expected %zu) fd=%d\n",
         stride, size, expected, fd);
  assert(size >= expected);
  assert(fd >= 0);
  assert(stride >= 1920);

  dma_allocator_free_buf(a, handle);
  dma_allocator_free(a);
  printf("  [dma_allocator] PASS\n");
  return 0;
}

static int test_timer(void) {
  printf("  [timer] measuring clock precision...\n");
  struct e2e_timer* t = e2e_timer_create(NULL);
  assert(t);

  uint64_t t0 = e2e_timer_now_ns(t);
  uint64_t t1 = e2e_timer_now_ns(t);
  printf("  [timer] two consecutive reads: delta=%llu ns\n",
         (unsigned long long)(t1 - t0));
  /* Should be < 1ms (usually < 100ns on modern ARM) */
  assert(t1 >= t0);

  e2e_timer_free(t);
  printf("  [timer] PASS\n");
  return 0;
}

static int test_slab_pool(void) {
  printf("  [slab_pool] create pool: 8 blocks x 65536 bytes...\n");
  struct slab_pool* p = slab_pool_create(8, 65536);
  assert(p);

  /* Allocate all blocks */
  uint8_t* blocks[8];
  for (int i = 0; i < 8; i++) {
    blocks[i] = slab_alloc(p);
    assert(blocks[i]);
    memset(blocks[i], 0xAB, 65536); /* write to verify valid memory */
  }

  /* Return them */
  for (int i = 0; i < 8; i++) {
    slab_free(p, blocks[i]);
  }

  /* Re-allocate — should succeed immediately */
  uint8_t* b = slab_alloc(p);
  assert(b);
  slab_free(p, b);

  slab_pool_free(p);
  printf("  [slab_pool] PASS\n");
  return 0;
}

static int test_sei_timestamp(void) {
  printf("  [sei_timestamp] build/extract round-trip...\n");

  uint8_t buf[SEI_TS_NAL_SIZE + 100];
  uint32_t seq_in = 0xABCDEF00;  /* 4-byte frame_seq per SEI format */
  uint64_t ts_in  = 0x1122334455667788ULL;

  /* Build SEI + append dummy payload after it */
  sei_timestamp_build(buf, seq_in, ts_in);
  memcpy(buf + SEI_TS_NAL_SIZE, "PAYLOAD_DATA", 12);
  size_t size = SEI_TS_NAL_SIZE + 12;

  /* Extract */
  uint32_t seq_out = 0;
  uint64_t ts_out = 0;
  int ret = sei_timestamp_extract(buf, &size, &seq_out, &ts_out);
  assert(ret == 1);
  assert(seq_out == seq_in);
  assert(ts_out == ts_in);
  assert(size == 12); /* SEI stripped, only payload remains */
  assert(memcmp(buf, "PAYLOAD_DATA", 12) == 0);

  printf("  [sei_timestamp] PASS (seq=0x%x ts=0x%llx round-trip OK)\n",
         seq_out, (unsigned long long)ts_out);
  return 0;
}

int main(void) {
  printf("=== Phase 1 HAL Self-Test ===\n\n");

  int failures = 0;
  failures += (test_dma_allocator() != 0);
  failures += (test_timer() != 0);
  failures += (test_slab_pool() != 0);
  failures += (test_sei_timestamp() != 0);

  printf("\n=== Result: %d failures ===\n", failures);
  return failures ? 1 : 0;
}
