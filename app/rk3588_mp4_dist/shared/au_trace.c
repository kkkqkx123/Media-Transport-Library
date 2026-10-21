/* SPDX-License-Identifier: BSD-3-Clause */

#include "au_trace.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static FILE* g_files[4];
static uint64_t g_seq[4];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_announced;
static char g_base[512];

static uint32_t au_crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xedb88320u & (~(crc & 1u) + 1u));
    }
    return crc ^ 0xffffffffu;
}

static int au_first_nal(const uint8_t* data, size_t size) {
    for (size_t i = 0; i + 4 < size; i++) {
        if (data[i] == 0 && data[i + 1] == 0 &&
            (data[i + 2] == 1 || (data[i + 2] == 0 && data[i + 3] == 1))) {
            size_t off = (data[i + 2] == 1) ? (i + 3) : (i + 4);
            if (off < size) return data[off] & 0x1f;
        }
    }
    return -1;
}

static int au_has_nal(const uint8_t* data, size_t size, int type) {
    for (size_t i = 0; i + 4 < size; i++) {
        if (data[i] == 0 && data[i + 1] == 0 &&
            (data[i + 2] == 1 || (data[i + 2] == 0 && data[i + 3] == 1))) {
            size_t off = (data[i + 2] == 1) ? (i + 3) : (i + 4);
            if (off < size && (int)(data[off] & 0x1f) == type) return 1;
        }
    }
    return 0;
}

static FILE* au_open_stage(enum au_trace_stage stage) {
    if (stage < 0 || stage > AU_TRACE_RX_BRIDGE) return NULL;
    if (g_files[stage]) return g_files[stage];

    const char* base = getenv("MP4_DIST_AU_TRACE");
    if (!base || !base[0]) return NULL;

    if (!g_base[0]) snprintf(g_base, sizeof(g_base), "%s", base);

    static const char* suffix[] = { ".tx.csv", ".tx_wire.csv", ".rx_mtl.csv",
                                    ".rx_br.csv" };
    char path[640];
    snprintf(path, sizeof(path), "%s%s", g_base, suffix[stage]);

    /* Lazy open: each fork child only touches its own stage files (both mode). */
    FILE* fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "au_trace: cannot open %s\n", path);
        return NULL;
    }
    fprintf(fp, "seq,stage,size,crc32,nal0,idr,sps,extra\n");
    fflush(fp);
    g_files[stage] = fp;

    if (!g_announced) {
        g_announced = 1;
        printf("au_trace: enabled base=%s (per-stage lazy open)\n", g_base);
    }
    return fp;
}

static const char* au_stage_name(enum au_trace_stage stage) {
    switch (stage) {
    case AU_TRACE_TX_PUBLISH: return "tx_pub";
    case AU_TRACE_TX_WIRE:    return "tx_wire";
    case AU_TRACE_RX_MTL:     return "rx_mtl";
    case AU_TRACE_RX_BRIDGE:  return "rx_br";
    default:                  return "?";
    }
}

void au_trace_record(enum au_trace_stage stage, const uint8_t* data, size_t size,
                     int64_t extra) {
    if (!data || !size || stage < 0 || stage > AU_TRACE_RX_BRIDGE) return;

    FILE* fp = au_open_stage(stage);
    if (!fp) return;

    uint32_t crc = au_crc32(data, size);
    int nal0 = au_first_nal(data, size);
    int idr  = au_has_nal(data, size, 5);
    int sps  = au_has_nal(data, size, 7);

    pthread_mutex_lock(&g_lock);
    uint64_t seq = ++g_seq[stage];
    fprintf(fp, "%llu,%s,%zu,%08x,%d,%d,%d,%lld\n",
            (unsigned long long)seq, au_stage_name(stage), size, crc,
            nal0, idr, sps, (long long)extra);
    fflush(fp);
    pthread_mutex_unlock(&g_lock);
}

void au_trace_close(void) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < 4; i++) {
        if (g_files[i]) {
            fclose(g_files[i]);
            g_files[i] = NULL;
        }
    }
    pthread_mutex_unlock(&g_lock);
}
