/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include "ring_buffer.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

struct ring_buffer* ring_create(int size) {
    struct ring_buffer* r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->buf = calloc((size_t)size, sizeof(void*));
    if (!r->buf) { free(r); return NULL; }
    r->size = size;
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    atomic_init(&r->count, 0);
    pthread_mutex_init(&r->mutex, NULL);
    pthread_cond_init(&r->not_empty, NULL);
    return r;
}

void ring_wake_all(struct ring_buffer* r) {
    if (!r) return;
    pthread_mutex_lock(&r->mutex);
    pthread_cond_broadcast(&r->not_empty);
    pthread_mutex_unlock(&r->mutex);
}

void ring_free(struct ring_buffer* r) {
    if (!r) return;
    ring_wake_all(r);
    pthread_mutex_destroy(&r->mutex);
    pthread_cond_destroy(&r->not_empty);
    free(r->buf);
    free(r);
}

int ring_push(struct ring_buffer* r, void* item) {
    int count = atomic_load_explicit(&r->count, memory_order_acquire);
    if (count >= r->size)
        return -ENOSPC;

    int head = atomic_load_explicit(&r->head, memory_order_relaxed);
    r->buf[head] = item;
    atomic_store_explicit(&r->head, (head + 1) % r->size, memory_order_release);
    atomic_store_explicit(&r->count, count + 1, memory_order_release);

    pthread_mutex_lock(&r->mutex);
    pthread_cond_signal(&r->not_empty);
    pthread_mutex_unlock(&r->mutex);
    return 0;
}

int ring_pop(struct ring_buffer* r, void** item) {
    int count = atomic_load_explicit(&r->count, memory_order_acquire);
    if (count <= 0)
        return -EAGAIN;

    int tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    *item = r->buf[tail];
    r->buf[tail] = NULL;
    atomic_store_explicit(&r->tail, (tail + 1) % r->size, memory_order_release);
    atomic_store_explicit(&r->count, count - 1, memory_order_release);
    return 0;
}

int ring_pop_wait(struct ring_buffer* r, void** item, const atomic_bool* running) {
    if (!r || !item) return -EINVAL;

    for (;;) {
        if (ring_pop(r, item) == 0)
            return 0;
        if (!running || !atomic_load(running))
            return -EAGAIN;

        pthread_mutex_lock(&r->mutex);
        if (atomic_load_explicit(&r->count, memory_order_acquire) <= 0 &&
            atomic_load(running)) {
            pthread_cond_wait(&r->not_empty, &r->mutex);
        }
        pthread_mutex_unlock(&r->mutex);
    }
}

#ifdef RING_TEST
#include <stdio.h>
#include <stdint.h>

int main(void) {
    struct ring_buffer* r = ring_create(2);
    if (!r) { printf("FAIL: create\n"); return 1; }

    int ret;
    ret = ring_push(r, (void*)(intptr_t)1);
    printf("push 1: %d (expected 0)\n", ret);
    ret = ring_push(r, (void*)(intptr_t)2);
    printf("push 2: %d (expected 0)\n", ret);
    ret = ring_push(r, (void*)(intptr_t)3);
    printf("push 3: %d (expected -ENOSPC=%d)\n", ret, -ENOSPC);

    void* v = NULL;
    ret = ring_pop(r, &v);
    printf("pop 1: ret=%d val=%lld (expected 0,1)\n", ret, (long long)(intptr_t)v);
    ret = ring_pop(r, &v);
    printf("pop 2: ret=%d val=%lld (expected 0,2)\n", ret, (long long)(intptr_t)v);
    ret = ring_pop(r, &v);
    printf("pop 3: ret=%d (expected -EAGAIN=%d)\n", ret, -EAGAIN);

    ret = ring_push(r, (void*)(intptr_t)4);
    printf("push 4: %d (expected 0)\n", ret);
    ret = ring_pop(r, &v);
    printf("pop 4: ret=%d val=%lld (expected 0,4)\n", ret, (long long)(intptr_t)v);

    ring_free(r);
    printf("=== ring_buffer PASS ===\n");
    return 0;
}
#endif /* RING_TEST */
