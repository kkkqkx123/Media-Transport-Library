#ifndef _LIVE_INGEST_MPP_ENCODER_H_
#define _LIVE_INGEST_MPP_ENCODER_H_

#include "codec_plugin.h"
#include "slab_pool.h"

struct codec_plugin* mpp_encoder_create(struct codec_config* cfg,
                                         struct slab_pool* coded_pool);

#endif
