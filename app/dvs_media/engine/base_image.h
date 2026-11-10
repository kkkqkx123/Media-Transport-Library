/* Wall still decode — node-local half of the platform base image.
 *
 * Responsibility split: the platform names the still (revision + url) and the
 * node-local sub-rectangle; the agent downloads the bytes to a file; this
 * module decodes the file, crops the sub-rectangle and stretches it to the
 * node canvas in XRGB8888 for the DRM background buffer. Pure decode/scale:
 * no MTL/DRM/MPP dependency so the math stays unit-testable where FFmpeg
 * exists. FFmpeg software decode only (PNG/JPEG stills); hardware paths are
 * never needed for a static backdrop.
 *
 * Supported decoder outputs: RGB24, RGBA, BGR24, GRAY8, YUVJ420P/YUV420P
 * (BT.601). Anything else reports unsupported instead of guessing.
 */
#ifndef _DVS_BASE_IMAGE_H_
#define _DVS_BASE_IMAGE_H_

#include <stddef.h>

/* Wall canvas ceiling shared with the output path (8K wide, 4K high).
 * Larger canvases are rejected instead of being silently downscaled. */
#define DVS_BASE_MAX_W 7680
#define DVS_BASE_MAX_H 4320

#ifdef __cplusplus
extern "C" {
#endif

/* Decode + crop + stretch. path is a local image file; sub_* is the crop in
 * source pixels (clamped); canvas_* is the output size. On success returns 0
 * with *px_out holding canvas_w*canvas_h XRGB8888 pixels (caller frees with
 * free()) and fills *err only on failure. */
int dvs_base_decode(const char* path,
                    int sub_x, int sub_y, int sub_w, int sub_h,
                    int canvas_w, int canvas_h,
                    unsigned char** px_out,
                    char* err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* _DVS_BASE_IMAGE_H_ */
