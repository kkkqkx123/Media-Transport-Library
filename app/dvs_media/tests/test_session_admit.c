/* Unit test for the session admission table (whitelist + LRU).
 * Self-contained (no MTL/MPP), runnable on x86. */

#include "engine/session_admit.h"

#include <assert.h>
#include <stdio.h>
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

int main(void) {
    test_whitelist();
    test_admit_and_lru();
    if (g_fail) {
        printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("session_admit: all checks passed\n");
    return 0;
}
