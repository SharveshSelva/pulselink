/* frame.c — see frame.h. */
#include "frame.h"
#include "protocol.h"

#include <stdlib.h>
#include <string.h>

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

size_t frame_chunk_build(uint8_t *out, size_t cap, uint32_t device_id,
                         uint16_t width, uint16_t height, uint32_t frame_seq,
                         uint16_t chunk_idx, uint16_t chunk_count,
                         const uint8_t *chunk, uint16_t chunk_len)
{
    if (chunk_len > FRAME_CHUNK_MAX)
        return 0;
    uint16_t plen = (uint16_t)(12 + chunk_len);
    if (cap < (size_t)PL_HDR_SIZE + plen)
        return 0;

    uint8_t *p = out + PL_HDR_SIZE;                 /* payload = frame hdr + chunk */
    put_u16(p + 0, width);
    put_u16(p + 2, height);
    put_u32(p + 4, frame_seq);
    put_u16(p + 8, chunk_idx);
    put_u16(p + 10, chunk_count);
    memcpy(p + 12, chunk, chunk_len);

    uint16_t ck = pl_checksum(p, plen);

    put_u16(out + 0, PL_MAGIC);
    out[2] = (uint8_t)PL_VERSION;
    out[3] = (uint8_t)PKT_FRAME;
    put_u32(out + 4, device_id);
    put_u16(out + 8, plen);
    put_u16(out + 10, ck);

    return (size_t)PL_HDR_SIZE + plen;
}

void frame_asm_init(frame_asm_t *fa)
{
    memset(fa, 0, sizeof *fa);
}

void frame_asm_reset(frame_asm_t *fa)
{
    free(fa->buf);
    free(fa->seen);
    memset(fa, 0, sizeof *fa);
}

int frame_asm_feed(frame_asm_t *fa, const uint8_t *payload, uint16_t payload_len,
                   image_t *out)
{
    if (payload_len < 12)
        return -1;

    uint16_t width  = rd_u16(payload + 0);
    uint16_t height = rd_u16(payload + 2);
    uint32_t fseq   = rd_u32(payload + 4);
    uint16_t cidx   = rd_u16(payload + 8);
    uint16_t ccount = rd_u16(payload + 10);
    const uint8_t *chunk = payload + 12;
    uint16_t chunk_len   = (uint16_t)(payload_len - 12);

    if (width == 0 || height == 0 || ccount == 0)
        return -1;

    uint32_t total = (uint32_t)width * (uint32_t)height;
    if (total > FRAME_MAX_PIXELS)
        return -1;
    if (frame_chunk_count(total) != ccount)        /* chunk_count must be consistent */
        return -1;
    if (cidx >= ccount)
        return -1;

    uint32_t off = (uint32_t)cidx * FRAME_CHUNK_MAX;
    if (off >= total)
        return -1;
    uint32_t expect = total - off;
    if (expect > FRAME_CHUNK_MAX)
        expect = FRAME_CHUNK_MAX;
    if (chunk_len != expect)                        /* exact size for this index */
        return -1;

    /* start (or restart) a frame when idle or the identity changed */
    if (!fa->active || fa->frame_seq != fseq ||
        fa->width != width || fa->height != height) {
        frame_asm_reset(fa);
        fa->buf  = malloc(total);
        fa->seen = calloc(ccount, 1);
        if (!fa->buf || !fa->seen) { frame_asm_reset(fa); return -1; }
        fa->active      = 1;
        fa->width       = width;
        fa->height      = height;
        fa->frame_seq   = fseq;
        fa->chunk_count = ccount;
        fa->total       = total;
        fa->received    = 0;
    }

    if (!fa->seen[cidx]) {                           /* ignore duplicate chunks */
        fa->seen[cidx] = 1;
        fa->received++;
    }
    memcpy(fa->buf + off, chunk, chunk_len);

    if (fa->received == fa->chunk_count) {
        if (image_alloc(out, width, height) != 0) { frame_asm_reset(fa); return -1; }
        memcpy(out->pix, fa->buf, total);
        frame_asm_reset(fa);
        return 1;
    }
    return 0;
}
