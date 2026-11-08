/* Unit test for the ST2110-21 pacing profile parser (x86-runnable, no MTL ctx). */
#include "file_playout_config.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, name)                                                    \
    do {                                                                     \
        if (cond) {                                                          \
            printf("  PASS %s\n", name);                                     \
        } else {                                                             \
            printf("  FAIL %s\n", name);                                     \
            failures++;                                                      \
        }                                                                    \
    } while (0)

int main(void) {
    file_playout_pacing way;

    /* defaults and valid strings */
    CHECK(file_playout_parse_pacing(NULL, &way) == 0 &&
              way == FILE_PLAYOUT_PACING_TSC_NARROW,
          "NULL falls back to tsc_narrow");
    CHECK(file_playout_parse_pacing("", &way) == 0 &&
              way == FILE_PLAYOUT_PACING_TSC_NARROW,
          "empty string falls back to tsc_narrow");
    CHECK(file_playout_parse_pacing("tsc_narrow", &way) == 0 &&
              way == FILE_PLAYOUT_PACING_TSC_NARROW, "tsc_narrow");
    CHECK(file_playout_parse_pacing("auto", &way) == 0 &&
              way == FILE_PLAYOUT_PACING_AUTO, "auto");
    CHECK(file_playout_parse_pacing("tsc", &way) == 0 &&
              way == FILE_PLAYOUT_PACING_TSC, "tsc");
    CHECK(file_playout_parse_pacing("rl", &way) == 0 &&
              way == FILE_PLAYOUT_PACING_RL, "rl");

    /* invalid strings rejected */
    CHECK(file_playout_parse_pacing("narrow", &way) == -1, "unknown value rejected");
    CHECK(file_playout_parse_pacing("TSC", &way) == -1, "case sensitive");

    /* string round trip */
    CHECK(strcmp(file_playout_pacing_str(FILE_PLAYOUT_PACING_TSC_NARROW),
                 "tsc_narrow") == 0, "str tsc_narrow");
    CHECK(strcmp(file_playout_pacing_str(FILE_PLAYOUT_PACING_AUTO), "auto") == 0,
          "str auto");

    /* MTL mapping */
    CHECK(file_playout_pacing_to_st21(FILE_PLAYOUT_PACING_TSC_NARROW) ==
              ST21_TX_PACING_WAY_TSC_NARROW, "map tsc_narrow");
    CHECK(file_playout_pacing_to_st21(FILE_PLAYOUT_PACING_AUTO) ==
              ST21_TX_PACING_WAY_AUTO, "map auto");
    CHECK(file_playout_pacing_to_st21(FILE_PLAYOUT_PACING_RL) ==
              ST21_TX_PACING_WAY_RL, "map rl");
    CHECK(file_playout_pacing_to_st21(FILE_PLAYOUT_PACING_TSC) ==
              ST21_TX_PACING_WAY_TSC, "map tsc");

    if (failures) {
        printf("test_pacing_config: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_pacing_config: all passed\n");
    return 0;
}
