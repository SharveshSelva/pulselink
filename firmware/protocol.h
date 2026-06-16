/* protocol.h — PulseLink wire protocol (shared between firmware and server)
 *
 * This header is the single source of truth for the on-the-wire format.
 * It is intentionally dependency-light (stdint/stddef only) so the exact
 * same file compiles on the ESP32 (lwIP) and on Linux. Byte-swapping
 * (htons/htonl) happens at the call sites, NOT in here, so this header
 * stays portable.
 *
 * RULE: every multi-byte field travels big-endian (network byte order).
 */
#ifndef PULSELINK_PROTOCOL_H
#define PULSELINK_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

/* ---- framing constants -------------------------------------------------- */
#define PL_MAGIC      0x504Cu   /* "PL" */
#define PL_VERSION    0x01u
#define PL_HDR_SIZE   12u       /* sizeof(pl_header_t) on the wire */
#define PL_MAX_PAYLOAD 1100u    /* sane upper bound (frame chunks are <=1024) */

/* ---- packet types ------------------------------------------------------- */
typedef enum {
    PKT_HELLO     = 0x01,  /* device->server : firmware_version(4), caps(4)   */
    PKT_HELLO_ACK = 0x02,  /* server->device : session_id(4)                  */
    PKT_SENSOR    = 0x03,  /* device->server : pl_sensor_payload_t            */
    PKT_ACK       = 0x04,  /* server->device : acked seq(4)                   */
    PKT_HEARTBEAT = 0x05,  /* device->server (UDP) : uptime_ms(4),free_heap(4)*/
    PKT_FRAME     = 0x06,  /* device->server : frame header + chunk (Stage 4) */
    PKT_ERROR     = 0x07   /* server->device : error code(2)                  */
} pkt_type_t;

/* ---- error codes (PKT_ERROR payload) ------------------------------------ */
typedef enum {
    PL_ERR_BAD_MAGIC    = 0x0001,
    PL_ERR_BAD_VERSION  = 0x0002,
    PL_ERR_BAD_CHECKSUM = 0x0003,
    PL_ERR_BAD_TYPE     = 0x0004,
    PL_ERR_TOO_BIG      = 0x0005,
    PL_ERR_NO_HELLO     = 0x0006   /* sent data before handshake */
} pl_error_t;

/* ---- header (12 bytes, prefixes every packet) --------------------------- */
typedef struct __attribute__((packed)) {
    uint16_t magic;        /* PL_MAGIC                                  */
    uint8_t  version;      /* PL_VERSION                                */
    uint8_t  type;         /* pkt_type_t                                */
    uint32_t device_id;    /* unique per device                        */
    uint16_t payload_len;  /* bytes following the header               */
    uint16_t checksum;     /* pl_checksum() over the payload bytes     */
} pl_header_t;

/* ---- payloads ----------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t firmware_version;
    uint32_t capabilities;     /* bitmask */
} pl_hello_payload_t;          /* 8 bytes */

typedef struct __attribute__((packed)) {
    uint32_t session_id;
} pl_hello_ack_payload_t;      /* 4 bytes */

typedef struct __attribute__((packed)) {
    uint32_t seq;              /* monotonic per device          */
    uint32_t timestamp_ms;     /* device uptime                 */
    int32_t  temperature;      /* degC * 100 (2654 == 26.54 C)  */
    int32_t  humidity;         /* %    * 100                    */
} pl_sensor_payload_t;         /* 16 bytes */

typedef struct __attribute__((packed)) {
    uint32_t acked_seq;
} pl_ack_payload_t;            /* 4 bytes */

typedef struct __attribute__((packed)) {
    uint32_t uptime_ms;
    uint32_t free_heap;
} pl_heartbeat_payload_t;      /* 8 bytes */

typedef struct __attribute__((packed)) {
    uint16_t error_code;       /* pl_error_t */
} pl_error_payload_t;          /* 2 bytes */

/* Frame header (Stage 4); kept here so both ends agree early. */
typedef struct __attribute__((packed)) {
    uint16_t width;
    uint16_t height;
    uint32_t frame_seq;
    uint16_t chunk_idx;
    uint16_t chunk_count;
    /* grayscale chunk bytes follow, <= 1024 */
} pl_frame_header_t;           /* 12 bytes */

/* Compile-time guarantees: structs are exactly the documented wire sizes.
 * If padding ever sneaks in, the build breaks here instead of on the wire. */
_Static_assert(sizeof(pl_header_t)          == 12, "pl_header_t must be 12B");
_Static_assert(sizeof(pl_hello_payload_t)   == 8,  "hello must be 8B");
_Static_assert(sizeof(pl_sensor_payload_t)  == 16, "sensor must be 16B");
_Static_assert(sizeof(pl_heartbeat_payload_t)== 8, "heartbeat must be 8B");
_Static_assert(sizeof(pl_frame_header_t)    == 12, "frame hdr must be 12B");

/* ---- checksum ----------------------------------------------------------- */
/* 16-bit ones'-complement sum (RFC 1071, "IP checksum"), computed over the
 * payload bytes. Returns a host-order value; the caller stores it with htons
 * and verifies by recomputing and comparing against ntohs(hdr.checksum).
 * Implemented once in checksum.c and unit-tested. */
uint16_t pl_checksum(const uint8_t *data, size_t len);

#endif /* PULSELINK_PROTOCOL_H */
