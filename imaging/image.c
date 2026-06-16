/* image.c — see image.h. */
#include "image.h"

#include <stdlib.h>

#define IMAGE_MAX_PIXELS (1u << 22)   /* 4 Mpx cap: defends allocations */

int image_alloc(image_t *img, int w, int h)
{
    if (w <= 0 || h <= 0)
        return -1;
    if ((unsigned long long)w * (unsigned long long)h > IMAGE_MAX_PIXELS)
        return -1;
    img->w = w;
    img->h = h;
    img->pix = calloc((size_t)w * (size_t)h, 1);
    return img->pix ? 0 : -1;
}

void image_free(image_t *img)
{
    free(img->pix);
    img->pix = NULL;
    img->w = img->h = 0;
}

void image_test_pattern(image_t *img)
{
    int w = img->w, h = img->h;

    /* horizontal gradient background */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            img->pix[y * w + x] = (uint8_t)((x * 255) / (w > 1 ? w - 1 : 1));

    /* a bright filled box -> four sharp edges */
    int bx0 = w / 4, bx1 = w / 2, by0 = h / 4, by1 = (3 * h) / 4;
    for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++)
            img->pix[y * w + x] = 230;

    /* a dark diagonal line -> oblique edges */
    for (int t = 0; t < (w < h ? w : h); t++) {
        int x = (w - 1) - t, y = t;
        if (x >= 0 && x < w && y >= 0 && y < h)
            img->pix[y * w + x] = 20;
    }
}
