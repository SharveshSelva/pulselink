/* telemetry.h — the firmware's wire-framing + parsing seam.
 *
 * Pure C, no networking headers, so the identical code compiles on the ESP32
 * and on the host, where fw_frame_test.c cross-checks every builder against the
 * server's own parser and vice-versa. The net task sends/receives these bytes.
 */
#ifndef PULSELINK_TELEMETRY_H
#define PULSELINK_TELEMETRY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* On-wire packet sizes (12-byte header + payload). Exposed here so the C++
 * sketch can size buffers without including protocol.h (whose C11
 * _Static_assert some Arduino GCCs reject in C++ mode). telemetry.c asserts
 * these match protocol.h. */
#define PL_FW_HDR_SIZE            12
#define PL_SENSOR_PACKET_SIZE     28   /* hdr + 16 */
#define PL_HELLO_PACKET_SIZE      20   /* hdr + 8  */
#define PL_HEARTBEAT_PACKET_SIZE  20   /* hdr + 8  */

/* A parsed inbound header (host byte order). */
typedef struct {
    uint8_t  type;
    uint32_t device_id;
    uint16_t payload_len;
    uint16_t checksum;
} pl_rx_header_t;

/* Inbound packet types the sketch inspects (values mirror pkt_type_t in
 * protocol.h; telemetry.c static-asserts they match, so the sketch never needs
 * to include protocol.h). */
#define PL_TYPE_HELLO_ACK  0x02
#define PL_TYPE_ACK        0x04
#define PL_TYPE_ERROR      0x07

/* Builders: return total bytes written, or 0 if the buffer is too small. */
size_t pl_build_sensor(uint8_t *out, size_t cap, uint32_t device_id,
                       uint32_t seq, uint32_t timestamp_ms,
                       int32_t temperature, int32_t humidity);
size_t pl_build_hello(uint8_t *out, size_t cap, uint32_t device_id,
                      uint32_t firmware_version, uint32_t capabilities);
size_t pl_build_heartbeat(uint8_t *out, size_t cap, uint32_t device_id,
                          uint32_t uptime_ms, uint32_t free_heap);

/* Decode a 12-byte header; returns 1 if magic+version are valid, else 0.
 * (The caller then reads payload_len bytes and verifies pl_checksum.) */
int pl_parse_header_fw(const uint8_t *hdr, pl_rx_header_t *out);

/* Checksum over the payload bytes (implemented in checksum.c). Re-declared so
 * the sketch needn't include protocol.h. */
uint16_t pl_checksum(const uint8_t *data, size_t len);

/* Big-endian field readers for payloads (e.g. HELLO_ACK session_id). */
static inline uint16_t pl_rd_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static inline uint32_t pl_rd_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

#ifdef __cplusplus
}
#endif

#endif /* PULSELINK_TELEMETRY_H */
