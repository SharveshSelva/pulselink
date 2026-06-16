/* convolve.c — see convolve.h. */
#include "convolve.h"

#include <math.h>

const int K_IDENTITY[9] = { 0, 0, 0,
                            0, 1, 0,
                            0, 0, 0 };
const int K_BLUR[9]     = { 1, 1, 1,
                            1, 1, 1,
                            1, 1, 1 };
const int K_SHARPEN[9]  = {  0, -1,  0,
                            -1,  5, -1,
                             0, -1,  0 };

static const int SOBEL_X[9] = { -1, 0, 1,
                                -2, 0, 2,
                                -1, 0, 1 };
static const int SOBEL_Y[9] = { -1, -2, -1,
                                 0,  0,  0,
                                 1,  2,  1 };

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* sample with edge clamping */
static inline int at(const image_t *im, int x, int y)
{
    x = clampi(x, 0, im->w - 1);
    y = clampi(y, 0, im->h - 1);
    return im->pix[(size_t)y * im->w + x];
}

int conv3x3(const image_t *in, image_t *out, const int kernel[9], int div, int bias)
{
    if (!in->pix || !out->pix || in->w != out->w || in->h != out->h || div == 0)
        return -1;

    for (int y = 0; y < in->h; y++) {
        for (int x = 0; x < in->w; x++) {
            int acc = 0, ki = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    acc += kernel[ki++] * at(in, x + dx, y + dy);
            acc = acc / div + bias;
            out->pix[(size_t)y * in->w + x] = (uint8_t)clampi(acc, 0, 255);
        }
    }
    return 0;
}

int sobel(const image_t *in, image_t *out)
{
    if (!in->pix || !out->pix || in->w != out->w || in->h != out->h)
        return -1;

    for (int y = 0; y < in->h; y++) {
        for (int x = 0; x < in->w; x++) {
            int gx = 0, gy = 0, ki = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int p = at(in, x + dx, y + dy);
                    gx += SOBEL_X[ki] * p;
                    gy += SOBEL_Y[ki] * p;
                    ki++;
                }
            }
            int mag = (int)lround(sqrt((double)gx * gx + (double)gy * gy));
            out->pix[(size_t)y * in->w + x] = (uint8_t)clampi(mag, 0, 255);
        }
    }
    return 0;
}
