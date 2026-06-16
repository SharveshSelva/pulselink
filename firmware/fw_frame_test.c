/* fw_frame_test.c — host proof that firmware and server agree on the wire.
 *
 *  - builders (sensor/hello/heartbeat): build with FIRMWARE code, decode with
 *    the SERVER's pl_parse_header (../common/wire.c).
 *  - parser (pl_parse_header_fw): build a reply with the SERVER's pl_build,
 *    decode with FIRMWARE code, compare.
 * Build/run: `make test`.
 */
#define _GNU_SOURCE
#include "telemetry.h"
#include "protocol.h"
#include "../common/wire.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)){ fails++; \
    fprintf(stderr,"FAIL %s:%d ",__FILE__,__LINE__); \
    fprintf(stderr,__VA_ARGS__); fprintf(stderr,"\n"); } } while(0)

static void server_decodes_firmware(void)
{
    uint8_t pkt[64];
    pl_header_t h;

    /* sensor */
    size_t n = pl_build_sensor(pkt, sizeof pkt, 0x1001, 42, 12345, 2654, 4810);
    CHECK(n == PL_SENSOR_PACKET_SIZE, "sensor len %zu", n);
    pl_parse_header(pkt, &h);
    CHECK(h.type == PKT_SENSOR && h.device_id == 0x1001 && h.payload_len == 16,
          "sensor header");
    CHECK(pl_checksum(pkt + PL_HDR_SIZE, h.payload_len) == h.checksum, "sensor cksum");
    pl_sensor_payload_t s;
    memcpy(&s, pkt + PL_HDR_SIZE, sizeof s);
    CHECK(ntohl(s.seq) == 42 &&
          (int32_t)ntohl((uint32_t)s.temperature) == 2654 &&
          (int32_t)ntohl((uint32_t)s.humidity) == 4810, "sensor payload");

    /* negative temperature survives */
    n = pl_build_sensor(pkt, sizeof pkt, 0x2002, 7, 0, -512, 3300);
    pl_parse_header(pkt, &h);
    memcpy(&s, pkt + PL_HDR_SIZE, sizeof s);
    CHECK((int32_t)ntohl((uint32_t)s.temperature) == -512, "neg temp");

    /* hello */
    n = pl_build_hello(pkt, sizeof pkt, 0x1001, 0x00010002, 0x1);
    CHECK(n == PL_HELLO_PACKET_SIZE, "hello len %zu", n);
    pl_parse_header(pkt, &h);
    CHECK(h.type == PKT_HELLO && h.payload_len == 8, "hello header");
    CHECK(pl_checksum(pkt + PL_HDR_SIZE, 8) == h.checksum, "hello cksum");
    pl_hello_payload_t hello;
    memcpy(&hello, pkt + PL_HDR_SIZE, sizeof hello);
    CHECK(ntohl(hello.firmware_version) == 0x00010002 &&
          ntohl(hello.capabilities) == 0x1, "hello payload");

    /* heartbeat */
    n = pl_build_heartbeat(pkt, sizeof pkt, 0x1001, 60000, 180000);
    CHECK(n == PL_HEARTBEAT_PACKET_SIZE, "hb len %zu", n);
    pl_parse_header(pkt, &h);
    CHECK(h.type == PKT_HEARTBEAT && h.payload_len == 8, "hb header");
    pl_heartbeat_payload_t hb;
    memcpy(&hb, pkt + PL_HDR_SIZE, sizeof hb);
    CHECK(ntohl(hb.uptime_ms) == 60000 && ntohl(hb.free_heap) == 180000, "hb payload");

    /* undersized buffer is refused */
    uint8_t tiny[8];
    CHECK(pl_build_sensor(tiny, sizeof tiny, 1, 1, 1, 1, 1) == 0, "tiny refused");
}

static void firmware_decodes_server(void)
{
    uint8_t pkt[64];
    pl_rx_header_t rh;

    /* server builds HELLO_ACK(session_id) -> firmware must parse it */
    uint32_t sid_be = htonl(7);
    size_t n = pl_build(pkt, sizeof pkt, PKT_HELLO_ACK, 0x1001, &sid_be, sizeof sid_be);
    CHECK(n == PL_HDR_SIZE + 4, "ack len %zu", n);
    CHECK(pl_parse_header_fw(pkt, &rh) == 1, "fw parse hello_ack ok");
    CHECK(rh.type == PKT_HELLO_ACK && rh.device_id == 0x1001 && rh.payload_len == 4,
          "fw parsed hello_ack header");
    CHECK(pl_checksum(pkt + PL_HDR_SIZE, rh.payload_len) == rh.checksum, "fw verify cksum");
    CHECK(pl_rd_u32(pkt + PL_HDR_SIZE) == 7, "fw read session_id");

    /* server builds PKT_ERROR(code) */
    uint16_t code_be = htons(PL_ERR_BAD_CHECKSUM);
    n = pl_build(pkt, sizeof pkt, PKT_ERROR, 0x1001, &code_be, sizeof code_be);
    CHECK(pl_parse_header_fw(pkt, &rh) == 1 && rh.type == PKT_ERROR, "fw parse error");
    CHECK(pl_rd_u16(pkt + PL_HDR_SIZE) == PL_ERR_BAD_CHECKSUM, "fw read error code");

    /* corrupt magic -> firmware parser rejects */
    pkt[0] ^= 0xFF;
    CHECK(pl_parse_header_fw(pkt, &rh) == 0, "fw rejects bad magic");
}

int main(void)
{
    server_decodes_firmware();
    firmware_decodes_server();
    if (fails == 0) {
        printf("fw_frame_test: ALL PASS (firmware <-> server agree on the wire)\n");
        return 0;
    }
    printf("fw_frame_test: %d FAILURES\n", fails);
    return 1;
}
