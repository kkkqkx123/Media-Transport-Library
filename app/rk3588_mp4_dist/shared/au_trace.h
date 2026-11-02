#ifndef _MP4_AU_TRACE_H_
#define _MP4_AU_TRACE_H_

#include <stddef.h>
#include <stdint.h>

enum au_trace_stage {
    AU_TRACE_TX_PUBLISH = 0,
    AU_TRACE_TX_WIRE,
    AU_TRACE_RX_MTL,
    AU_TRACE_RX_BRIDGE,
};

void au_trace_record(enum au_trace_stage stage, const uint8_t* data, size_t size,
                     int64_t extra);

void au_trace_close(void);

#endif /* _MP4_AU_TRACE_H_ */
