/* Unit test for the session admission table (whitelist + LRU).
 * Self-contained (no MTL/MPP), runnable on x86. */

#include "engine/session_admit.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__);     \
            g_fail++;                                            \
        }                                                        \
    } while (0)

static void test_whitelist(void) {
    /* exact ip:port and bare ip (any port) */
    dvs_session_whitelist_set("239.0.0.1:5000, 239.0.0.2");
    CHECK(dvs_session_whitelist_check("239.0.0.1", 5000) == 1);
    CHECK(dvs_session_whitelist_check("239.0.0.1", 6000) == 0);
    CHECK(dvs_session_whitelist_check("239.0.0.2", 9999) == 1);
    CHECK(dvs_session_whitelist_check("239.0.0.3", 5000) == 0);

    /* empty => allow all */
    dvs_session_whitelist_set(NULL);
    CHECK(dvs_session_whitelist_check("10.0.0.1", 1234) == 1);
    dvs_session_whitelist_set("");
    CHECK(dvs_session_whitelist_check("10.0.0.1", 1234) == 1);
}

static void test_admit_and_lru(void) {
    dvs_session_whitelist_set(NULL); /* allow all */

    int lane = -3;
    CHECK(dvs_session_admit("239.1.0.1", 5000, &lane) == 0);
    CHECK(lane >= 0 && lane < DVS_MAX_SESSIONS);
    int first_lane = lane;
    CHECK(dvs_session_admit("239.1.0.2", 5000, &lane) == 0);
    CHECK(lane != first_lane);

    /* re-admit same source => same lane, count unchanged */
    int before = dvs_session_count();
    CHECK(dvs_session_admit("239.1.0.1", 5000, &lane) == 0);
    CHECK(lane == first_lane);
    CHECK(dvs_session_count() == before);

    /* fill the table to capacity with distinct sources */
    for (int i = 0; i < DVS_MAX_SESSIONS; i++) {
        char ip[32];
        snprintf(ip, sizeof(ip), "239.2.0.%d", i);
        CHECK(dvs_session_admit(ip, 6000, &lane) == 0);
    }
    CHECK(dvs_session_count() == DVS_MAX_SESSIONS);

    /* one more distinct source must evict LRU, count stays capped */
    CHECK(dvs_session_admit("239.3.0.99", 7000, &lane) == 0);
    CHECK(dvs_session_count() == DVS_MAX_SESSIONS);

    /* release reduces count */
    dvs_session_release("239.3.0.99", 7000);
    CHECK(dvs_session_count() == DVS_MAX_SESSIONS - 1);
}

/* Batch reservation (P2-6): all-or-nothing admission at prepare time. */
static void test_reserve_batch(void) {
    dvs_session_whitelist_set(NULL); /* allow all */
    /* drain any leftovers from the previous test */
    {
        char keys[DVS_MAX_SESSIONS][DVS_SESSION_KEY_LEN];
        int lanes[DVS_MAX_SESSIONS];
        int n = dvs_session_snapshot(keys, lanes, DVS_MAX_SESSIONS);
        for (int i = 0; i < n; i++) {
            char* colon = strrchr(keys[i], ':');
            if (!colon) continue;
            *colon = '\0';
            dvs_session_release(keys[i], (uint16_t)atoi(colon + 1));
        }
    }
    CHECK(dvs_session_count() == 0);

    /* empty batch is a no-op success */
    CHECK(dvs_session_reserve_batch(NULL, NULL, 0, NULL) == 0);
    CHECK(dvs_session_count() == 0);

    /* happy path: two sources admitted, lanes reported */
    char ips[2][DVS_SESSION_KEY_LEN];
    snprintf(ips[0], sizeof(ips[0]), "239.10.0.1");
    snprintf(ips[1], sizeof(ips[1]), "239.10.0.2");
    uint16_t ports[2] = {5000, 5000};
    int lanes[2] = {-1, -1};
    CHECK(dvs_session_reserve_batch(ips, ports, 2, lanes) == 0);
    CHECK(lanes[0] >= 0 && lanes[1] >= 0 && lanes[0] != lanes[1]);
    CHECK(dvs_session_count() == 2);

    /* re-reserve is idempotent: same lanes, count unchanged */
    int again[2] = {-1, -1};
    CHECK(dvs_session_reserve_batch(ips, ports, 2, again) == 0);
    CHECK(again[0] == lanes[0] && again[1] == lanes[1]);
    CHECK(dvs_session_count() == 2);

    /* one rejected entry rejects the whole batch and admits nothing */
    dvs_session_whitelist_set("239.10.0.1:5000");
    char bad[2][DVS_SESSION_KEY_LEN];
    snprintf(bad[0], sizeof(bad[0]), "239.10.0.1");
    snprintf(bad[1], sizeof(bad[1]), "239.10.0.9");
    uint16_t bp[2] = {5000, 5000};
    int before = dvs_session_count();
    CHECK(dvs_session_reserve_batch(bad, bp, 2, NULL) == -1);
    CHECK(dvs_session_count() == before);

    /* bad inputs never touch the table */
    CHECK(dvs_session_reserve_batch(NULL, ports, 1, NULL) == -1);
    CHECK(dvs_session_reserve_batch(ips, NULL, 1, NULL) == -1);
    CHECK(dvs_session_reserve_batch(ips, ports, -1, NULL) == -1);
    CHECK(dvs_session_reserve_batch(ips, ports, DVS_MAX_SESSIONS + 1, NULL) == -1);
    CHECK(dvs_session_count() == before);
    char empty_ip[1][DVS_SESSION_KEY_LEN];
    empty_ip[0][0] = '\0';
    uint16_t ep[1] = {5000};
    CHECK(dvs_session_reserve_batch(empty_ip, ep, 1, NULL) == -1);
    CHECK(dvs_session_count() == before);

    dvs_session_whitelist_set(NULL);
}

int main(void) {
    test_whitelist();
    test_admit_and_lru();
    test_reserve_batch();
    if (g_fail) {
        printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("session_admit: all checks passed\n");
    return 0;
}
