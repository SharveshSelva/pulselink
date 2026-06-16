/* bmp.c — see bmp.h. */
#include "bmp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_HDR   14
#define DIB_HDR    40
#define PALETTE   (256 * 4)
#define PIX_OFFSET (FILE_HDR + DIB_HDR + PALETTE)   /* 1078 */

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t get_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int bmp_write(const image_t *img, const char *path)
{
    if (img->w <= 0 || img->h <= 0 || !img->pix)
        return -1;

    int      w      = img->w, h = img->h;
    uint32_t stride = ((uint32_t)w + 3u) & ~3u;          /* rows pad to 4 bytes */
    uint32_t pixsz  = stride * (uint32_t)h;
    uint32_t fsize  = PIX_OFFSET + pixsz;

    uint8_t hdr[PIX_OFFSET];
    memset(hdr, 0, sizeof hdr);

    /* BITMAPFILEHEADER */
    hdr[0] = 'B'; hdr[1] = 'M';
    put_le32(hdr + 2,  fsize);
    put_le32(hdr + 10, PIX_OFFSET);
    /* BITMAPINFOHEADER */
    put_le32(hdr + 14, DIB_HDR);
    put_le32(hdr + 18, (uint32_t)w);
    put_le32(hdr + 22, (uint32_t)h);          /* positive => bottom-up rows */
    put_le16(hdr + 26, 1);                     /* planes */
    put_le16(hdr + 28, 8);                     /* bits per pixel */
    put_le32(hdr + 30, 0);                     /* BI_RGB (uncompressed) */
    put_le32(hdr + 34, pixsz);
    put_le32(hdr + 46, 256);                   /* colors used */
    /* grayscale palette: entry i = (B,G,R,0) = (i,i,i,0) */
    for (int i = 0; i < 256; i++) {
        uint8_t *e = hdr + FILE_HDR + DIB_HDR + i * 4;
        e[0] = e[1] = e[2] = (uint8_t)i;
    }

    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;

    int rc = -1;
    if (fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr)
        goto done;

    {
        uint8_t pad[4] = {0, 0, 0, 0};
        size_t  padlen = stride - (uint32_t)w;
        /* BMP rows are stored bottom-to-top */
        for (int y = h - 1; y >= 0; y--) {
            if (fwrite(img->pix + (size_t)y * w, 1, (size_t)w, f) != (size_t)w)
                goto done;
            if (padlen && fwrite(pad, 1, padlen, f) != padlen)
                goto done;
        }
    }
    rc = 0;
done:
    fclose(f);
    return rc;
}

int bmp_read(image_t *img, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    int rc = -1;
    uint8_t fh[FILE_HDR + DIB_HDR];
    if (fread(fh, 1, sizeof fh, f) != sizeof fh) goto done;
    if (fh[0] != 'B' || fh[1] != 'M')            goto done;

    uint32_t offset = get_le32(fh + 10);
    int32_t  w      = (int32_t)get_le32(fh + 18);
    int32_t  h      = (int32_t)get_le32(fh + 22);
    uint16_t bpp    = get_le16(fh + 28);
    if (bpp != 8 || w <= 0 || h <= 0)            goto done;
    if (image_alloc(img, w, h) != 0)             goto done;

    if (fseek(f, (long)offset, SEEK_SET) != 0)   { image_free(img); goto done; }
    {
        uint32_t stride = ((uint32_t)w + 3u) & ~3u;
        uint8_t  pad[4];
        size_t   padlen = stride - (uint32_t)w;
        for (int y = h - 1; y >= 0; y--) {       /* stored bottom-up */
            if (fread(img->pix + (size_t)y * w, 1, (size_t)w, f) != (size_t)w) {
                image_free(img); goto done;
            }
            if (padlen && fread(pad, 1, padlen, f) != padlen) {
                image_free(img); goto done;
            }
        }
    }
    rc = 0;
done:
    fclose(f);
    return rc;
}
