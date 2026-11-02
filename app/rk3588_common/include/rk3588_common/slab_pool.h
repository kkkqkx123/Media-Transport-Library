/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 *
 * Pre-allocated slab pool for variable-length coded data.
 * Avoids malloc/free on the hot path (TX Fill Thread).
 */

#ifndef _E2E_SLAB_POOL_H_
#define _E2E_SLAB_POOL_H_

#include <stddef.h>
#include <stdint.h>

struct slab_pool;

/**
 * Create a slab pool.
 * @param n_blks    number of pre-allocated blocks
 * @param blk_size  size of each block in bytes
 */
struct slab_pool* slab_pool_create(int n_blks, size_t blk_size);
void slab_pool_free(struct slab_pool* p);
size_t slab_blk_size(struct slab_pool* p);

/**
 * Allocate a block. Blocks until one is available (pthread_cond_wait).
 * Returns NULL only on pool destruction (slab_pool_stop was called).
 */
uint8_t* slab_alloc(struct slab_pool* p);

/**
 * Allocate payload region with SEI_TS_NAL_SIZE bytes reserved at block head.
 * Caller writes SEI at (payload - SEI_TS_NAL_SIZE).
 */
uint8_t* slab_alloc_payload(struct slab_pool* p, size_t sei_reserve);

/** Free block given payload pointer from slab_alloc_payload */
void slab_free_payload(struct slab_pool* p, uint8_t* payload, size_t sei_reserve);

/** Return a block to the pool */
void slab_free(struct slab_pool* p, uint8_t* blk);

/** Wake all waiters and prevent further allocations (for shutdown) */
void slab_pool_stop(struct slab_pool* p);

#endif /* _E2E_SLAB_POOL_H_ */
