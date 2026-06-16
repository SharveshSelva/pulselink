/* checksum.c — RFC 1071 ones'-complement Internet checksum.
 *
 * Reads the buffer as a sequence of 16-bit big-endian words, accumulates
 * into a 32-bit sum, folds the carries back in, then takes the ones'
 * complement. An odd trailing byte is padded with a zero low byte (the
 * standard RFC 1071 treatment).
 *
 * Why this algorithm: it is cheap (add + a couple of folds), endian-defined,
 * detects all single-bit errors and most burst errors, and has the tidy
 * self-verifying property exploited in the tests. It is NOT cryptographic —
 * it guards against line noise / truncation, not tampering.
 */
#include "protocol.h"

uint16_t pl_checksum(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;
    size_t i = 0;

    /* sum complete 16-bit big-endian words */
    for (; i + 1 < len; i += 2) {
        uint16_t word = (uint16_t)(((uint16_t)data[i] << 8) | data[i + 1]);
        sum += word;
    }

    /* trailing odd byte: pad on the right with 0x00 */
    if (i < len) {
        uint16_t word = (uint16_t)((uint16_t)data[i] << 8);
        sum += word;
    }

    /* fold the carry bits back into the low 16 bits until none remain */
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);

    return (uint16_t)(~sum);
}
