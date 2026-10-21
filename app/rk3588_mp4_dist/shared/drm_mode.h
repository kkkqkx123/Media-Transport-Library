/* SPDX-License-Identifier: BSD-3-Clause
 *
 * DRM mode string helpers — derive refresh from content fps (P4).
 */

#ifndef _MP4_DRM_MODE_H_
#define _MP4_DRM_MODE_H_

#include <stdbool.h>
#include <stddef.h>

/** True when mode is empty or "auto" (case-insensitive). */
bool mp4_drm_mode_is_auto(const char* mode_str);

/**
 * Map content fps to preferred HDMI vrefresh (23.976→24, 59.94→60, …).
 * Returns nearest integer Hz when no standard rate matches.
 */
int mp4_drm_suggest_refresh(float fps);

/**
 * Build "WxH@Hz" from content parameters.
 * Uses 1920x1080 defaults when w/h <= 0.
 */
int mp4_drm_build_mode_string(int width, int height, float fps,
                              char* out, size_t out_len);

#endif /* _MP4_DRM_MODE_H_ */
