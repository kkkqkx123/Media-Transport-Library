/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include "slab_pool.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct slab_pool {
  uint8_t**    blocks;
  atomic_int*  in_use;   /* 0 = free, 1 = allocated */
  int          n_blks;
  size_t       blk_size;

  atomic_int   stopped;

  pthread_mutex_t mutex;
  pthread_cond_t  cond;
};

struct slab_pool* slab_pool_create(int n_blks, size_t blk_size) {
  struct slab_pool* p = calloc(1, sizeof(*p));
  if (!p) return NULL;

  p->n_blks   = n_blks;
  p->blk_size = blk_size;

  p->blocks = calloc((size_t)n_blks, sizeof(uint8_t*));
  p->in_use = calloc((size_t)n_blks, sizeof(atomic_int));
  if (!p->blocks || !p->in_use) goto fail;

  for (int i = 0; i < n_blks; i++) {
    p->blocks[i] = calloc(1, blk_size);
    if (!p->blocks[i]) goto fail;
    atomic_init(&p->in_use[i], 0);
  }

  atomic_init(&p->stopped, 0);
  pthread_mutex_init(&p->mutex, NULL);
  pthread_cond_init(&p->cond, NULL);
  return p;

fail:
  if (p->blocks) {
    for (int i = 0; i < n_blks; i++)
      free(p->blocks[i]);
  }
  free(p->blocks);
  free(p->in_use);
  free(p);
  return NULL;
}

void slab_pool_free(struct slab_pool* p) {
  if (!p) return;
  slab_pool_stop(p);
  for (int i = 0; i < p->n_blks; i++) free(p->blocks[i]);
  free(p->blocks);
  free(p->in_use);
  pthread_mutex_destroy(&p->mutex);
  pthread_cond_destroy(&p->cond);
  free(p);
}

size_t slab_blk_size(struct slab_pool* p) {
  return p ? p->blk_size : 0;
}

void slab_pool_stop(struct slab_pool* p) {
  if (!p) return;
  atomic_store(&p->stopped, 1);
  pthread_mutex_lock(&p->mutex);
  pthread_cond_broadcast(&p->cond);
  pthread_mutex_unlock(&p->mutex);
}

uint8_t* slab_alloc(struct slab_pool* p) {
  while (1) {
    if (atomic_load(&p->stopped)) return NULL;

    for (int i = 0; i < p->n_blks; i++) {
      int expected = 0;
      if (atomic_compare_exchange_weak(&p->in_use[i], &expected, 1))
        return p->blocks[i];
    }

    pthread_mutex_lock(&p->mutex);
    if (!atomic_load(&p->stopped)) {
      int busy = 1;
      for (int i = 0; i < p->n_blks; i++) {
        if (atomic_load(&p->in_use[i]) == 0) {
          busy = 0;
          break;
        }
      }
      if (busy) pthread_cond_wait(&p->cond, &p->mutex);
    }
    pthread_mutex_unlock(&p->mutex);
  }
}

uint8_t* slab_alloc_payload(struct slab_pool* p, size_t sei_reserve) {
  uint8_t* blk = slab_alloc(p);
  if (!blk) return NULL;
  return blk + sei_reserve;
}

void slab_free_payload(struct slab_pool* p, uint8_t* payload, size_t sei_reserve) {
  if (!payload) return;
  slab_free(p, payload - sei_reserve);
}

void slab_free(struct slab_pool* p, uint8_t* blk) {
  if (!p || !blk) return;

  for (int i = 0; i < p->n_blks; i++) {
    if (p->blocks[i] != blk) continue;
    if (atomic_exchange(&p->in_use[i], 0) == 1) {
      pthread_mutex_lock(&p->mutex);
      pthread_cond_signal(&p->cond);
      pthread_mutex_unlock(&p->mutex);
    }
    return;
  }
}
