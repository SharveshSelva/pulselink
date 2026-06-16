/* telemetry.c — see telemetry.h.
 *
 * Big-endian bytes are written/read explicitly (no htons/htonl, no struct
 * packing), so this is endian-independent and needs zero networking headers.
 * Byte layouts match the payload structs in protocol.h exactly, which is what
 * lets the server decode firmware packets (and the firmware decode the server's)
 * with no end-specific knowledge.
 */
#include "telemetry.h"
#include "protocol.h"

_Static_assert(PL_FW_HDR_SIZE           == PL_HDR_SIZE,      "header size mismatch");
_Static_assert(PL_SENSOR_PACKET_SIZE    == PL_HDR_SIZE + 16, "sensor size mismatch");
_Static_assert(PL_HELLO_PACKET_SIZE     == PL_HDR_SIZE + 8,  "hello size mismatch");
_Static_assert(PL_HEARTBEAT_PACKET_SIZE == PL_HDR_SIZE + 8,  "heartbeat size mismatch");
_Static_assert(PL_TYPE_HELLO_ACK == PKT_HELLO_ACK, "hello_ack type mismatch");
_Static_assert(PL_TYPE_ACK       == PKT_ACK,       "ack type mismatch");
_Static_assert(PL_TYPE_ERROR     == PKT_ERROR,     "error type mismatch");

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xFF);
}

/* Fill the 12-byte header in place given a finished payload. */
static void put_header(uint8_t *out, uint8_t type, uint32_t device_id,
                       uint16_t plen)
{
    put_u16(out + 0, PL_MAGIC);
    out[2] = (uint8_t)PL_VERSION;
    out[3] = type;
    put_u32(out + 4, device_id);
    put_u16(out + 8, plen);
    put_u16(out + 10, pl_checksum(out + PL_HDR_SIZE, plen));
}

size_t pl_build_sensor(uint8_t *out, size_t cap, uint32_t device_id,
                       uint32_t seq, uint32_t timestamp_ms,
                       int32_t temperature, int32_t humidity)
{
    const uint16_t plen = 16;
    if (cap < (size_t)PL_HDR_SIZE + plen) return 0;
    uint8_t *p = out + PL_HDR_SIZE;
    put_u32(p +  0, seq);
    put_u32(p +  4, timestamp_ms);
    put_u32(p +  8, (uint32_t)temperature);
    put_u32(p + 12, (uint32_t)humidity);
    put_header(out, (uint8_t)PKT_SENSOR, device_id, plen);
    return (size_t)PL_HDR_SIZE + plen;
}

size_t pl_build_hello(uint8_t *out, size_t cap, uint32_t device_id,
                      uint32_t firmware_version, uint32_t capabilities)
{
    const uint16_t plen = 8;
    if (cap < (size_t)PL_HDR_SIZE + plen) return 0;
    uint8_t *p = out + PL_HDR_SIZE;
    put_u32(p + 0, firmware_version);
    put_u32(p + 4, capabilities);
    put_header(out, (uint8_t)PKT_HELLO, device_id, plen);
    return (size_t)PL_HDR_SIZE + plen;
}

size_t pl_build_heartbeat(uint8_t *out, size_t cap, uint32_t device_id,
                          uint32_t uptime_ms, uint32_t free_heap)
{
    const uint16_t plen = 8;
    if (cap < (size_t)PL_HDR_SIZE + plen) return 0;
    uint8_t *p = out + PL_HDR_SIZE;
    put_u32(p + 0, uptime_ms);
    put_u32(p + 4, free_heap);
    put_header(out, (uint8_t)PKT_HEARTBEAT, device_id, plen);
    return (size_t)PL_HDR_SIZE + plen;
}

int pl_parse_header_fw(const uint8_t *hdr, pl_rx_header_t *out)
{
    uint16_t magic = pl_rd_u16(hdr);
    if (magic != PL_MAGIC) return 0;
    if (hdr[2] != PL_VERSION) return 0;
    out->type        = hdr[3];
    out->device_id   = pl_rd_u32(hdr + 4);
    out->payload_len = pl_rd_u16(hdr + 8);
    out->checksum    = pl_rd_u16(hdr + 10);
    return 1;
}
