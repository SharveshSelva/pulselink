/* bmp.h — hand-rolled 8-bit grayscale BMP I/O (no libpng/libjpeg).
 *
 * Writing a BMP by hand is the point: it shows the file format (a 14-byte file
 * header, a 40-byte DIB header, a 256-entry grayscale palette, then bottom-up
 * rows padded to a 4-byte boundary) rather than hiding behind a library. All
 * multi-byte fields are little-endian, written explicitly so it's correct on any
 * host — the mirror image of the big-endian wire code.
 */
#ifndef PULSELINK_BMP_H
#define PULSELINK_BMP_H

#include "image.h"

/* Write img as an 8-bit grayscale BMP. Returns 0 on success, -1 on error. */
int bmp_write(const image_t *img, const char *path);

/* Read an 8-bit grayscale BMP written by bmp_write (used by the round-trip
 * test). Returns 0 on success, -1 on error; caller image_free()s on success. */
int bmp_read(image_t *img, const char *path);

#endif /* PULSELINK_BMP_H */
