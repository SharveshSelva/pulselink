/* pl_imaging.c — demo the Stage 4 imaging path end to end.
 *
 *   pl_imaging [out_dir]      (default ".")
 *
 * Synthesizes a grayscale frame, writes it and three filtered versions as BMPs,
 * and round-trips the frame through PKT_FRAME chunking to show the wire path.
 */
#include "image.h"
#include "bmp.h"
#include "convolve.h"
#include "frame.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_one(const image_t *img, const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (bmp_write(img, path) != 0) { fprintf(stderr, "write %s failed\n", path); return -1; }
    printf("  wrote %s (%dx%d)\n", path, img->w, img->h);
    return 0;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : ".";

    image_t src, edges, blur, sharp;
    if (image_alloc(&src, 192, 144) != 0) return 1;
    image_alloc(&edges, 192, 144);
    image_alloc(&blur,  192, 144);
    image_alloc(&sharp, 192, 144);
    image_test_pattern(&src);

    sobel(&src, &edges);
    conv3x3(&src, &blur,  K_BLUR, 9, 0);
    conv3x3(&src, &sharp, K_SHARPEN, 1, 0);

    printf("imaging demo -> %s:\n", dir);
    write_one(&src,   dir, "frame_input.bmp");
    write_one(&edges, dir, "frame_edges.bmp");
    write_one(&blur,  dir, "frame_blur.bmp");
    write_one(&sharp, dir, "frame_sharpen.bmp");

    /* round-trip the source frame through PKT_FRAME chunking */
    uint32_t total  = (uint32_t)src.w * src.h;
    uint16_t ccount = frame_chunk_count(total);
    frame_asm_t fa; frame_asm_init(&fa);
    image_t recon; int done = 0;

    for (uint16_t i = 0; i < ccount; i++) {
        uint32_t off = (uint32_t)i * FRAME_CHUNK_MAX;
        uint32_t len = total - off; if (len > FRAME_CHUNK_MAX) len = FRAME_CHUNK_MAX;
        uint8_t pkt[PL_HDR_SIZE + 12 + FRAME_CHUNK_MAX];
        size_t n = frame_chunk_build(pkt, sizeof pkt, 0x1001,
                                     (uint16_t)src.w, (uint16_t)src.h, 1,
                                     i, ccount, src.pix + off, (uint16_t)len);
        (void)n;
        if (frame_asm_feed(&fa, pkt + PL_HDR_SIZE, (uint16_t)(12 + len), &recon) == 1)
            done = 1;
    }
    printf("  frame chunked into %u PKT_FRAME packets and reassembled: %s\n",
           ccount, (done && memcmp(recon.pix, src.pix, total) == 0) ? "OK" : "MISMATCH");
    if (done) image_free(&recon);

    image_free(&src); image_free(&edges); image_free(&blur); image_free(&sharp);
    return 0;
}
