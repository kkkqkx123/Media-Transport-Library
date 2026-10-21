/* SPDX-License-Identifier: BSD-3-Clause
 *
 * MPP H.264 hardware encoder (RK3588 VPU).
 * Input: NV12 dma-buf frames
 * Output: H.264 Annex B coded packets (allocated from slab_pool)
 */

#ifndef _E2E_MPP_ENCODER_H_
#define _E2E_MPP_ENCODER_H_

#include "codec_plugin.h"
#include "../hal/slab_pool.h"

struct codec_plugin* mpp_encoder_create(struct codec_config* cfg,
                                         struct slab_pool* coded_pool);

#endif
