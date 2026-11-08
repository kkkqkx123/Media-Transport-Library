/* Unit test for engine/stream_validate.c (no MTL/MPP/DRM dependency). */
#include "../engine/stream_validate.h"

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
    char err[128];

    CHECK(dvs_stream_shape_valid("239.0.0.1", 5000, 1920, 1080, 30.0f,
                                 err, sizeof(err)) == 0,
          "shape accepts a complete stream");
    CHECK(dvs_stream_shape_valid("", 5000, 1920, 1080, 30.0f, err,
                                 sizeof(err)) != 0,
          "shape rejects missing sip");
    CHECK(dvs_stream_shape_valid("239.0.0.1", 0, 1920, 1080, 30.0f, err,
                                 sizeof(err)) != 0,
          "shape rejects zero port");
    CHECK(dvs_stream_shape_valid("239.0.0.1", 5000, 0, 1080, 30.0f, err,
                                 sizeof(err)) != 0,
          "shape rejects zero width");
    CHECK(dvs_stream_shape_valid("239.0.0.1", 5000, 1920, 0, 30.0f, err,
                                 sizeof(err)) != 0,
          "shape rejects zero height");
    CHECK(dvs_stream_shape_valid("239.0.0.1", 5000, -1920, 1080, 30.0f, err,
                                 sizeof(err)) != 0,
          "shape rejects negative width");
    CHECK(dvs_stream_shape_valid("239.0.0.1", 5000, 1920, 1080, -1.0f, err,
                                 sizeof(err)) != 0,
          "shape rejects negative fps");
    CHECK(dvs_stream_shape_valid(NULL, 5000, 1920, 1080, 30.0f, err,
                                 sizeof(err)) != 0,
          "shape rejects NULL sip");
    CHECK(dvs_stream_shape_valid("239.0.0.1", 5000, 1920, 1080, 30.0f,
                                 NULL, 0) == 0,
          "shape tolerates a NULL error buffer on success");
    CHECK(dvs_stream_shape_valid("239.0.0.1", 5000, 1920, 1080, 0.0f, err,
                                 sizeof(err)) != 0,
          "shape rejects zero fps");

    CHECK(dvs_stream_crop_valid(1920, 1080, 0, 0, 0, 0, err, sizeof(err)) == 0,
          "crop accepts full frame (zero w/h)");
    CHECK(dvs_stream_crop_valid(1920, 1080, 100, 50, 640, 360, err,
                                sizeof(err)) == 0,
          "crop accepts an inside offset window");
    CHECK(dvs_stream_crop_valid(1920, 1080, 0, 0, 1920, 1080, err,
                                sizeof(err)) == 0,
          "crop accepts an explicit full-frame window");
    CHECK(dvs_stream_crop_valid(1920, 1080, 0, 0, 0, 360, err, sizeof(err)) == 0,
          "crop accepts mixed zero-w partial-h (axis fallback)");
    CHECK(dvs_stream_crop_valid(1920, 1080, 1800, 0, 200, 100, err,
                                sizeof(err)) != 0,
          "crop rejects a window overflowing the right edge");
    CHECK(dvs_stream_crop_valid(1920, 1080, 0, 1000, 100, 200, err,
                                sizeof(err)) != 0,
          "crop rejects a window overflowing the bottom edge");
    CHECK(dvs_stream_crop_valid(1920, 1080, -10, 0, 100, 100, err,
                                sizeof(err)) != 0,
          "crop rejects a negative origin");
    CHECK(dvs_stream_crop_valid(1920, 1080, 0, 0, 0, 0, NULL, 0) == 0,
          "crop tolerates a NULL error buffer on success");
    CHECK(dvs_stream_crop_valid(1920, 1080, 0, 0, 1920, 0, err,
                                sizeof(err)) == 0,
          "crop accepts zero-h axis fallback to full height");
    CHECK(dvs_stream_crop_valid(1920, 1080, 0, 0, -4, 100, err,
                                sizeof(err)) == 0,
          "crop treats a negative width as full-frame fallback");
    CHECK(dvs_stream_crop_valid(0, 0, 0, 0, 0, 0, err, sizeof(err)) != 0,
          "crop rejects a zero-size frame");
    CHECK(dvs_stream_crop_valid(1920, 1080, 1900, 0, 100, 100, NULL, 0) != 0,
          "crop failure tolerates a NULL error buffer");
    {
        char e2[128] = {0};
        CHECK(dvs_stream_crop_valid(1920, 1080, 1900, 0, 100, 100, e2,
                                    sizeof(e2)) != 0 &&
                  e2[0] != '\0',
              "crop failure reports the offending window");
    }

    /* SSM source filter: empty means any-source, otherwise dotted IPv4. */
    CHECK(dvs_stream_src_valid(NULL, err, sizeof(err)) == 0,
          "src filter accepts NULL as any-source");
    CHECK(dvs_stream_src_valid("", err, sizeof(err)) == 0,
          "src filter accepts empty as any-source");
    CHECK(dvs_stream_src_valid("192.168.10.11", err, sizeof(err)) == 0,
          "src filter accepts IPv4");
    CHECK(dvs_stream_src_valid("999.1.1.1", err, sizeof(err)) != 0,
          "src filter rejects bad octet");
    CHECK(dvs_stream_src_valid("1.2.3", err, sizeof(err)) != 0,
          "src filter rejects short address");
    CHECK(dvs_stream_src_valid("1.2.3.4.5", err, sizeof(err)) != 0,
          "src filter rejects long address");
    CHECK(dvs_stream_src_valid("abc", NULL, 0) != 0,
          "src filter tolerates a NULL error buffer");

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
