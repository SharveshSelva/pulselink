/* img_test.c — host tests for the imaging stage. `make test`. */
#include "image.h"
#include "bmp.h"
#include "convolve.h"
#include "frame.h"
#include "protocol.h"
#include "wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)){ fails++; \
    fprintf(stderr,"FAIL %s:%d ",__FILE__,__LINE__); \
    fprintf(stderr,__VA_ARGS__); fprintf(stderr,"\n"); } } while(0)

static void test_bmp_roundtrip(void)
{
    image_t a, b;
    CHECK(image_alloc(&a, 37, 19) == 0, "alloc");   /* odd width -> exercises padding */
    image_test_pattern(&a);

    const char *path = "/tmp/_pl_rt.bmp";
    CHECK(bmp_write(&a, path) == 0, "bmp_write");
    CHECK(bmp_read(&b, path) == 0, "bmp_read");
    CHECK(a.w == b.w && a.h == b.h, "dims %dx%d vs %dx%d", a.w, a.h, b.w, b.h);
    CHECK(memcmp(a.pix, b.pix, image_size(&a)) == 0, "pixels survive round-trip");

    remove(path);
    image_free(&a);
    image_free(&b);
}

static void test_identity(void)
{
    image_t a, o;
    image_alloc(&a, 24, 24); image_alloc(&o, 24, 24);
    image_test_pattern(&a);
    CHECK(conv3x3(&a, &o, K_IDENTITY, 1, 0) == 0, "conv");
    CHECK(memcmp(a.pix, o.pix, image_size(&a)) == 0, "identity kernel is a no-op");
    image_free(&a); image_free(&o);
}

static void test_sobel_edge(void)
{
    image_t a, o;
    image_alloc(&a, 16, 8); image_alloc(&o, 16, 8);
    for (int y = 0; y < 8; y++)               /* sharp vertical edge at x=8 */
        for (int x = 0; x < 16; x++)
            a.pix[y * 16 + x] = (x < 8) ? 0 : 255;

    CHECK(sobel(&a, &o) == 0, "sobel");
    int row = 4 * 16;
    CHECK(o.pix[row + 2]  == 0,   "flat-left ~0, got %u", o.pix[row + 2]);
    CHECK(o.pix[row + 13] == 0,   "flat-right ~0, got %u", o.pix[row + 13]);
    CHECK(o.pix[row + 7]  > 200,  "edge strong, got %u", o.pix[row + 7]);
    image_free(&a); image_free(&o);
}

/* chunk an image into PKT_FRAME packets, parse each with the SERVER's
 * pl_parse_header, verify the checksum, reassemble, and compare. */
static void test_frame_roundtrip(void)
{
    image_t src, got;
    image_alloc(&src, 50, 40);               /* 2000 bytes -> 2 chunks */
    image_test_pattern(&src);

    uint32_t total = (uint32_t)src.w * src.h;
    uint16_t ccount = frame_chunk_count(total);
    CHECK(ccount == 2, "expected 2 chunks, got %u", ccount);

    frame_asm_t fa; frame_asm_init(&fa);
    int completed = 0;

    for (uint16_t i = 0; i < ccount; i++) {
        uint32_t off = (uint32_t)i * FRAME_CHUNK_MAX;
        uint32_t len = total - off; if (len > FRAME_CHUNK_MAX) len = FRAME_CHUNK_MAX;

        uint8_t pkt[PL_HDR_SIZE + 12 + FRAME_CHUNK_MAX];
        size_t n = frame_chunk_build(pkt, sizeof pkt, 0x1001,
                                     (uint16_t)src.w, (uint16_t)src.h, 7,
                                     i, ccount, src.pix + off, (uint16_t)len);
        CHECK(n == (size_t)PL_HDR_SIZE + 12 + len, "chunk %u size", i);

        pl_header_t h;
        pl_parse_header(pkt, &h);             /* server's decoder */
        CHECK(h.type == PKT_FRAME, "type");
        CHECK(pl_checksum(pkt + PL_HDR_SIZE, h.payload_len) == h.checksum, "checksum");

        int r = frame_asm_feed(&fa, pkt + PL_HDR_SIZE, h.payload_len, &got);
        CHECK(r >= 0, "feed chunk %u not malformed", i);
        if (r == 1) {
            completed = 1;
            CHECK(i == ccount - 1, "completes on last chunk");
            CHECK(got.w == src.w && got.h == src.h, "reassembled dims");
            CHECK(memcmp(got.pix, src.pix, total) == 0, "reassembled pixels identical");
            image_free(&got);
        }
    }
    CHECK(completed, "frame completed");

    /* a malformed chunk (bad chunk_count) must be rejected */
    uint8_t bad[PL_HDR_SIZE + 12 + 4];
    uint8_t z[4] = {0};
    frame_chunk_build(bad, sizeof bad, 1, 50, 40, 9, 0, 99 /*wrong*/, z, 4);
    frame_asm_t fb; frame_asm_init(&fb);
    image_t tmp;
    CHECK(frame_asm_feed(&fb, bad + PL_HDR_SIZE, 12 + 4, &tmp) == -1,
          "inconsistent chunk_count rejected");
    frame_asm_reset(&fb);

    image_free(&src);
}

int main(void)
{
    test_bmp_roundtrip();
    test_identity();
    test_sobel_edge();
    test_frame_roundtrip();
    if (fails == 0) { printf("img_test: ALL PASS\n"); return 0; }
    printf("img_test: %d FAILURES\n", fails);
    return 1;
}
