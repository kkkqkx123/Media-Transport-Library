/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 *
 * Lock-free SPSC (Single Producer, Single Consumer) ring buffer.
 *
 * push/pop remain non-blocking. pop_wait blocks the consumer on empty
 * using pthread_cond (replaces usleep polling).
 */

#ifndef _E2E_RING_BUFFER_H_
#define _E2E_RING_BUFFER_H_

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

struct ring_buffer {
    void**        buf;
    int           size;
    atomic_int    head;
    atomic_int    tail;
    atomic_int    count;

    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
};

struct ring_buffer* ring_create(int size);
void ring_free(struct ring_buffer* r);

int ring_push(struct ring_buffer* r, void* item);
int ring_pop(struct ring_buffer* r, void** item);

/**
 * Block until an item is available or *running becomes false.
 * @return 0 on success, -EAGAIN if empty and not running.
 */
int ring_pop_wait(struct ring_buffer* r, void** item, const atomic_bool* running);

/** Wake all waiters (call before join on shutdown). */
void ring_wake_all(struct ring_buffer* r);

#endif /* _E2E_RING_BUFFER_H_ */
