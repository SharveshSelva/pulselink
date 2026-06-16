/* wire.h — packet (de)framing. Keeps all htons/htonl + checksum logic in one
 * place so the server and fake_client cannot disagree on the byte layout, and
 * so protocol.h stays free of any networking headers (it must compile on the
 * ESP32 too). */
#ifndef PULSELINK_WIRE_H
#define PULSELINK_WIRE_H

#include "protocol.h"

/* Serialize header+payload into out[] in network byte order, filling in magic,
 * version, length and the payload checksum automatically.
 *   returns total bytes written (PL_HDR_SIZE + plen), or 0 if it won't fit. */
size_t pl_build(uint8_t *out, size_t outcap, pkt_type_t type,
                uint32_t device_id, const void *payload, uint16_t plen);

/* Decode a 12-byte header into host-order fields (no validation — the caller
 * checks magic/version/length/checksum and decides policy). */
void pl_parse_header(const uint8_t hdr[PL_HDR_SIZE], pl_header_t *out_host);

#endif /* PULSELINK_WIRE_H */
