/* Unit test for the RX decode-codec selector (x86-runnable, no MTL ctx).
 * Covers file_playout_cfg.codec: default, --codec CLI, JSON "codec" key.
 * Unknown names must be rejected loudly, missing keys keep H264. */
#include "file_playout_config.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

static int parse_cli(file_playout_cfg* cfg, int argc, char** argv) {
    file_playout_cfg_defaults(cfg);
    return file_playout_config_parse_rx_cli(argc, argv, cfg);
}

int main(void) {
    file_playout_cfg cfg;

    /* default stays H264 */
    file_playout_cfg_defaults(&cfg);
    CHECK(cfg.codec == CODEC_FMT_H264, "default codec is h264");

    /* CLI selector */
    {
        char* av[] = {"rx", "--codec", "h265"};
        CHECK(parse_cli(&cfg, 3, av) == 0 && cfg.codec == CODEC_FMT_H265,
              "cli --codec h265");
    }
    {
        char* av[] = {"rx", "--codec", "hevc"};
        CHECK(parse_cli(&cfg, 3, av) == 0 && cfg.codec == CODEC_FMT_H265,
              "cli --codec hevc");
    }
    {
        char* av[] = {"rx", "--codec", "H265"};
        CHECK(parse_cli(&cfg, 3, av) == 0 && cfg.codec == CODEC_FMT_H265,
              "cli --codec H265");
    }
    {
        char* av[] = {"rx", "--codec", "h264"};
        file_playout_cfg_defaults(&cfg);
        cfg.codec = CODEC_FMT_H265;
        CHECK(parse_cli(&cfg, 3, av) == 0 && cfg.codec == CODEC_FMT_H264,
              "cli --codec h264 overrides");
    }
    {
        char* av[] = {"rx", "--codec", "av1"};
        CHECK(parse_cli(&cfg, 3, av) != 0, "cli unknown codec rejected");
    }
    {
        char* av[] = {"rx", "--codec", "h265", "--width", "1280"};
        CHECK(parse_cli(&cfg, 5, av) == 0 && cfg.codec == CODEC_FMT_H265 &&
                  cfg.width == 1280,
              "cli codec composes with other flags");
    }

    /* JSON loader */
    {
        char path[64];
        snprintf(path, sizeof(path), "/tmp/test_codec_cfg_%d.json", getpid());
        FILE* f = fopen(path, "w");
        if (!f) {
            printf("  FAIL json tmp create\n");
            failures++;
        } else {
            fputs("{\"codec\":\"h265\",\"width\":1920}", f);
            fclose(f);
            file_playout_cfg_defaults(&cfg);
            CHECK(file_playout_config_load(path, &cfg) == 0 &&
                      cfg.codec == CODEC_FMT_H265 && cfg.width == 1920,
                  "json codec h265");
            remove(path);
        }
    }
    {
        char path[64];
        snprintf(path, sizeof(path), "/tmp/test_codec_cfg_%d.json", getpid());
        FILE* f = fopen(path, "w");
        if (!f) {
            printf("  FAIL json tmp create\n");
            failures++;
        } else {
            fputs("{\"width\":1920}", f);
            fclose(f);
            file_playout_cfg_defaults(&cfg);
            CHECK(file_playout_config_load(path, &cfg) == 0 &&
                      cfg.codec == CODEC_FMT_H264,
                  "json missing codec keeps h264");
            remove(path);
        }
    }
    {
        char path[64];
        snprintf(path, sizeof(path), "/tmp/test_codec_cfg_%d.json", getpid());
        FILE* f = fopen(path, "w");
        if (!f) {
            printf("  FAIL json tmp create\n");
            failures++;
        } else {
            fputs("{\"codec\":\"av1\"}", f);
            fclose(f);
            file_playout_cfg_defaults(&cfg);
            CHECK(file_playout_config_load(path, &cfg) != 0,
                  "json unknown codec rejected");
            remove(path);
        }
    }

    if (failures) {
        printf("test_codec_config: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_codec_config: all passed\n");
    return 0;
}
