/* Unit test for engine/audio_owner.c (no compositor/ALSA dependency). */
#include "../engine/audio_owner.h"

#include <stdio.h>

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
    int lane = -1;

    CHECK(dvs_audio_owner_classify(NULL) == DVS_OWNER_MUTE,
          "classify treats NULL as mute");
    CHECK(dvs_audio_owner_classify("") == DVS_OWNER_MUTE,
          "classify treats empty as mute");
    CHECK(dvs_audio_owner_classify("lane0") == DVS_OWNER_LANE,
          "classify accepts lane0");
    CHECK(dvs_audio_owner_classify("lane7") == DVS_OWNER_LANE,
          "classify accepts lane7");
    CHECK(dvs_audio_owner_classify("lane") == DVS_OWNER_INVALID,
          "classify rejects bare lane prefix");
    CHECK(dvs_audio_owner_classify("laneX") == DVS_OWNER_INVALID,
          "classify rejects non-numeric lane");
    CHECK(dvs_audio_owner_classify("3") == DVS_OWNER_INDEX,
          "classify accepts a bare decimal index");
    CHECK(dvs_audio_owner_classify("239.0.0.1:5000") == DVS_OWNER_ENDPOINT,
          "classify accepts sip:port");
    CHECK(dvs_audio_owner_classify("239.0.0.1") == DVS_OWNER_SIP,
          "classify accepts a bare sip");
    CHECK(dvs_audio_owner_classify("main") == DVS_OWNER_INVALID,
          "classify rejects a window name without fallback");
    CHECK(dvs_audio_owner_classify("239.0.0.1:0") == DVS_OWNER_INVALID,
          "classify rejects a zero audio port");
    CHECK(dvs_audio_owner_classify("999.0.0.1") == DVS_OWNER_INVALID,
          "classify rejects a malformed address");
    CHECK(dvs_audio_owner_classify("1:2:3") == DVS_OWNER_INVALID,
          "classify rejects a double colon owner");
    CHECK(dvs_audio_owner_classify("lane255") == DVS_OWNER_LANE,
          "classify accepts the top lane index");
    CHECK(dvs_audio_owner_classify("lane256") == DVS_OWNER_INVALID,
          "classify rejects a lane past the range");
    CHECK(dvs_audio_owner_classify("lane9999999999") == DVS_OWNER_INVALID,
          "classify rejects an overflowing lane number");
    CHECK(dvs_audio_owner_classify("LANE1") == DVS_OWNER_INVALID,
          "classify rejects an uppercase lane prefix");
    CHECK(dvs_audio_owner_classify(" 1") == DVS_OWNER_INVALID,
          "classify rejects a padded index");
    CHECK(dvs_audio_owner_classify("999") == DVS_OWNER_INDEX,
          "classify accepts an out-of-range decimal as index kind");
    CHECK(dvs_audio_owner_classify("239.0.0.1:65535") == DVS_OWNER_ENDPOINT,
          "classify accepts the top audio port");
    CHECK(dvs_audio_owner_classify("239.0.0.1:65536") == DVS_OWNER_INVALID,
          "classify rejects a port past the range");
    CHECK(dvs_audio_owner_classify("239.0.0.1:") == DVS_OWNER_INVALID,
          "classify rejects an empty port");
    CHECK(dvs_audio_owner_classify(":5000") == DVS_OWNER_INVALID,
          "classify rejects an empty address");
    CHECK(dvs_audio_owner_classify("239.0.0.1:abc") == DVS_OWNER_INVALID,
          "classify rejects a non-numeric port");

    CHECK(dvs_audio_owner_lane_index("lane2", &lane) == 0 && lane == 2,
          "lane index resolves lane2");
    CHECK(dvs_audio_owner_lane_index("5", &lane) == 0 && lane == 5,
          "lane index resolves a bare decimal");
    CHECK(dvs_audio_owner_lane_index("239.0.0.1", &lane) != 0,
          "lane index refuses a sip owner");
    CHECK(dvs_audio_owner_lane_index("lane1", NULL) != 0,
          "lane index refuses a NULL out-param");
    CHECK(dvs_audio_owner_lane_index("0", &lane) == 0 && lane == 0,
          "lane index resolves zero");
    CHECK(dvs_audio_owner_lane_index("lane300", &lane) != 0,
          "lane index refuses an out-of-range lane");
    CHECK(dvs_audio_owner_lane_index("999", &lane) != 0,
          "lane index refuses an out-of-range decimal");
    CHECK(dvs_audio_owner_lane_index(NULL, &lane) != 0,
          "lane index refuses a NULL owner");
    CHECK(dvs_audio_owner_lane_index("", &lane) != 0,
          "lane index refuses an empty owner");

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
