#ifndef AURIS_NV12_H
#define AURIS_NV12_H

#include "auris_image.h"

/* An NV12 picture, as the chip's image processor gives it: a plane of
   brightness (Y) samples, then a plane of interleaved Cb Cr pairs with one
   pair for each 2 x 2 block. BT.601 video range (Y 16..235). Strides are in
   bytes; width and height are even. */
typedef struct {
  const uint8_t *y, *uv;
  uint32_t width, height;
  size_t y_stride, uv_stride;
} auris_nv12_t;

/* Draws the picture into the canvas with its top-left corner at (x, y). The
   rest of the canvas is left as it is. -EINVAL if it does not fit. */
int auris_nv12_to_rgb24(const auris_nv12_t *picture,
                        const auris_rgb24_t *canvas, uint32_t x, uint32_t y);

/* Mean brightness, 0 (black) to 1 (white) of the video range. */
int auris_nv12_mean(const auris_nv12_t *picture, float *mean);

#endif
