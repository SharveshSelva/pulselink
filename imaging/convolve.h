/* convolve.h — 3x3 spatial filters over grayscale images.
 *
 * Borders use edge-clamping (out-of-bounds samples replicate the nearest edge
 * pixel) so the output is the same size as the input with no dark frame.
 */
#ifndef PULSELINK_CONVOLVE_H
#define PULSELINK_CONVOLVE_H

#include "image.h"

/* Named 3x3 integer kernels (row-major). */
extern const int K_IDENTITY[9];
extern const int K_BLUR[9];      /* divisor 9  */
extern const int K_SHARPEN[9];   /* divisor 1  */

/* out = clamp((in (*) kernel) / div + bias). in and out must be same dims and
 * distinct buffers. Returns 0 on success, -1 on bad args. */
int conv3x3(const image_t *in, image_t *out, const int kernel[9], int div, int bias);

/* Sobel edge magnitude: out = clamp(sqrt(Gx^2 + Gy^2)). */
int sobel(const image_t *in, image_t *out);

#endif /* PULSELINK_CONVOLVE_H */
