/* checksum_test.c — unit tests for pl_checksum (M1 deliverable).
 *
 * Build: gcc -Wall -Wextra -Werror -std=c11 checksum.c checksum_test.c -o checksum_test
 * Run:   ./checksum_test    (exit 0 = all pass)
 */
#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>   /* htons — only the test/server use this, not protocol.h */

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d  ", __FILE__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                    \
            fprintf(stderr, "\n");                                           \
        }                                                                    \
    } while (0)

/* Verify helper as the server would use it: recompute, compare to stored. */
static int verify(const uint8_t *payload, size_t len, uint16_t wire_checksum)
{
    return pl_checksum(payload, len) == ntohs(wire_checksum);
}

/* ---- test 1: empty payload ---------------------------------------------- */
static void test_empty(void)
{
    /* sum is 0, ~0 == 0xFFFF */
    CHECK(pl_checksum(NULL, 0) == 0xFFFF, "empty payload should be 0xFFFF");
}

/* ---- test 2: a known, hand-checkable vector ----------------------------- */
static void test_known_vector(void)
{
    /* one 16-bit word 0x0001 -> sum 0x0001 -> ~ == 0xFFFE */
    uint8_t a[2] = { 0x00, 0x01 };
    CHECK(pl_checksum(a, 2) == 0xFFFE, "0x0001 -> 0xFFFE, got 0x%04X",
          pl_checksum(a, 2));

    /* two words 0xFFFF + 0x0001 = 0x10000 -> fold -> 0x0001 -> ~ == 0xFFFE */
    uint8_t b[4] = { 0xFF, 0xFF, 0x00, 0x01 };
    CHECK(pl_checksum(b, 4) == 0xFFFE, "carry-fold case, got 0x%04X",
          pl_checksum(b, 4));

    /* all ones, two words: 0xFFFF+0xFFFF=0x1FFFE -> fold 0xFFFF -> ~ ==0 */
    uint8_t c[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK(pl_checksum(c, 4) == 0x0000, "all-ones -> 0, got 0x%04X",
          pl_checksum(c, 4));
}

/* ---- test 3: odd-length payload (zero pad) ------------------------------ */
static void test_odd_length(void)
{
    /* single byte 0xAB padded to word 0xAB00 -> ~ == 0x54FF */
    uint8_t a[1] = { 0xAB };
    CHECK(pl_checksum(a, 1) == (uint16_t)~0xAB00, "odd pad, got 0x%04X",
          pl_checksum(a, 1));

    /* three bytes: 0x1234 + 0x5600 = 0x6834 -> ~ == 0x97CB */
    uint8_t b[3] = { 0x12, 0x34, 0x56 };
    CHECK(pl_checksum(b, 3) == (uint16_t)~0x6834, "odd 3B, got 0x%04X",
          pl_checksum(b, 3));
}

/* ---- test 4: the self-verifying property -------------------------------- *
 * If c = checksum(payload), then summing the payload AND c together and
 * complementing yields 0. This is the same trick the IP stack uses to verify
 * a header in one pass. We append c as a big-endian word and recompute.      */
static void test_self_verify(void)
{
    uint8_t payload[16];
    for (int i = 0; i < 16; i++) payload[i] = (uint8_t)(i * 17 + 3);

    uint16_t c = pl_checksum(payload, sizeof payload);

    uint8_t with_ck[18];
    memcpy(with_ck, payload, 16);
    with_ck[16] = (uint8_t)(c >> 8);   /* big-endian append */
    with_ck[17] = (uint8_t)(c & 0xFF);

    CHECK(pl_checksum(with_ck, sizeof with_ck) == 0x0000,
          "self-verify must be 0, got 0x%04X",
          pl_checksum(with_ck, sizeof with_ck));
}

/* ---- test 5: realistic sensor payload round-trips ----------------------- */
static void test_sensor_roundtrip(void)
{
    pl_sensor_payload_t s;
    s.seq          = htonl(42);
    s.timestamp_ms = htonl(123456);
    s.temperature  = (int32_t)htonl((uint32_t)2654);   /* 26.54 C */
    s.humidity     = (int32_t)htonl((uint32_t)4810);   /* 48.10 % */

    uint16_t c = pl_checksum((const uint8_t *)&s, sizeof s);
    uint16_t wire = htons(c);

    CHECK(verify((const uint8_t *)&s, sizeof s, wire),
          "sensor payload should verify");

    /* flip one bit in the payload -> verification must fail */
    ((uint8_t *)&s)[7] ^= 0x01;
    CHECK(!verify((const uint8_t *)&s, sizeof s, wire),
          "single-bit corruption must be detected");
}

/* ---- test 6: bit-flip detection over many random payloads --------------- */
static void test_bitflip_detection(void)
{
    srand(0xC0FFEE);
    int missed = 0;
    const int trials = 20000;

    for (int t = 0; t < trials; t++) {
        uint8_t buf[16];
        for (int i = 0; i < 16; i++) buf[i] = (uint8_t)rand();

        uint16_t good = pl_checksum(buf, sizeof buf);

        int byte = rand() % 16;
        int bit  = rand() % 8;
        buf[byte] ^= (uint8_t)(1u << bit);

        if (pl_checksum(buf, sizeof buf) == good)
            missed++;   /* a single-bit flip slipped past — must never happen */
    }
    CHECK(missed == 0, "single-bit flips missed: %d / %d", missed, trials);
}

int main(void)
{
    test_empty();
    test_known_vector();
    test_odd_length();
    test_self_verify();
    test_sensor_roundtrip();
    test_bitflip_detection();

    printf("checksum tests: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    return 1;
}
