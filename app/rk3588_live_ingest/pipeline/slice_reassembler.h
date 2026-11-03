#ifndef _LIVE_INGEST_SLICE_REASSEMBLER_H_
#define _LIVE_INGEST_SLICE_REASSEMBLER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct slice_reassembler;
struct slab_pool;

/** Create a reassembler for one picture. */
struct slice_reassembler* slice_reasm_create(void);
void slice_reasm_free(struct slice_reassembler* r);

/**
 * Feed a coded slice (Annex B format with start-code prefix).
 * When all slices for a picture have been received, returns the complete
 * frame via frame_out/frame_size_out. The caller owns the returned buffer
 * (allocated with malloc).
 *
 * @param data   Annex B coded slice data (with 00 00 00 01 prefix)
 * @param size   size of data in bytes
 * @param frame_out [out] complete frame (NULL if picture not yet complete)
 * @param frame_size_out [out] size of complete frame
 * @return 1 if a complete frame is ready, 0 if waiting for more slices,
 *         -1 on error.
 */
int slice_reasm_feed(struct slice_reassembler* r,
                     uint8_t* data, size_t size,
                     uint8_t** frame_out, size_t* frame_size_out);

/**
 * Reset internal state for a new picture. Call after frame_out is consumed.
 */
void slice_reasm_reset(struct slice_reassembler* r);

/**
 * Attach an optional slab pool for zero-copy frame handoff.
 * With a pool, the accumulation buffer is a slab block handed to the caller
 * on emit (release with slice_reasm_free_frame); a picture larger than one
 * block fails the feed instead of growing the heap. Without a pool the
 * previous malloc behaviour is kept. Call before the first feed.
 */
void slice_reasm_set_pool(struct slice_reassembler* r, struct slab_pool* pool);

/** Buffered but not yet emitted bytes (for timeout decisions). */
size_t slice_reasm_pending(struct slice_reassembler* r);

/** Release a frame emitted by feed/flush (slab-aware). */
void slice_reasm_free_frame(struct slice_reassembler* r, uint8_t* frame);

/**
 * Flush buffered data even when the picture is incomplete (timeout / stop).
 * Returns 1 with a frame, 0 when nothing was buffered.
 */
int slice_reasm_flush(struct slice_reassembler* r,
                      uint8_t** frame_out, size_t* frame_size_out);

#endif /* _LIVE_INGEST_SLICE_REASSEMBLER_H_ */
