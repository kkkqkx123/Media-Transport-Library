/* Unit test for the hardware-free capability logic in engine/capability.c:
 * backend naming, PTP state math and the decode-session ceiling. */
#include "../engine/capability.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (cond) {                                                           \
            printf("PASS: %s\n", name);                                       \
        } else {                                                              \
            printf("FAIL: %s\n", name);                                       \
            failures++;                                                       \
        }                                                                     \
    } while (0)

int main(void) {
    /* backend naming is shared with the IPC bridge */
    CHECK(strcmp(dvs_capability_backend_name("dpdk:0000:af:00.0"), "dpdk") == 0,
          "backend names a dpdk port");
    CHECK(strcmp(dvs_capability_backend_name("kernel:eth0"), "kernel") == 0,
          "backend names a kernel port");
    CHECK(strcmp(dvs_capability_backend_name("native_af_xdp:eth0"), "af_xdp") == 0,
          "backend names a native af_xdp port");
    CHECK(strcmp(dvs_capability_backend_name("dpdk_af_xdp:eth0"), "af_xdp") == 0,
          "backend names a dpdk af_xdp port");
    CHECK(strcmp(dvs_capability_backend_name(NULL), "unknown") == 0,
          "backend names NULL unknown");
    CHECK(strcmp(dvs_capability_backend_name(""), "unknown") == 0,
          "backend names empty unknown");

    /* PTP state math */
    {
        char state[32] = {0};
        int off = -1;
        dvs_ptp_state_from_delta(state, sizeof(state), &off, 0, 0);
        CHECK(strcmp(state, "unknown") == 0 && off == 0,
              "ptp reports unknown without a valid TAI read");
    }
    {
        char state[32] = {0};
        int off = 0;
        dvs_ptp_state_from_delta(state, sizeof(state), &off, 1, 2500000LL);
        CHECK(strcmp(state, "synced") == 0 && off == 2500,
              "ptp converts a TAI delta to microsecond offset");
    }
    {
        char state[32] = {0};
        int off = 0;
        dvs_ptp_state_from_delta(state, sizeof(state), &off, 1, -1200000LL);
        CHECK(strcmp(state, "synced") == 0 && off == -1200,
              "ptp keeps the sign of a negative delta");
    }

    /* decode ceiling: plane/lane/mpp minimum with env override last */
    dvs_capability_set_mpp_ceiling(0); /* restore default */
    {
        dvs_capability cap;
        memset(&cap, 0, sizeof(cap));
        cap.max_layers = 16;
        CHECK(dvs_capability_static_ceiling(&cap) == 4,
              "ceiling defaults to the mpp value under many planes");
    }
    dvs_capability_set_mpp_ceiling(8);
    {
        dvs_capability cap;
        memset(&cap, 0, sizeof(cap));
        cap.max_layers = 16;
        CHECK(dvs_capability_static_ceiling(&cap) == 4,
              "ceiling still honors the compositor lane limit");
    }
    dvs_capability_set_mpp_ceiling(2);
    {
        dvs_capability cap;
        memset(&cap, 0, sizeof(cap));
        cap.max_layers = 16;
        CHECK(dvs_capability_static_ceiling(&cap) == 2,
              "ceiling follows an injected mpp limit");
    }
    dvs_capability_set_mpp_ceiling(0); /* restore default for other tests */

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
