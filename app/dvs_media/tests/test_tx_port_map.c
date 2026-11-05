/* Unit test for engine/tx_port_map.c (no hardware, no MTL/MPP). */
#include "../engine/tx_port_map.h"

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
    /* table build: gap numbers keep scan-order binding, not fixed indices */
    {
        char devs[2][DVS_TX_DEV_LEN] = {"/dev/video0", "/dev/video2"};
        char stables[2][DVS_TX_STABLE_LEN] = {"cardA|busA|in0",
                                              "cardB|busB|in0"};
        int sig[2] = {1, 1};
        dvs_tx_port_entry t[8];
        int n = dvs_tx_table_build(t, 8, devs, stables, sig, 2);
        CHECK(n == 2, "build two entries over gap");
        CHECK(strcmp(t[0].port, "HDMI1") == 0 &&
                  strcmp(t[0].device, "/dev/video0") == 0,
              "build HDMI1 binds video0");
        CHECK(strcmp(t[1].port, "HDMI2") == 0 &&
                  strcmp(t[1].device, "/dev/video2") == 0,
              "build HDMI2 binds video2 despite gap");
    }

    /* reorder: stable ID wins over position */
    {
        dvs_tx_port_entry table[2];
        char sdevs[2][DVS_TX_DEV_LEN] = {"/dev/video0", "/dev/video2"};
        char sst[2][DVS_TX_STABLE_LEN] = {"cardA|busA|in0", "cardB|busB|in0"};
        int ssig[2] = {1, 1};
        dvs_tx_table_build(table, 8, sdevs, sst, ssig, 2);
        char cur_devs[2][DVS_TX_DEV_LEN] = {"/dev/video2", "/dev/video0"};
        char cur_st[2][DVS_TX_STABLE_LEN] = {"cardB|busB|in0",
                                             "cardA|busA|in0"};
        int cur_sig[2] = {1, 1};
        char out[DVS_TX_DEV_LEN] = {0};
        char err[256] = {0};
        int rc = dvs_tx_resolve_device(table, 2, cur_devs, cur_st, cur_sig, 2,
                                       "HDMI1", out, sizeof(out), err,
                                       sizeof(err));
        CHECK(rc == 0 && strcmp(out, "/dev/video0") == 0,
              "resolve HDMI1 follows stable across reorder");
        rc = dvs_tx_resolve_device(table, 2, cur_devs, cur_st, cur_sig, 2,
                                   "HDMI2", out, sizeof(out), err,
                                   sizeof(err));
        CHECK(rc == 0 && strcmp(out, "/dev/video2") == 0,
              "resolve HDMI2 follows stable across reorder");
    }

    /* empty table: honest failure with available list, never a guess */
    {
        char out[DVS_TX_DEV_LEN] = {0};
        char err[256] = {0};
        int rc = dvs_tx_resolve_device(NULL, 0, NULL, NULL, NULL, 0, "HDMI1",
                                       out, sizeof(out), err, sizeof(err));
        CHECK(rc != 0, "resolve on empty table fails");
        CHECK(strstr(err, "no capture devices") != NULL,
              "empty error names the empty table");
        rc = dvs_tx_resolve_device(NULL, 0, NULL, NULL, NULL, 0, "", out,
                                   sizeof(out), err, sizeof(err));
        CHECK(rc != 0, "resolve empty port fails");
    }

    /* no-signal targets are rejected, never rebound */
    {
        dvs_tx_port_entry table[2];
        char sdevs[2][DVS_TX_DEV_LEN] = {"/dev/video0", "/dev/video2"};
        char sst[2][DVS_TX_STABLE_LEN] = {"cardA|busA|in0", "cardB|busB|in0"};
        int ssig[2] = {1, 1};
        dvs_tx_table_build(table, 8, sdevs, sst, ssig, 2);
        char cur_devs[2][DVS_TX_DEV_LEN] = {"/dev/video0", "/dev/video2"};
        char cur_st[2][DVS_TX_STABLE_LEN] = {"cardA|busA|in0",
                                             "cardB|busB|in0"};
        int cur_sig[2] = {1, 0};
        char out[DVS_TX_DEV_LEN] = {0};
        char err[256] = {0};
        int rc = dvs_tx_resolve_device(table, 2, cur_devs, cur_st, cur_sig, 2,
                                       "HDMI2", out, sizeof(out), err,
                                       sizeof(err));
        CHECK(rc != 0 && strstr(err, "no signal") != NULL,
              "no-signal HDMI2 rejected explicitly");
        rc = dvs_tx_resolve_device(table, 2, cur_devs, cur_st, cur_sig, 2,
                                   "HDMI1", out, sizeof(out), err,
                                   sizeof(err));
        CHECK(rc == 0 && strcmp(out, "/dev/video0") == 0,
              "healthy HDMI1 still resolves");
    }

    /* device path and stable ID direct forms */
    {
        dvs_tx_port_entry table[1];
        char sdevs[1][DVS_TX_DEV_LEN] = {"/dev/video0"};
        char sst[1][DVS_TX_STABLE_LEN] = {"cardA|busA|in0"};
        int ssig[1] = {1};
        dvs_tx_table_build(table, 8, sdevs, sst, ssig, 1);
        char out[DVS_TX_DEV_LEN] = {0};
        char err[256] = {0};
        CHECK(dvs_tx_resolve_device(table, 1, sdevs, sst, ssig, 1,
                                    "/dev/video0", out, sizeof(out), err,
                                    sizeof(err)) == 0,
              "direct device path resolves");
        CHECK(dvs_tx_resolve_device(table, 1, sdevs, sst, ssig, 1,
                                    "cardA|busA|in0", out, sizeof(out), err,
                                    sizeof(err)) == 0 &&
                  strcmp(out, "/dev/video0") == 0,
              "direct stable ID resolves to current path");
        CHECK(dvs_tx_resolve_device(table, 1, sdevs, sst, ssig, 1, "HDMI9",
                                    out, sizeof(out), err,
                                    sizeof(err)) != 0,
              "unknown port rejected");
    }

    /* port index parsing */
    CHECK(dvs_tx_port_index("HDMI1") == 1, "port index HDMI1");
    CHECK(dvs_tx_port_index("HDMI8") == 8, "port index HDMI8");
    CHECK(dvs_tx_port_index("HDMI0") < 0, "port index rejects HDMI0");
    CHECK(dvs_tx_port_index("HDMI9") < 0, "port index rejects HDMI9");
    CHECK(dvs_tx_port_index("/dev/video0") < 0, "port index rejects path");

    /* codec + bitrate + defaults */
    CHECK(dvs_tx_codec_from_name("h264") == 0, "codec h264");
    CHECK(dvs_tx_codec_from_name("h265") == 1, "codec h265");
    CHECK(dvs_tx_codec_from_name("vp9") < 0, "codec rejects vp9");
    {
        int bps = 0;
        CHECK(dvs_tx_bitrate_to_bps(4000, &bps) == 0 && bps == 4000000,
              "bitrate kbps to bps");
        CHECK(dvs_tx_bitrate_to_bps(0, &bps) != 0, "bitrate rejects zero");
    }
    CHECK(dvs_tx_default_gop(30) == 30, "default gop tracks fps");
    CHECK(dvs_tx_default_gop(0) == 60, "default gop falls back");

    /* scan on this host never fails hard (0 when no capture hardware) */
    {
        char devs[8][DVS_TX_DEV_LEN];
        char st[8][DVS_TX_STABLE_LEN];
        int sig[8];
        int n = dvs_tx_scan_devices(devs, st, sig, 8);
        CHECK(n >= 0 && n <= 8, "scan never fails hard");
    }

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
