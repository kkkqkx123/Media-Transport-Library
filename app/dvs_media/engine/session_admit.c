#include "session_admit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char key[DVS_SESSION_KEY_LEN]; /* "ip:port" */
    int lane;
    int last_used;                 /* LRU tick; -1 = free */
} session_slot_t;

static session_slot_t g_slots[DVS_MAX_SESSIONS];
static int g_tick = 0;
static int g_inited = 0;

/* whitelist: array of "ip" or "ip:port" entries */
static char g_wl_ip[DVS_MAX_SESSIONS][64];
static uint16_t g_wl_port[DVS_MAX_SESSIONS]; /* 0 = any port */
static int g_wl_n = 0;
static int g_wl_enabled = 0;

/* Static storage is zero-initialized, where last_used==0 would look "active".
 * Mark every slot free (-1) exactly once. */
static void ensure_init(void) {
    if (g_inited) return;
    for (int i = 0; i < DVS_MAX_SESSIONS; i++) {
        g_slots[i].last_used = -1;
        g_slots[i].key[0] = '\0';
        g_slots[i].lane = i;
    }
    g_inited = 1;
}

static void make_key(char* out, const char* ip, uint16_t port) {
    snprintf(out, DVS_SESSION_KEY_LEN, "%s:%u", ip, (unsigned)port);
}

static int valid_ip4_str(const char* s) {
    if (!s || !s[0]) return 0;
    int parts = 0;
    const char* p = s;
    while (1) {
        if (*p < '0' || *p > '9') return 0;
        int v = 0, digits = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); digits++; p++; if (digits > 3) return 0; }
        if (v > 255) return 0;
        parts++;
        if (*p == '\0') break;
        if (*p != '.') return 0;
        p++;
        if (*p == '\0') return 0;
    }
    return parts == 4;
}

void dvs_session_whitelist_set(const char* csv) {
    ensure_init();
    g_wl_n = 0;
    g_wl_enabled = 0;
    if (!csv || !csv[0]) return;

    char buf[512];
    snprintf(buf, sizeof(buf), "%s", csv);

    char* save = NULL;
    for (char* tok = strtok_r(buf, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ') tok++; /* trim leading spaces */
        if (!*tok) continue;
        char* end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
        *end = '\0';
        if (!*tok) continue;
        char* colon = strchr(tok, ':');
        uint16_t p = 0;
        if (colon) {
            if (strchr(colon + 1, ':')) continue; /* skip malformed */
            *colon = '\0';
            const char* ps = colon + 1;
            if (!ps[0]) continue;
            int ok = 1;
            for (const char* q = ps; *q; q++) if (*q < '0' || *q > '9') ok = 0;
            if (!ok) continue;
            long v = atol(ps);
            if (v < 1 || v > 65535) continue;
            p = (uint16_t)v;
        }
        if (!valid_ip4_str(tok)) continue; /* WL-1: drop malformed entries */
        if (g_wl_n < DVS_MAX_SESSIONS) {
            snprintf(g_wl_ip[g_wl_n], sizeof(g_wl_ip[g_wl_n]), "%s", tok);
            g_wl_port[g_wl_n] = p;
            g_wl_n++;
        }
    }
    if (g_wl_n > 0) g_wl_enabled = 1;
}

int dvs_session_whitelist_check(const char* mcast_ip, uint16_t port) {
    ensure_init();
    if (!g_wl_enabled) return 1; /* allow all when disabled */
    for (int i = 0; i < g_wl_n; i++) {
        if (strcmp(g_wl_ip[i], mcast_ip) != 0) continue;
        if (g_wl_port[i] == 0 || g_wl_port[i] == port) return 1;
    }
    return 0;
}

int dvs_session_admit(const char* mcast_ip, uint16_t port, int* lane_out) {
    ensure_init();
    if (!dvs_session_whitelist_check(mcast_ip, port)) return -1;

    char key[DVS_SESSION_KEY_LEN];
    make_key(key, mcast_ip, port);

    g_tick++; /* monotonic: makes LRU ordering well-defined within a batch */

    /* hit: refresh LRU, reuse lane */
    for (int i = 0; i < DVS_MAX_SESSIONS; i++) {
        if (g_slots[i].last_used >= 0 && strcmp(g_slots[i].key, key) == 0) {
            g_slots[i].last_used = g_tick;
            *lane_out = g_slots[i].lane;
            return 0;
        }
    }

    /* free slot */
    for (int i = 0; i < DVS_MAX_SESSIONS; i++) {
        if (g_slots[i].last_used < 0) {
            snprintf(g_slots[i].key, sizeof(g_slots[i].key), "%s", key);
            g_slots[i].lane = i;
            g_slots[i].last_used = g_tick;
            *lane_out = i;
            return 0;
        }
    }

    /* full: evict LRU (smallest last_used) */
    int lru = 0;
    for (int i = 1; i < DVS_MAX_SESSIONS; i++)
        if (g_slots[i].last_used < g_slots[lru].last_used) lru = i;
    snprintf(g_slots[lru].key, sizeof(g_slots[lru].key), "%s", key);
    g_slots[lru].lane = lru;
    g_slots[lru].last_used = g_tick;
    *lane_out = lru;
    return 0;
}

void dvs_session_release(const char* mcast_ip, uint16_t port) {
    ensure_init();
    char key[DVS_SESSION_KEY_LEN];
    make_key(key, mcast_ip, port);
    for (int i = 0; i < DVS_MAX_SESSIONS; i++) {
        if (g_slots[i].last_used >= 0 && strcmp(g_slots[i].key, key) == 0) {
            g_slots[i].last_used = -1;
            g_slots[i].key[0] = '\0';
            return;
        }
    }
}

void dvs_session_tick(void) {
    ensure_init();
    /* Admission advances its own clock; this hook is kept for API symmetry. */
}

int dvs_session_count(void) {
    ensure_init();
    int n = 0;
    for (int i = 0; i < DVS_MAX_SESSIONS; i++)
        if (g_slots[i].last_used >= 0) n++;
    return n;
}

int dvs_session_snapshot(char keys[][DVS_SESSION_KEY_LEN], int* lanes, int max) {
    ensure_init();
    if (!keys || !lanes || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < DVS_MAX_SESSIONS && n < max; i++) {
        if (g_slots[i].last_used < 0) continue;
        snprintf(keys[n], DVS_SESSION_KEY_LEN, "%s", g_slots[i].key);
        lanes[n] = g_slots[i].lane;
        n++;
    }
    return n;
}
