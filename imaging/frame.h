/* frame.h — carry a grayscale frame across the PulseLink protocol.
 *
 * A frame is split into <=1024-byte chunks, each sent as a PKT_FRAME packet
 * (12-byte wire header + 12-byte pl_frame_header_t + chunk bytes). The
 * reassembler collects the chunks back into a complete image. This is the same
 * protocol.h header the rest of the project uses, so frames travel the same pipe
 * as sensor readings.
 */
#ifndef PULSELINK_FRAME_H
#define PULSELINK_FRAME_H

#include "image.h"
#include <stddef.h>
#include <stdint.h>

#define FRAME_CHUNK_MAX   1024
#define FRAME_MAX_PIXELS  (1u << 22)

/* Build one PKT_FRAME wire packet into out. Returns total bytes, or 0 if the
 * chunk is oversized or the buffer too small. */
size_t frame_chunk_build(uint8_t *out, size_t cap, uint32_t device_id,
                         uint16_t width, uint16_t height, uint32_t frame_seq,
                         uint16_t chunk_idx, uint16_t chunk_count,
                         const uint8_t *chunk, uint16_t chunk_len);

/* How many chunks an image of `total` bytes needs. */
static inline uint16_t frame_chunk_count(uint32_t total)
{
    return (uint16_t)((total + FRAME_CHUNK_MAX - 1) / FRAME_CHUNK_MAX);
}

typedef struct {
    int      active;
    uint16_t width, height, chunk_count, received;
    uint32_t frame_seq, total;
    uint8_t *buf;    /* width*height bytes */
    uint8_t *seen;   /* chunk_count flags  */
} frame_asm_t;

void frame_asm_init(frame_asm_t *fa);
void frame_asm_reset(frame_asm_t *fa);   /* frees buffers, returns to idle */

/* Feed one PKT_FRAME payload (bytes AFTER the 12-byte wire header). On the chunk
 * that completes the frame, fills *out (caller image_free()s) and returns 1;
 * returns 0 if more chunks are needed, -1 on a malformed/inconsistent chunk. */
int frame_asm_feed(frame_asm_t *fa, const uint8_t *payload, uint16_t payload_len,
                   image_t *out);

#endif /* PULSELINK_FRAME_H */
