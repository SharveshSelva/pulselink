/* image.h — a dirt-simple 8-bit grayscale image: row-major, pix[y*w + x]. */
#ifndef PULSELINK_IMAGE_H
#define PULSELINK_IMAGE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int      w;
    int      h;
    uint8_t *pix;   /* w*h bytes, row-major */
} image_t;

/* Allocate w*h pixels (zeroed). Returns 0 on success, -1 on failure/bad dims. */
int  image_alloc(image_t *img, int w, int h);
void image_free(image_t *img);

static inline size_t image_size(const image_t *img)
{
    return (size_t)img->w * (size_t)img->h;
}

/* Fill with a deterministic test pattern (gradient + a filled box + a diagonal)
 * so edge detection has obvious, checkable structure. */
void image_test_pattern(image_t *img);

#endif /* PULSELINK_IMAGE_H */
