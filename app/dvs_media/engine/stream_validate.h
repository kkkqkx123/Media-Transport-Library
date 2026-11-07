/* Stream shape validation — pure checks, no MTL/DRM/MPP dependency.
 *
 * Used by the IPC prepare path to fail fast on malformed layouts before
 * anything is staged in the shadow layout. Commit-time validation inside
 * the compositor/display_layout chain remains authoritative; these helpers
 * enforce the same rules earlier so prepare and commit cannot disagree. */

#ifndef _DVS_STREAM_VALIDATE_H_
#define _DVS_STREAM_VALIDATE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Identity and size fields every lane must carry. 0 ok, -1 with err set. */
int dvs_stream_shape_valid(const char* sip, uint16_t udp_port, int width,
                           int height, float fps, char* err, size_t errlen);

/* Source crop bounds. Zero w/h means the full frame (either axis falls back
 * independently, matching display_layout). 0 ok, -1 with err set. */
int dvs_stream_crop_valid(int width, int height, int src_x, int src_y,
                          int src_w, int src_h, char* err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* _DVS_STREAM_VALIDATE_H_ */
