/* wire.c — see wire.h */
#define _GNU_SOURCE
#include "wire.h"

#include <string.h>
#include <arpa/inet.h>   /* htons/htonl/ntohs/ntohl */

size_t pl_build(uint8_t *out, size_t outcap, pkt_type_t type,
                uint32_t device_id, const void *payload, uint16_t plen)
{
    size_t total = (size_t)PL_HDR_SIZE + plen;
    if (total > outcap)
        return 0;

    pl_header_t h;
    h.magic       = htons(PL_MAGIC);
    h.version     = PL_VERSION;
    h.type        = (uint8_t)type;
    h.device_id   = htonl(device_id);
    h.payload_len = htons(plen);
    h.checksum    = htons(pl_checksum((const uint8_t *)payload, plen));

    /* packed struct is exactly PL_HDR_SIZE bytes (enforced by _Static_assert) */
    memcpy(out, &h, PL_HDR_SIZE);
    if (plen && payload)
        memcpy(out + PL_HDR_SIZE, payload, plen);
    return total;
}

void pl_parse_header(const uint8_t hdr[PL_HDR_SIZE], pl_header_t *o)
{
    pl_header_t raw;
    memcpy(&raw, hdr, PL_HDR_SIZE);
    o->magic       = ntohs(raw.magic);
    o->version     = raw.version;
    o->type        = raw.type;
    o->device_id   = ntohl(raw.device_id);
    o->payload_len = ntohs(raw.payload_len);
    o->checksum    = ntohs(raw.checksum);
}
